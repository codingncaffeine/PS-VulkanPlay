// Setting a stream up on its own connection: the client's opening message with
// the launch specification and its public key, the console's answer with its
// key, the keys both sides make from them, and the stream information.
// docs/protocol/stream-setup.md describes it. No sockets and no clock in
// here: the caller passes datagrams and the time in and sends what comes out.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "proto/key_exchange.h"
#include "proto/probe.h"
#include "proto/rp_cipher.h"
#include "proto/session.h"
#include "proto/stream_message.h"
#include "proto/transport.h"

namespace psvp::proto::transport {

// The version of the stream's protocol each family is spoken to in.
[[nodiscard]] constexpr std::uint32_t stream_version(ConsoleKind kind) noexcept { return kind == ConsoleKind::ps5 ? 12 : 9; }

// How much smaller than the path's largest packet a datagram of a split message is kept.
inline constexpr std::uint32_t kSplitMargin = 50;

struct VideoProfile {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t max_fps = 60;
    std::uint32_t bitrate_kbps = 10000;
    Codec codec = Codec::h264;
};

struct StreamRequest {
    ConsoleKind kind = ConsoleKind::ps5;
    std::string session_id;        // names the session in the opening message
    RpCipher session_cipher;       // masks the launch specification
    ProbeResult network;           // what the probe measured, or its defaults
    VideoProfile video;
    bool dualsense = false;        // the controller the console is told about
};

// What the console's answer to the opening message showed.
struct BangFacts {
    enum class Code { none, valid, invalid, unchecked };

    std::uint32_t server_version = 0;
    bool signature_matches = false;   // the signature of its public key, checked with the handshake key
    Code code = Code::unchecked;      // the authentication code of the packet that carried the answer
    std::size_t session_key_size = 0;
    bool session_key_is_ours = false; // it repeated the session ID the client sent
    std::uint16_t chunks = 1;
};

struct StreamCounters {
    std::uint64_t video_packets = 0;      // with the right authentication code
    std::uint64_t audio_packets = 0;
    std::uint64_t media_rejected = 0;     // with a wrong one
    std::uint64_t media_before_keys = 0;  // arrived before there was a key to check them with
    std::uint64_t other_datagrams = 0;    // of a kind this version does not read
    std::uint64_t heartbeats_sent = 0;
    std::uint64_t split_messages = 0;     // messages from the console that came in more than one chunk
    std::map<std::uint32_t, std::uint64_t> messages;    // from the console after the set-up, by message type
    std::map<std::uint8_t, std::uint64_t> other_data;   // data that is not a message (rumble and the like), by kind
};

// The client's side, from a finished handshake on the stream port.
class ClientStream {
public:
    enum class State { idle, awaiting_bang, awaiting_stream_info, ready, closed, failed };

    static constexpr std::chrono::milliseconds kAnswerWait{5000};
    static constexpr std::chrono::milliseconds kHeartbeatEvery{1000};
    static constexpr std::uint16_t kSessionChannel = 1;
    static constexpr std::uint16_t kStreamInfoAckChannel = 9;

    ClientStream(std::uint32_t local_tag, const Endpoint& console, StreamRequest request, const KeyExchange& keys,
                 const Key16& handshake_key);

    [[nodiscard]] Datagrams start(TimePoint now);
    [[nodiscard]] Datagrams on_datagram(std::span<const std::uint8_t> datagram, TimePoint now);
    // To be called when deadline() has passed: sends again, gives up, or sends the next heartbeat.
    [[nodiscard]] Datagrams on_timeout(TimePoint now);
    [[nodiscard]] TimePoint deadline() const;
    // Says goodbye. The state becomes closed; the goodbye is sent again until the console has it.
    [[nodiscard]] Datagrams disconnect(TimePoint now);
    [[nodiscard]] bool goodbye_pending() const noexcept { return goodbye_ && messenger_.waiting() > 0; }

    [[nodiscard]] State state() const noexcept { return state_; }
    // Why it failed, or the reason the console gave when it closed the stream.
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }
    [[nodiscard]] bool closed_by_console() const noexcept { return closed_by_console_; }
    [[nodiscard]] const std::optional<BangFacts>& bang() const noexcept { return bang_; }
    [[nodiscard]] const std::optional<StreamInfo>& info() const noexcept { return info_; }
    [[nodiscard]] const StreamCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] std::uint64_t control_rejected() const noexcept { return messenger_.rejected(); }
    // The opening message: its size and how many datagrams it went out in.
    [[nodiscard]] std::size_t opening_size() const noexcept { return opening_size_; }
    [[nodiscard]] std::size_t opening_datagrams() const noexcept { return opening_datagrams_; }

private:
    void send(Datagrams& out, const StreamMessage& message, std::uint16_t channel, TimePoint now);
    void handle(Datagrams& out, Messenger::Received& received, TimePoint now);
    void on_message(Datagrams& out, const DataChunk& chunk, const StreamMessage& message, TimePoint now);
    void on_bang(Datagrams& out, const DataChunk& chunk, const BangMessage& bang, TimePoint now);
    void on_stream_info(Datagrams& out, const StreamInfo& info, TimePoint now);
    void fail(std::string why);

    Messenger messenger_;
    StreamRequest request_;
    KeyExchange keys_;
    Key16 handshake_key_;
    std::size_t max_datagram_;
    State state_ = State::idle;
    TimePoint step_deadline_{};
    TimePoint next_heartbeat_{};
    std::string reason_;
    bool closed_by_console_ = false;
    bool goodbye_ = false;
    std::shared_ptr<ReceiveKeys> receive_keys_;
    std::optional<BangFacts> bang_;
    std::optional<StreamInfo> info_;
    std::optional<StreamInfo> early_info_;
    StreamCounters counters_;
    std::size_t opening_size_ = 0;
    std::size_t opening_datagrams_ = 0;
};

// The console's side, as the mock console and the tests play it.
class ConsoleStream {
public:
    struct Options {
        ConsoleKind kind = ConsoleKind::ps5;
        RpCipher session_cipher;
        std::string session_id;                    // the one it handed out on the control channel
        std::vector<StreamResolution> resolutions;
        AudioHeader audio;
        std::size_t max_datagram = kMtuCeiling - kSplitMargin;
        bool accept_version = true;
        bool wrong_signature = false;              // sign its public key with a key that is not the handshake key
        std::chrono::milliseconds media_every{0};  // send a sealed sound packet this often; 0: none
        bool corrupt_media = false;                // change a byte of each after sealing it
    };

    ConsoleStream(std::uint32_t local_tag, const Endpoint& client, Options options, const KeyExchange& keys);

    [[nodiscard]] Datagrams on_datagram(std::span<const std::uint8_t> datagram, TimePoint now);
    [[nodiscard]] Datagrams on_timer(TimePoint now);
    [[nodiscard]] std::optional<TimePoint> deadline() const;
    // Closes the stream from the console's side, with a reason.
    [[nodiscard]] Datagrams disconnect(const std::string& reason, TimePoint now);

    // One line per thing the client did, in order; taking them empties the list.
    [[nodiscard]] std::vector<std::string> take_events();
    // One line that counts the packets that are not messages, and tells whether the
    // key positions of everything the client sealed follow one another as its sizes say.
    [[nodiscard]] std::string summary() const;
    [[nodiscard]] bool keyed() const noexcept { return messenger_.keyed(); }
    [[nodiscard]] bool disconnected() const noexcept { return disconnected_; }

private:
    void send(Datagrams& out, const StreamMessage& message, std::uint16_t channel, TimePoint now);
    void on_message(Datagrams& out, const DataChunk& chunk, TimePoint now);
    void on_big(Datagrams& out, const DataChunk& chunk, const BigMessage& big, TimePoint now);

    Messenger messenger_;
    Options options_;
    KeyExchange keys_;
    std::shared_ptr<SendKeys> send_keys_;
    std::shared_ptr<ReceiveKeys> receive_keys_;
    bool disconnected_ = false;
    std::optional<TimePoint> next_media_;
    std::uint16_t media_index_ = 0;
    std::uint64_t heartbeats_ = 0;
    std::map<std::uint8_t, std::uint64_t> authentic_;  // packets that are not messages, by kind
    std::map<std::uint8_t, std::uint64_t> rejected_;
    std::uint64_t before_keys_ = 0;
    // The key position of each authentic packet from the client, and the size it counts for.
    std::map<std::uint64_t, std::size_t> positions_;
    bool positions_checkable_ = true;
    std::vector<std::string> events_;
};

}  // namespace psvp::proto::transport
