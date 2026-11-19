// The stream transport over UDP: control packets and the opening handshake.
// docs/protocol/transport.md describes it. Nothing here touches a socket or a
// clock; the caller passes datagrams and the time in and sends what comes out.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "proto/stream_cipher.h"

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
    [[nodiscard]] std::uint32_t tag() const noexcept { return tag_; }

private:
    std::uint32_t tag_;
    std::array<std::uint8_t, kCookieSize> cookie_;
    std::optional<Endpoint> client_;
    std::optional<ControlPacket> init_;
    bool connected_ = false;
};

// ---- Packet authentication -----------------------------------------------
// Once a connection has its keys, every packet carries a four-byte code and
// the low 32 bits of its key position. docs/protocol/stream-setup.md.

// The keys this end seals its packets with, and how far into the key stream it is.
struct SendKeys {
    explicit SendKeys(const StreamCipher& keys) : cipher(keys) {}

    // The key position for a packet that counts `size` bytes; the next packet's lies behind it.
    [[nodiscard]] std::uint64_t take(std::size_t size) noexcept {
        const std::uint64_t position = next_position;
        next_position += size + size % 16;
        return position;
    }

    StreamCipher cipher;
    std::uint64_t next_position = 0;
};

// The keys the other end seals its packets with.
struct ReceiveKeys {
    explicit ReceiveKeys(const StreamCipher& keys) : cipher(keys) {}

    StreamCipher cipher;
    KeyPositionTracker positions;
};

// Where a kind of packet keeps its code and its key position.
struct MacLayout {
    std::size_t mac_offset = 0;
    std::size_t position_offset = 0;
    bool position_counts = false;  // false: the code is computed with the position's bytes as zero
};
// Nothing for a kind of packet that carries no code. Only the low four bits of the first byte say the kind.
[[nodiscard]] std::optional<MacLayout> mac_layout(std::uint8_t first_byte) noexcept;

// The code a datagram must carry at a key position. Nothing for a kind that carries none, or a datagram too short for its fields.
[[nodiscard]] std::optional<Mac4> datagram_mac(const StreamCipher& cipher, std::uint64_t key_position,
                                               std::span<const std::uint8_t> datagram);
// Writes the key position and the code into a datagram. False for a datagram that has no place for them.
bool seal_datagram(const StreamCipher& cipher, std::uint64_t key_position, std::span<std::uint8_t> datagram);
// True for a datagram that carries the right code; its key position is then the last accepted one.
[[nodiscard]] bool verify_datagram(ReceiveKeys& keys, std::span<const std::uint8_t> datagram);

// ---- Messages that must arrive -------------------------------------------

namespace data_type {
inline constexpr std::uint8_t kMessage = 0;  // a message in the field coding (proto/stream_message.h)
inline constexpr std::uint8_t kRumble = 7;
inline constexpr std::uint8_t kPadInfo = 9;
inline constexpr std::uint8_t kTriggerEffects = 11;
}  // namespace data_type

namespace chunk_flag {
inline constexpr std::uint8_t kLast = 1;  // the message ends with this chunk
}  // namespace chunk_flag

// The payload of a data chunk; as a Messenger hands it over, a whole message.
struct DataChunk {
    std::uint32_t sequence = 0;  // of the chunk, or of a message's last chunk
    std::uint16_t channel = 0;
    std::uint8_t data_type = data_type::kMessage;
    std::vector<std::uint8_t> data;
    // The rest is not part of the payload.
    std::uint8_t flags = 0;          // of the packet that carried it (the last one of a message)
    std::uint16_t chunks = 1;        // how many chunks the message came in
    bool authentic = false;          // every chunk carried the right authentication code
    std::uint32_t key_position = 0;  // as the (last) packet carried it
    Mac4 mac{};                      // as the (last) packet carried it
};

[[nodiscard]] std::vector<std::uint8_t> encode_data_chunk(const DataChunk& chunk);
[[nodiscard]] std::optional<DataChunk> parse_data_chunk(std::span<const std::uint8_t> payload);

// The payload of a data acknowledgment: everything up to `cumulative` has arrived.
struct DataAck {
    std::uint32_t cumulative = 0;
    std::uint32_t receive_window = kReceiveWindow;
    std::uint16_t gap_blocks = 0;
    std::uint16_t duplicates = 0;
};

[[nodiscard]] std::vector<std::uint8_t> encode_data_ack(const DataAck& ack);
[[nodiscard]] std::optional<DataAck> parse_data_ack(std::span<const std::uint8_t> payload);

// Sequence numbers wrap around: `a` comes before `b` when it is less than half the number space behind it.
[[nodiscard]] constexpr bool sequence_before(std::uint32_t a, std::uint32_t b) noexcept {
    return a != b && b - a < 0x80000000U;
}

// One end's numbered messages: sends them again until they are acknowledged,
// and hands the other end's messages over in order, each exactly once.
class Messenger {
public:
    static constexpr std::size_t kWindow = 16;
    static constexpr std::chrono::milliseconds kResendAfter{200};
    static constexpr int kResends = 25;
    static constexpr std::size_t kMaxMessage = 65536;      // a message that grows past this is dropped
    static constexpr std::size_t kSmallestDatagram = 64;   // no message is split into smaller datagrams

    // Whether the connection will get keys (use_keys). While it waits for them
    // it hands over one message at a time, so that the message that brings the
    // keys can be acted on before the next one is looked at.
    enum class Keys { none, expected };

    Messenger(std::uint32_t local_tag, std::uint32_t remote_tag, std::uint32_t first_local_sequence,
              std::uint32_t first_remote_sequence, Keys keys = Keys::none);

    struct Sent {
        std::uint32_t sequence = 0;
        std::vector<std::uint8_t> datagram;
    };
    // One chunk. Nothing while kWindow chunks still wait for their acknowledgment.
    [[nodiscard]] std::optional<Sent> send(std::uint8_t flags, std::uint16_t channel, std::span<const std::uint8_t> data,
                                           TimePoint now);
    // A whole message, in as many chunks as it takes to keep every datagram
    // within `max_datagram` bytes. Nothing when the window has no room for all of them.
    [[nodiscard]] std::optional<std::vector<Sent>> send_message(std::uint16_t channel, std::span<const std::uint8_t> data,
                                                                TimePoint now, std::size_t max_datagram);

    struct Received {
        std::vector<DataChunk> delivered;         // whole messages, in order
        std::vector<std::uint32_t> acknowledged;  // our chunks the other end now has
    };
    // For a control packet that carries this end's tag; others change nothing.
    // Once the keys are in use, neither does one without the right authentication code.
    [[nodiscard]] Received on_packet(const ControlPacket& packet);
    // More of what is in order, where on_packet stopped after one message (see Keys).
    [[nodiscard]] Received pump();
    [[nodiscard]] bool ready() const noexcept { return early_[0].has_value(); }
    // The acknowledgment that is due, if one is. Made when asked for, so that it
    // is sealed with keys that the message it answers brought.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> take_ack();

    // From here on every packet sent is sealed and every packet received is checked.
    // Chunks that arrived earlier and wait for their turn are checked now.
    void use_keys(std::shared_ptr<SendKeys> send, std::shared_ptr<ReceiveKeys> receive);
    [[nodiscard]] bool keyed() const noexcept { return receive_keys_ != nullptr; }
    // Packets dropped for a wrong authentication code.
    [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }
    // True while the first chunks of a message have arrived and its last has not.
    [[nodiscard]] bool mid_message() const noexcept { return partial_.has_value(); }

    struct Timer {
        std::vector<std::vector<std::uint8_t>> resend;
        std::vector<std::uint32_t> abandoned;  // given up on after kResends
    };
    [[nodiscard]] Timer on_timer(TimePoint now);
    // When on_timer has work to do; nothing while no message waits.
    [[nodiscard]] std::optional<TimePoint> deadline() const;

    [[nodiscard]] std::size_t waiting() const noexcept { return unacknowledged_.size(); }
    [[nodiscard]] std::uint32_t local_tag() const noexcept { return local_tag_; }
    [[nodiscard]] std::uint32_t remote_tag() const noexcept { return remote_tag_; }

private:
    struct Pending {
        std::uint32_t sequence = 0;
        int resends = 0;
        TimePoint last_sent{};
        std::vector<std::uint8_t> datagram;
    };
    // A chunk that arrived and waits for its turn.
    struct Held {
        std::uint32_t sequence = 0;
        std::uint16_t channel = 0;
        std::uint8_t flags = 0;
        std::vector<std::uint8_t> rest;  // the payload after its first eight bytes
        std::uint32_t key_position = 0;
        Mac4 mac{};
        bool authentic = false;
        std::vector<std::uint8_t> unchecked;  // the whole packet, kept until the keys arrive
    };

    Sent send_chunk(std::uint8_t flags, std::uint16_t channel, bool first, std::span<const std::uint8_t> data, TimePoint now);
    void hand_over(Received& result);

    std::uint32_t local_tag_;
    std::uint32_t remote_tag_;
    std::uint32_t next_local_;
    std::uint32_t next_remote_;
    Keys keys_;
    bool delivered_any_ = false;
    bool ack_due_ = false;
    std::uint64_t rejected_ = 0;
    std::shared_ptr<SendKeys> send_keys_;
    std::shared_ptr<ReceiveKeys> receive_keys_;
    std::vector<Pending> unacknowledged_;
    std::array<std::optional<Held>, kWindow> early_{};  // early_[i] has sequence next_remote_ + i
    std::optional<DataChunk> partial_;                  // a message whose last chunk has not come
};

// ---- Picture and sound packets, as the network probe uses them ------------

// The header of a picture or sound packet in the layout of the probe
// connection (the stream's own layouts differ and come with the receivers).
struct MediaHeader {
    bool video = false;
    bool unit_info = false;        // three more header bytes follow
    std::uint16_t packet_index = 0;
    std::uint16_t frame_index = 0;
    std::uint16_t unit_index = 0;   // eleven bits
    std::uint16_t units_total = 1;  // 1 to 2048
    std::uint16_t units_repair = 0; // ten bits
    std::uint8_t codec = 0;
    std::uint32_t key_position = 0;
    std::uint16_t video_word = 0;      // video only
    std::uint8_t adaptive_stream = 0;  // video only, three bits

    [[nodiscard]] std::size_t size() const noexcept { return 18 + (video ? 3U : 0U) + (unit_info ? 3U : 0U); }
};

// A datagram of `datagram_size` bytes (at least the header's) that starts with the header; the rest is zero.
[[nodiscard]] std::vector<std::uint8_t> encode_media(const MediaHeader& header, std::size_t datagram_size);

struct MediaPacket {
    MediaHeader header;
    std::span<const std::uint8_t> data;  // a view into the datagram
};
[[nodiscard]] std::optional<MediaPacket> parse_media(std::span<const std::uint8_t> datagram);

}  // namespace psvp::proto::transport
