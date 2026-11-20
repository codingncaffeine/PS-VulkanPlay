// Starting a session with a registered console: the request that yields the
// console's nonce, the control channel's opening request, and the messages that
// then travel on it. docs/protocol/session.md describes all of it.
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "proto/console.h"
#include "proto/rp_cipher.h"

namespace psvp::proto {

enum class Codec { h264, hevc, hevc_hdr };

// --- The session request -----------------------------------------------------

// `console_address` is the dotted address the request is sent to.
[[nodiscard]] std::string build_session_request(ConsoleKind kind, std::string_view console_address,
                                                std::string_view regist_key);

enum class SessionRefusal {
    malformed,         // not an answer a console sends
    in_use,            // Remote Play is already in use on the console
    crashed,           // Remote Play has crashed on the console
    version_mismatch,  // the console speaks another protocol version
    invalid_account,
    not_registered,
    other,
};

struct SessionError {
    SessionRefusal refusal = SessionRefusal::malformed;
    int status = 0;
    std::uint32_t reason = 0;
    std::string console_version;  // the version the console named, if it did
};

[[nodiscard]] std::string describe(const SessionError& error);

// The answer's head, up to and including its blank line. Returns the console's nonce.
[[nodiscard]] std::expected<Key16, SessionError> parse_session_response(std::string_view head);

// --- The control channel's opening -------------------------------------------

using DeviceId = std::array<std::uint8_t, 32>;

// A device ID with the given sixteen random bytes in its middle.
[[nodiscard]] DeviceId make_device_id(const Key16& random_part);

struct CtrlRequest {
    std::string text;
    std::uint64_t next_local_number = 0;  // the number of the client's first message with a payload
};

[[nodiscard]] CtrlRequest build_ctrl_request(const RpCipher& cipher, std::string_view console_address,
                                             std::string_view regist_key, const DeviceId& device_id, Codec codec);

enum class ServerModel { ps4, ps4_pro, ps5, unknown };

[[nodiscard]] std::string_view to_string(ServerModel model) noexcept;

struct CtrlAccepted {
    ServerModel model = ServerModel::unknown;
    bool prohibited = false;
    std::uint64_t next_remote_number = 0;  // the number of the console's first message with a payload
};

struct CtrlError {
    bool malformed = false;
    int status = 0;
};

[[nodiscard]] std::string describe(const CtrlError& error);

[[nodiscard]] std::expected<CtrlAccepted, CtrlError> parse_ctrl_response(const RpCipher& cipher, std::string_view head);

// --- Control messages ---------------------------------------------------------

namespace ctrl_type {
inline constexpr std::uint16_t kSessionId = 0x33;
inline constexpr std::uint16_t kHeartbeatRequest = 0xfe;
inline constexpr std::uint16_t kHeartbeatReply = 0x1fe;
inline constexpr std::uint16_t kLoginPinRequest = 0x04;
inline constexpr std::uint16_t kLoginPinReply = 0x8004;
inline constexpr std::uint16_t kLoginResult = 0x05;
inline constexpr std::uint16_t kGoToRest = 0x50;
inline constexpr std::uint16_t kSwitchToStream = 0x34;
inline constexpr std::uint16_t kMicrophoneMute = 0x36;
inline constexpr std::uint16_t kDualSenseFeatures = 0x13;    // a client with a DualSense asks for what it can do
inline constexpr std::uint16_t kControllerConnected = 0x11;  // and names the controller, right behind it
inline constexpr std::uint16_t kDisplayDevices = 0x910;
}  // namespace ctrl_type

inline constexpr std::size_t kCtrlHeaderSize = 8;
// Header and payload together; nothing longer is ever sent or accepted.
inline constexpr std::size_t kMaxCtrlMessage = 512;

struct CtrlMessage {
    std::uint16_t type = 0;
    std::vector<std::uint8_t> payload;  // decrypted
};

// One end of the control channel after its opening: turns messages into bytes
// and bytes into messages, keeping each direction's message number. The console
// side uses the same class with the two starting numbers the other way around.
class CtrlChannel {
public:
    CtrlChannel(const RpCipher& cipher, std::uint64_t next_local_number, std::uint64_t next_remote_number);

    // The bytes to send. Nothing when the payload would exceed kMaxCtrlMessage.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> encode(std::uint16_t type, std::span<const std::uint8_t> payload = {});

    // Takes bytes as they arrive. False when the stream is no longer readable (a
    // message announced longer than the limit); the connection is then to be closed.
    [[nodiscard]] bool feed(std::span<const std::uint8_t> bytes);

    // The next complete message, or nothing until more bytes arrive.
    [[nodiscard]] std::optional<CtrlMessage> next();

private:
    RpCipher cipher_;
    std::uint64_t local_number_;
    std::uint64_t remote_number_;
    std::vector<std::uint8_t> buffer_;
    bool broken_ = false;
};

// The session ID carried by a kSessionId message, or nothing when it is not one
// a console sends (one byte with the ID's length, then up to 78 letters and digits).
[[nodiscard]] std::optional<std::string> parse_session_id(std::span<const std::uint8_t> payload);

}  // namespace psvp::proto
