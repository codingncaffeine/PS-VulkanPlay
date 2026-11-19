// The stream transport over UDP: control packets and the opening handshake.
// docs/protocol/transport.md describes it. Nothing here touches a socket or a
// clock; the caller passes datagrams and the time in and sends what comes out.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace psvp::proto::transport {

using TimePoint = std::chrono::steady_clock::time_point;

inline constexpr std::uint16_t kProbePort = 9297;   // the short connection that measures the network
inline constexpr std::uint16_t kStreamPort = 9296;  // the stream itself

namespace packet_kind {
inline constexpr std::uint8_t kControl = 0;
inline constexpr std::uint8_t kInputHistory = 1;
inline constexpr std::uint8_t kVideo = 2;
inline constexpr std::uint8_t kAudio = 3;
inline constexpr std::uint8_t kHandshake = 4;
inline constexpr std::uint8_t kCongestion = 5;
inline constexpr std::uint8_t kInputState = 6;
inline constexpr std::uint8_t kClientInfo = 8;
}  // namespace packet_kind

namespace chunk {
inline constexpr std::uint8_t kData = 0;
inline constexpr std::uint8_t kInit = 1;
inline constexpr std::uint8_t kInitAck = 2;
inline constexpr std::uint8_t kDataAck = 3;
inline constexpr std::uint8_t kCookie = 10;
inline constexpr std::uint8_t kCookieAck = 11;
}  // namespace chunk

inline constexpr std::size_t kCookieSize = 32;
inline constexpr std::uint32_t kReceiveWindow = 0x19000;
inline constexpr std::uint16_t kStreams = 100;

struct ControlPacket {
    std::uint32_t tag = 0;               // the receiving side's tag
    std::array<std::uint8_t, 4> mac{};   // zero until the stream keys exist
    std::uint32_t key_position = 0;
    std::uint8_t chunk_type = 0;
    std::uint8_t chunk_flags = 0;
    std::vector<std::uint8_t> payload;
};

[[nodiscard]] std::vector<std::uint8_t> encode_control(const ControlPacket& packet);

// Nothing for a datagram that is not a control packet or whose size field does not match its length.
[[nodiscard]] std::optional<ControlPacket> parse_control(std::span<const std::uint8_t> datagram);

// The five numbers each side announces in the handshake.
struct Endpoint {
    std::uint32_t tag = 0;
    std::uint32_t receive_window = kReceiveWindow;
    std::uint16_t outbound_streams = kStreams;
    std::uint16_t inbound_streams = kStreams;
    std::uint32_t first_sequence = 0;
};

// The client's side of the opening handshake.
class ClientHandshake {
public:
    enum class State { idle, awaiting_init_ack, awaiting_cookie_ack, connected, failed };

    // `tag` is this side's random tag; it is also its first sequence number.
    explicit ClientHandshake(std::uint32_t tag);

    // Each of these returns the datagram to send now, if there is one.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> start(TimePoint now);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> on_datagram(std::span<const std::uint8_t> datagram, TimePoint now);
    // To be called when deadline() has passed: sends again, or gives up after the third try.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> on_timeout(TimePoint now);

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] TimePoint deadline() const noexcept { return deadline_; }
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    [[nodiscard]] const Endpoint& console() const noexcept { return console_; }
    [[nodiscard]] std::uint32_t tag() const noexcept { return tag_; }

    static constexpr int kTries = 3;
    static constexpr std::chrono::milliseconds kAnswerWait{5000};

private:
    std::vector<std::uint8_t> init_packet() const;
    std::vector<std::uint8_t> cookie_packet() const;
    std::optional<std::vector<std::uint8_t>> fail(std::string why);

    std::uint32_t tag_;
    State state_ = State::idle;
    int tries_ = 0;
    TimePoint deadline_{};
    Endpoint console_;
    std::array<std::uint8_t, kCookieSize> cookie_{};
    std::string failure_;
};

// The console's side, as the mock console and the tests play it.
class ConsoleHandshake {
public:
    ConsoleHandshake(std::uint32_t tag, const std::array<std::uint8_t, kCookieSize>& cookie);

    // The answer to send, if the datagram deserves one.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> on_datagram(std::span<const std::uint8_t> datagram);

    [[nodiscard]] bool connected() const noexcept { return connected_; }
    // What the client's init announced; meaningful once one has arrived.
    [[nodiscard]] const std::optional<Endpoint>& client() const noexcept { return client_; }
    // The header of the client's init, for the mock's record: its tag, authentication code and key position.
    [[nodiscard]] const std::optional<ControlPacket>& init_header() const noexcept { return init_; }

private:
    std::uint32_t tag_;
    std::array<std::uint8_t, kCookieSize> cookie_;
    std::optional<Endpoint> client_;
    std::optional<ControlPacket> init_;
    bool connected_ = false;
};

}  // namespace psvp::proto::transport
