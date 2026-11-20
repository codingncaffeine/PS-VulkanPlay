// The messages that travel in the transport's data chunks, in the field
// coding of proto/wire.h. docs/protocol/messages.md lists them.
// Only the messages this client uses so far have a body here; any other is
// read as its type alone.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace psvp::proto {

namespace message_type {
inline constexpr std::uint32_t kBig = 0;
inline constexpr std::uint32_t kBang = 1;
inline constexpr std::uint32_t kHeartbeat = 3;
inline constexpr std::uint32_t kCorruptFrame = 5;
inline constexpr std::uint32_t kDisconnect = 8;
inline constexpr std::uint32_t kProbe = 12;
inline constexpr std::uint32_t kStreamInfo = 13;
inline constexpr std::uint32_t kStreamInfoAck = 14;
inline constexpr std::uint32_t kConnectionQuality = 16;
inline constexpr std::uint32_t kControllerConnection = 21;
inline constexpr std::uint32_t kKeyframeRequest = 25;
inline constexpr std::uint32_t kProtocolRequest = 31;
inline constexpr std::uint32_t kProtocolRequestAck = 32;
}  // namespace message_type

// The client's opening message of a connection.
struct BigMessage {
    std::uint32_t client_version = 0;
    std::string session_key;
    std::string launch_spec;
    std::vector<std::uint8_t> encrypted_key;
    std::optional<std::vector<std::uint8_t>> ecdh_public_key;
    std::optional<std::vector<std::uint8_t>> ecdh_signature;
    friend bool operator==(const BigMessage&, const BigMessage&) = default;
};

// The console's answer to it.
struct BangMessage {
    std::uint32_t server_version = 0;
    std::uint32_t token = 0;
    bool encrypted_key_accepted = false;
    bool version_accepted = false;
    std::string session_key;
    std::optional<std::vector<std::uint8_t>> ecdh_public_key;
    std::optional<std::vector<std::uint8_t>> ecdh_signature;
    friend bool operator==(const BangMessage&, const BangMessage&) = default;
};

struct DisconnectMessage {
    std::string reason;
    friend bool operator==(const DisconnectMessage&, const DisconnectMessage&) = default;
};

// Network probe: "send back what I send you" on or off.
struct ProbeEcho {
    bool state = false;
    friend bool operator==(const ProbeEcho&, const ProbeEcho&) = default;
};

// Network probe: "send me a packet of this size".
struct ProbeMtu {
    std::uint32_t id = 0;
    std::uint32_t mtu_request = 0;
    std::optional<std::uint32_t> mtu_sent;
    std::optional<std::uint32_t> count;
    std::optional<std::uint32_t> send_delay;
    std::optional<std::uint32_t> delta;
    friend bool operator==(const ProbeMtu&, const ProbeMtu&) = default;
};

// Network probe: the start and the end of the client's own size test.
struct ProbeClientMtu {
    std::uint32_t id = 0;
    std::uint32_t mtu_request = 0;
    bool state = false;
    std::optional<std::uint32_t> mtu_down;
    friend bool operator==(const ProbeClientMtu&, const ProbeClientMtu&) = default;
};

struct ProtocolRequest {
    std::vector<std::uint32_t> versions;
    friend bool operator==(const ProtocolRequest&, const ProtocolRequest&) = default;
};

struct ProtocolRequestAck {
    std::optional<std::uint32_t> version;
    friend bool operator==(const ProtocolRequestAck&, const ProtocolRequestAck&) = default;
};

// One picture size the console can send, with the header its decoder needs first.
struct StreamResolution {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> video_header;
    friend bool operator==(const StreamResolution&, const StreamResolution&) = default;
};

// What a stream will carry. The console sends it once the keys exist; the
// client sends one too, with only the sound format of its microphone.
// One of the sound channels a console lists in its stream information: a type, the format in
// the form of the sound header, and whether its frames are samples as they are (not coded).
struct SoundChannel {
    std::uint32_t type = 0;
    std::vector<std::uint8_t> header;
    std::optional<bool> raw;
    friend bool operator==(const SoundChannel&, const SoundChannel&) = default;
};

struct StreamInfo {
    static constexpr std::size_t kMaxResolutions = 8;  // further ones are not kept
    static constexpr std::size_t kMaxSoundChannels = 8;

    std::vector<StreamResolution> resolutions;
    std::vector<std::uint8_t> audio_header;  // an AudioHeader in its fourteen-byte form
    std::optional<std::uint32_t> start_timeout;
    std::optional<std::uint32_t> afk_timeout;
    std::optional<std::uint32_t> afk_timeout_disconnect;
    std::optional<std::uint32_t> congestion_control_interval;
    std::vector<SoundChannel> sound_channels;  // what the console says it sends besides, or as, the sound
    friend bool operator==(const StreamInfo&, const StreamInfo&) = default;
};

namespace controller_type {
inline constexpr std::uint32_t kDualShock4 = 2;
inline constexpr std::uint32_t kDualSense = 6;
}  // namespace controller_type

// "A controller of this type is (dis)connected."
struct ControllerConnection {
    std::optional<std::uint32_t> controller_id;
    std::optional<bool> connected;
    std::optional<std::uint32_t> type;
    friend bool operator==(const ControllerConnection&, const ControllerConnection&) = default;
};

// How the console sees the connection; it sends one about every second.
struct ConnectionQuality {
    std::optional<std::uint32_t> target_bitrate;    // of the picture, in bits per second
    std::optional<std::uint32_t> upstream_bitrate;
    std::optional<float> upstream_loss;
    std::optional<bool> disable_upstream_audio;
    std::optional<double> rtt;
    std::optional<std::uint64_t> loss;
    friend bool operator==(const ConnectionQuality&, const ConnectionQuality&) = default;
};

// "These picture frames did not arrive whole", first to last, both included.
struct CorruptFrames {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    friend bool operator==(const CorruptFrames&, const CorruptFrames&) = default;
};

struct StreamMessage {
    using Body = std::variant<std::monostate, BigMessage, BangMessage, DisconnectMessage, ProbeEcho, ProbeMtu,
                              ProbeClientMtu, ProtocolRequest, ProtocolRequestAck, StreamInfo, ControllerConnection,
                              CorruptFrames, ConnectionQuality>;
    std::uint32_t type = 0;
    Body body;  // nothing for a type without a body here, or whose body was missing

    template <typename T> [[nodiscard]] const T* as() const noexcept { return std::get_if<T>(&body); }
    friend bool operator==(const StreamMessage&, const StreamMessage&) = default;
};

[[nodiscard]] StreamMessage make_message(StreamMessage::Body body);
// A message that is its type alone: a heartbeat, the answer to the stream information, a keyframe request.
[[nodiscard]] StreamMessage make_plain_message(std::uint32_t type);

[[nodiscard]] std::vector<std::uint8_t> encode_message(const StreamMessage& message);

// Nothing for bytes that are not a message, or for a known type whose body is
// present but lacks a field that the type requires.
[[nodiscard]] std::optional<StreamMessage> parse_message(std::span<const std::uint8_t> bytes);

// The format of a sound stream, as it travels in the stream information.
struct AudioHeader {
    static constexpr std::size_t kSize = 14;

    std::uint8_t channels = 2;
    std::uint8_t bits = 16;
    std::uint32_t rate = 48000;
    std::uint32_t frame_size = 480;  // samples per frame
    std::uint32_t unknown = 1;       // always 1 in what was seen
    friend bool operator==(const AudioHeader&, const AudioHeader&) = default;
};

[[nodiscard]] std::vector<std::uint8_t> encode_audio_header(const AudioHeader& header);
[[nodiscard]] std::optional<AudioHeader> parse_audio_header(std::span<const std::uint8_t> bytes);

}  // namespace psvp::proto
