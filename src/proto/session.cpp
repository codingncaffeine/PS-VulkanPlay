#include "proto/session.h"

#include <algorithm>
#include <charconv>
#include <format>

#include "proto/base64.h"
#include "proto/hex.h"
#include "proto/text_message.h"

namespace psvp::proto {
namespace {

constexpr std::string_view kOsType = "Win10.0.0";

std::span<const std::uint8_t> as_bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::string sealed_base64(const RpCipher& cipher, std::uint64_t number, std::span<const std::uint8_t> plain) {
    return base64_encode(encrypt(cipher, number, plain));
}

// The registration key as it travels encrypted: its characters in a sixteen-byte field.
Key16 regist_key_field(std::string_view regist_key) {
    Key16 field{};
    std::copy_n(regist_key.begin(), std::min(regist_key.size(), field.size()), field.begin());
    return field;
}

}  // namespace

std::string build_session_request(ConsoleKind kind, std::string_view console_address, std::string_view regist_key) {
    return std::format("GET {} HTTP/1.1\r\n"
                       "Host: {}:{}\r\n"
                       "User-Agent: remoteplay Windows\r\n"
                       "Connection: close\r\n"
                       "Content-Length: 0\r\n"
                       "RP-Registkey: {}\r\n"
                       "Rp-Version: {}\r\n"
                       "\r\n",
                       kind == ConsoleKind::ps5 ? "/sie/ps5/rp/sess/init" : "/sie/ps4/rp/sess/init", console_address,
                       kSessionPort, hex_encode(as_bytes(regist_key)), rp_version(kind));
}

std::string describe(const SessionError& error) {
    switch (error.refusal) {
        case SessionRefusal::malformed: return "the console's answer could not be read";
        case SessionRefusal::in_use: return "Remote Play is already in use on this console";
        case SessionRefusal::crashed: return "Remote Play has crashed on the console; restarting the console clears it";
        case SessionRefusal::version_mismatch:
            if (!error.console_version.empty()) {
                return std::format("the console speaks protocol version {}, which this program does not", error.console_version);
            }
            return "the console speaks a protocol version this program does not";
        case SessionRefusal::invalid_account: return "the console does not accept the PSN account this client is registered for";
        case SessionRefusal::not_registered: return "the console no longer knows this client; register with it again";
        case SessionRefusal::other: break;
    }
    if (error.reason != 0) return std::format("the console refused the session (status {}, reason {:08x})", error.status, error.reason);
    return std::format("the console refused the session (status {})", error.status);
}

std::expected<Key16, SessionError> parse_session_response(std::string_view head) {
    const auto message = parse_status_message(head);
    if (!message) return std::unexpected(SessionError{});

    if (message->status == 200) {
        const auto nonce_text = message->find("RP-Nonce");
        const auto bytes = nonce_text ? base64_decode(*nonce_text) : std::unexpected(Base64Error::bad_length);
        Key16 nonce{};
        if (!bytes || bytes->size() != nonce.size()) return std::unexpected(SessionError{SessionRefusal::malformed, 200, 0, {}});
        std::copy(bytes->begin(), bytes->end(), nonce.begin());
        return nonce;
    }

    SessionError error{SessionRefusal::other, message->status, 0, {}};
    if (const auto reason = message->find("RP-Application-Reason")) {
        std::from_chars(reason->data(), reason->data() + reason->size(), error.reason, 16);
    }
    if (const auto version = message->find("RP-Version")) error.console_version = *version;
    switch (error.reason) {
        case 0x80108b10: error.refusal = SessionRefusal::in_use; break;
        case 0x80108b15: error.refusal = SessionRefusal::crashed; break;
        case 0x80108b11: error.refusal = SessionRefusal::version_mismatch; break;
        case 0x80108b02: error.refusal = SessionRefusal::invalid_account; break;
        case 0x80108b09: error.refusal = SessionRefusal::not_registered; break;
        default: break;
    }
    return std::unexpected(error);
}

DeviceId make_device_id(const Key16& random_part) {
    constexpr std::array<std::uint8_t, 10> prefix{0x00, 0x18, 0x00, 0x00, 0x00, 0x07, 0x00, 0x40, 0x00, 0x80};
    DeviceId id{};
    std::copy(prefix.begin(), prefix.end(), id.begin());
    std::copy(random_part.begin(), random_part.end(), id.begin() + prefix.size());
    return id;  // the last six bytes stay zero
}

CtrlRequest build_ctrl_request(const RpCipher& cipher, std::string_view console_address, std::string_view regist_key,
                               const DeviceId& device_id, Codec codec) {
    const bool ps5 = cipher.kind == ConsoleKind::ps5;
    std::uint64_t number = 0;
    const std::string auth = sealed_base64(cipher, number++, regist_key_field(regist_key));
    const std::string did = sealed_base64(cipher, number++, device_id);
    std::vector<std::uint8_t> os_type(kOsType.begin(), kOsType.end());
    os_type.push_back(0);
    const std::string os = sealed_base64(cipher, number++, os_type);
    const std::array<std::uint8_t, 4> zero_bitrate{};
    const std::string bitrate = sealed_base64(cipher, number++, zero_bitrate);

    CtrlRequest request;
    request.text = std::format("GET {} HTTP/1.1\r\n"
                               "Host: {}:{}\r\n"
                               "User-Agent: remoteplay Windows\r\n"
                               "Connection: keep-alive\r\n"
                               "Content-Length: 0\r\n"
                               "RP-Auth: {}\r\n"
                               "RP-Version: {}\r\n"
                               "RP-Did: {}\r\n"
                               "RP-ControllerType: 3\r\n"
                               "RP-ClientType: 11\r\n"
                               "RP-OSType: {}\r\n"
                               "RP-ConPath: 1\r\n",
                               ps5 ? "/sie/ps5/rp/sess/ctrl" : "/sie/ps4/rp/sess/ctrl", console_address, kSessionPort, auth,
                               rp_version(cipher.kind), did, os);
    if (ps5) request.text += "RP-PadProcNo: 3\r\n";
    request.text += std::format("RP-StartBitrate: {}\r\n", bitrate);
    if (ps5) {
        const std::uint32_t type = codec == Codec::hevc_hdr ? 3U : codec == Codec::hevc ? 2U : 1U;
        const std::array<std::uint8_t, 4> streaming_type{static_cast<std::uint8_t>(type), 0, 0, 0};
        request.text += std::format("RP-StreamingType: {}\r\n", sealed_base64(cipher, number++, streaming_type));
    }
    request.text += "\r\n";
    request.next_local_number = number;
    return request;
}

std::string_view to_string(ServerModel model) noexcept {
    switch (model) {
        case ServerModel::ps4: return "PS4";
        case ServerModel::ps4_pro: return "PS4 Pro";
        case ServerModel::ps5: return "PS5";
        case ServerModel::unknown: break;
    }
    return "unknown model";
}

std::string describe(const CtrlError& error) {
    if (error.malformed) return "the console's answer on the control channel could not be read";
    return std::format("the console refused the control channel (status {})", error.status);
}

std::expected<CtrlAccepted, CtrlError> parse_ctrl_response(const RpCipher& cipher, std::string_view head) {
    const auto message = parse_status_message(head);
    if (!message) return std::unexpected(CtrlError{true, 0});
    if (message->status != 200) return std::unexpected(CtrlError{false, message->status});

    CtrlAccepted accepted;
    if (const auto prohibit = message->find("RP-Prohibit")) accepted.prohibited = *prohibit == "1";
    if (const auto server_type = message->find("RP-Server-Type")) {
        const auto sealed = base64_decode(*server_type);
        if (!sealed) return std::unexpected(CtrlError{true, 200});
        // Sixteen bytes, the console's first encrypted value. Anything else is not
        // a server type, and then the console has used no number for it.
        if (sealed->size() == 16) {
            const std::vector<std::uint8_t> plain = decrypt(cipher, accepted.next_remote_number++, *sealed);
            accepted.model = plain[0] == 0 ? ServerModel::ps4 : plain[0] == 1 ? ServerModel::ps4_pro
                           : plain[0] == 2 ? ServerModel::ps5 : ServerModel::unknown;
        }
    }
    return accepted;
}

CtrlChannel::CtrlChannel(const RpCipher& cipher, std::uint64_t next_local_number, std::uint64_t next_remote_number)
    : cipher_(cipher), local_number_(next_local_number), remote_number_(next_remote_number) {}

std::optional<std::vector<std::uint8_t>> CtrlChannel::encode(std::uint16_t type, std::span<const std::uint8_t> payload) {
    if (kCtrlHeaderSize + payload.size() > kMaxCtrlMessage) return std::nullopt;
    // Sized for the whole message at once: header, then the encrypted payload.
    std::vector<std::uint8_t> out(kCtrlHeaderSize + payload.size());
    const auto size = static_cast<std::uint32_t>(payload.size());
    out[0] = static_cast<std::uint8_t>(size >> 24);
    out[1] = static_cast<std::uint8_t>(size >> 16);
    out[2] = static_cast<std::uint8_t>(size >> 8);
    out[3] = static_cast<std::uint8_t>(size);
    out[4] = static_cast<std::uint8_t>(type >> 8);
    out[5] = static_cast<std::uint8_t>(type);
    if (!payload.empty()) {
        // Only a message with a payload takes a number.
        const std::vector<std::uint8_t> sealed = encrypt(cipher_, local_number_++, payload);
        std::copy(sealed.begin(), sealed.end(), out.begin() + kCtrlHeaderSize);
    }
    return out;
}

bool CtrlChannel::feed(std::span<const std::uint8_t> bytes) {
    if (broken_) return false;
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    return true;
}

std::optional<CtrlMessage> CtrlChannel::next() {
    if (broken_ || buffer_.size() < kCtrlHeaderSize) return std::nullopt;
    const std::size_t size = (std::size_t{buffer_[0]} << 24) | (std::size_t{buffer_[1]} << 16) |
                             (std::size_t{buffer_[2]} << 8) | std::size_t{buffer_[3]};
    if (kCtrlHeaderSize + size > kMaxCtrlMessage) {
        broken_ = true;
        buffer_.clear();
        return std::nullopt;
    }
    if (buffer_.size() < kCtrlHeaderSize + size) return std::nullopt;

    CtrlMessage message;
    message.type = static_cast<std::uint16_t>((std::uint16_t{buffer_[4]} << 8) | buffer_[5]);
    if (size > 0) {
        message.payload = decrypt(cipher_, remote_number_++, std::span(buffer_).subspan(kCtrlHeaderSize, size));
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(kCtrlHeaderSize + size));
    return message;
}

std::optional<std::string> parse_session_id(std::span<const std::uint8_t> payload) {
    if (payload.size() < 2 || payload[0] != 'J') return std::nullopt;
    const auto id = payload.subspan(1);
    if (id.size() < 24 || id.size() > 78) return std::nullopt;
    const bool alphanumeric = std::ranges::all_of(id, [](std::uint8_t c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    });
    if (!alphanumeric) return std::nullopt;
    return std::string(id.begin(), id.end());
}

}  // namespace psvp::proto
