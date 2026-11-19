// The network probe: a short connection on its own port, before the stream,
// that measures the round trip and the largest packet that passes each way.
// docs/protocol/probe.md describes it. No sockets and no clock in here: the
// caller passes datagrams and the time in and sends what comes out.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "proto/stream_message.h"
#include "proto/transport.h"

namespace psvp::proto::transport {

// Sizes are those of whole IP packets; a datagram is this much smaller.
inline constexpr std::uint32_t kIpUdpOverhead = 28;
inline constexpr std::uint32_t kMtuFloor = 576;
inline constexpr std::uint32_t kMtuCeiling = 1454;

struct ProbeResult {
    std::chrono::microseconds round_trip{1000};
    std::uint32_t mtu_in = kMtuCeiling;   // console to client
    std::uint32_t mtu_out = kMtuCeiling;  // client to console
    friend bool operator==(const ProbeResult&, const ProbeResult&) = default;
};

using Datagrams = std::vector<std::vector<std::uint8_t>>;

// The client's side, from a finished handshake to the goodbye.
class ClientProbe {
public:
    enum class State { idle, running, done, failed };

    static constexpr std::uint32_t kProtocolVersion = 9;
    static constexpr int kPings = 10;
    static constexpr std::size_t kPingSize = 0x224;
    static constexpr int kSizeTries = 3;
    static constexpr std::chrono::milliseconds kAnswerWait{5000};
    static constexpr std::chrono::milliseconds kPongWait{1000};
    static constexpr std::chrono::milliseconds kSizeWaitFloor{20};
    static constexpr std::chrono::milliseconds kSizeWaitCeiling{500};

    // `random` gives the tags that tell one ping from another.
    ClientProbe(std::uint32_t local_tag, const Endpoint& console, std::function<std::uint32_t()> random);

    [[nodiscard]] Datagrams start(TimePoint now);
    [[nodiscard]] Datagrams on_datagram(std::span<const std::uint8_t> datagram, TimePoint now);
    // To be called when deadline() has passed.
    [[nodiscard]] Datagrams on_timeout(TimePoint now);

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] TimePoint deadline() const noexcept;
    // What was measured; the defaults of ProbeResult until the probe is done.
    [[nodiscard]] const ProbeResult& result() const noexcept { return result_; }
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    [[nodiscard]] int pongs() const noexcept { return pongs_; }

private:
    enum class Step { none, protocol, big, echo_on, ping, echo_off, size_in, client_size_begin, size_out, client_size_end };

    void send_message(Datagrams& out, const StreamMessage& message, std::uint16_t channel, TimePoint now, bool await_ack);
    void send_ping(Datagrams& out, TimePoint now);
    void send_size_request(Datagrams& out, TimePoint now);
    void send_size_ping(Datagrams& out, TimePoint now);
    void begin_search(std::uint32_t first);
    // Moves the search on after a size passed or did not; false when it has ended (its answer is low_).
    bool narrow(bool passed);
    void on_message(Datagrams& out, const StreamMessage& message, TimePoint now);
    void on_acknowledged(Datagrams& out, TimePoint now);
    void on_media(Datagrams& out, const MediaPacket& packet, TimePoint now);
    void after_size_in(Datagrams& out, TimePoint now);
    void after_size_out(Datagrams& out, TimePoint now);
    void fail(std::string why);

    Messenger messenger_;
    std::function<std::uint32_t()> random_;
    State state_ = State::idle;
    Step step_ = Step::none;
    TimePoint step_deadline_{};
    std::optional<std::uint32_t> awaited_ack_;
    ProbeResult result_;
    std::string failure_;

    int ping_index_ = 0;
    std::uint32_t ping_tag_ = 0;
    TimePoint ping_sent_{};
    std::chrono::microseconds round_trip_total_{0};
    int pongs_ = 0;

    std::chrono::milliseconds size_wait_{kSizeWaitFloor};
    std::uint32_t low_ = kMtuFloor;
    std::uint32_t high_ = kMtuCeiling;
    std::uint32_t current_ = kMtuCeiling;
    int attempt_ = 0;
    bool ceiling_tried_ = false;
    std::uint32_t request_id_ = 0;
    std::uint32_t first_id_of_size_ = 0;                    // the size requests for current_ are first_id_of_size_ .. request_id_
    std::array<std::uint32_t, kSizeTries> size_ping_tags_{}; // the tags of the pings sent for current_
};

// The console's side, as the mock console and the tests play it.
class ConsoleProbe {
public:
    // The largest IP packet that survives the way in each direction.
    struct Path {
        std::uint32_t mtu_in = kMtuCeiling;   // console to client
        std::uint32_t mtu_out = kMtuCeiling;  // client to console
    };

    ConsoleProbe(std::uint32_t local_tag, const Endpoint& client, Path path);

    [[nodiscard]] Datagrams on_datagram(std::span<const std::uint8_t> datagram, TimePoint now);
    [[nodiscard]] Datagrams on_timer(TimePoint now);
    [[nodiscard]] std::optional<TimePoint> deadline() const { return messenger_.deadline(); }

    // One line per thing the client did, in order; taking them empties the list.
    [[nodiscard]] std::vector<std::string> take_events();
    [[nodiscard]] bool disconnected() const noexcept { return disconnected_; }

private:
    void on_message(Datagrams& out, const DataChunk& chunk, std::uint8_t flags, TimePoint now);

    Messenger messenger_;
    Path path_;
    bool echo_ = false;
    bool client_size_test_ = false;
    bool disconnected_ = false;
    std::uint16_t next_picture_index_ = 0;
    std::vector<std::string> events_;
};

}  // namespace psvp::proto::transport
