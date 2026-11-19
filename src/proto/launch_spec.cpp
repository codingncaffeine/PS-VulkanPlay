#include "proto/launch_spec.h"

#include <algorithm>
#include <format>
#include <vector>

#include "proto/base64.h"

namespace psvp::proto {
namespace {

// The key stream that masks the launch specification: the session cipher run over zero bytes.
std::vector<std::uint8_t> mask(const RpCipher& session_cipher, std::size_t size) {
    const std::vector<std::uint8_t> zeros(size, 0);
    return encrypt(session_cipher, 0, zeros);
}

constexpr std::string_view kHandshakeKeyField = "\"handshakeKey\":\"";

}  // namespace

std::string format_launch_spec(const LaunchSpec& spec) {
    // The fixed values are the ones other clients send and consoles accept; none
    // of them describes this client, and none has been tried with another value.
    const bool ps5 = spec.kind == ConsoleKind::ps5;
    const bool hevc = spec.codec != Codec::h264;
    const bool hdr = spec.codec == Codec::hevc_hdr;
    std::string text = std::format(
        "{{\"sessionId\":\"sessionId4321\","
        "\"streamResolutions\":[{{\"resolution\":{{\"width\":{},\"height\":{}}},\"maxFps\":{},\"score\":10}}],"
        "\"network\":{{\"bwKbpsSent\":{},\"bwLoss\":0.001000,\"mtu\":{},\"rtt\":{},\"ports\":[53,2053]}},"
        "\"slotId\":1,"
        "\"appSpecification\":{{\"minFps\":30,\"minBandwidth\":0,\"extTitleId\":\"ps3\",\"version\":1,\"timeLimit\":1,"
        "\"startTimeout\":100,\"afkTimeout\":100,\"afkTimeoutDisconnect\":100}},"
        "\"konan\":{{\"ps3AccessToken\":\"accessToken\",\"ps3RefreshToken\":\"refreshToken\"}},"
        "\"requestGameSpecification\":{{\"model\":\"bravia_tv\",\"platform\":\"android\",\"audioChannels\":\"5.1\","
        "\"language\":\"sp\",\"acceptButton\":\"X\",\"connectedControllers\":[\"xinput\",\"ds3\",\"ds4\"],"
        "\"yuvCoefficient\":\"bt601\",\"videoEncoderProfile\":\"hw4.1\",\"audioEncoderProfile\":\"audio1\"",
        spec.width, spec.height, spec.max_fps, spec.bitrate_kbps, spec.mtu, spec.round_trip_ms);
    if (ps5) text += ",\"adaptiveStreamMode\": \"resize\"";
    text += "},\"userProfile\":{\"onlineId\":\"psnId\",\"npId\":\"npId\",\"region\":\"US\",\"languagesUsed\":[\"en\",\"jp\"]},";
    if (ps5) {
        text += hevc ? "\"videoCodec\":\"hevc\"," : "\"videoCodec\":\"avc\",";
        text += hdr ? "\"dynamicRange\":\"HDR\"," : "\"dynamicRange\":\"SDR\",";
    }
    text += kHandshakeKeyField;
    text += base64_encode(spec.handshake_key);
    text += "\"}";
    return text;
}

std::string seal_launch_spec(const RpCipher& session_cipher, std::string_view text) {
    std::vector<std::uint8_t> sealed = mask(session_cipher, text.size() + 1);
    for (std::size_t i = 0; i < text.size(); ++i) sealed[i] ^= static_cast<std::uint8_t>(text[i]);
    // The last byte masks the closing zero and stays as it is.
    return base64_encode(sealed);
}

std::optional<std::string> open_launch_spec(const RpCipher& session_cipher, std::string_view sealed) {
    const auto bytes = base64_decode(sealed);
    if (!bytes || bytes->empty()) return std::nullopt;
    const std::vector<std::uint8_t> key_stream = mask(session_cipher, bytes->size());
    std::string text(bytes->size(), '\0');
    for (std::size_t i = 0; i < bytes->size(); ++i) text[i] = static_cast<char>((*bytes)[i] ^ key_stream[i]);
    if (text.back() != '\0') return std::nullopt;
    text.pop_back();
    return text;
}

std::optional<Key16> launch_spec_handshake_key(std::string_view text) {
    const std::size_t field = text.find(kHandshakeKeyField);
    if (field == std::string_view::npos) return std::nullopt;
    const std::size_t begin = field + kHandshakeKeyField.size();
    const std::size_t end = text.find('"', begin);
    if (end == std::string_view::npos) return std::nullopt;
    const auto bytes = base64_decode(text.substr(begin, end - begin));
    if (!bytes || bytes->size() != 16) return std::nullopt;
    Key16 key{};
    std::copy(bytes->begin(), bytes->end(), key.begin());
    return key;
}

}  // namespace psvp::proto
