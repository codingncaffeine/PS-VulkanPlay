#include "proto/regist.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <stdexcept>

#include "proto/base64.h"
#include "proto/hex.h"
#include "proto/text_message.h"

namespace psvp::proto {
namespace {

// Identifies the kind of client to the console.
constexpr std::string_view kClientType = "dabfa2ec873de5839bee8d3f4c0239c4282c07c25c6077a2931afcf0adc0d34f";

constexpr std::string_view request_path(ConsoleKind kind) {
    return kind == ConsoleKind::ps5 ? "/sie/ps5/rp/sess/rgst" : "/sie/ps4/rp/sess/rgst";
}

std::string_view family_prefix(ConsoleKind kind) { return kind == ConsoleKind::ps5 ? "PS5" : "PS4"; }

template <class Number>
bool parse_number(std::string_view text, Number& out, int base) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out, base);
    return error == std::errc{} && end == text.data() + text.size();
}

// The console's own limits on the text fields it sends.
bool fits(std::string_view value, std::size_t limit) { return value.size() < limit; }

}  // namespace

std::string regist_search_packet(ConsoleKind kind) {
    std::string packet(kind == ConsoleKind::ps5 ? "SRC3" : "SRC2");
    packet.push_back('\0');
    return packet;
}

bool is_regist_search_reply(ConsoleKind kind, std::span<const std::uint8_t> datagram) {
    const std::string_view expected = kind == ConsoleKind::ps5 ? "RES3" : "RES2";
    return datagram.size() >= expected.size() && std::equal(expected.begin(), expected.end(), datagram.begin());
}

RegistRequest build_regist_request(ConsoleKind kind, const AccountId& account_id, std::uint32_t pin,
                                   const Key16& nonce) {
    using namespace regist_layout;
    RegistRequest request;
    request.payload.assign(kInnerOffset, kFiller);

    // Two bytes of the filler choose which entries of the fixed tables are used.
    const std::size_t key_offset = request.payload[kKeyOffsetByte] & 0x1f;
    const std::size_t wrap_offset = request.payload[kWrapOffsetByte] >> 3;
    const auto cipher = make_regist_cipher(kind, nonce, key_offset, pin);
    const auto wrapped = wrap_nonce(kind, nonce, wrap_offset);
    if (!cipher || !wrapped) throw std::logic_error("regist: table offset out of range");
    request.cipher = *cipher;
    std::copy_n(wrapped->begin(), 8, request.payload.begin() + kWrappedLowAt);
    std::copy_n(wrapped->begin() + 8, 8, request.payload.begin() + kWrappedHighAt);

    const std::string inner =
        std::format("Client-Type: {}\r\nNp-AccountId: {}\r\n", kClientType, base64_encode(account_id));
    const std::vector<std::uint8_t> sealed =
        encrypt(*cipher, 0, std::span(reinterpret_cast<const std::uint8_t*>(inner.data()), inner.size()));
    request.payload.insert(request.payload.end(), sealed.begin(), sealed.end());

    // The request line and the HOST value are sent the way consoles are known to accept them.
    request.head = std::format("POST {} HTTP/1.1\r\n HTTP/1.1\r\n"
                               "HOST: 10.0.2.15\r\n"
                               "User-Agent: remoteplay Windows\r\n"
                               "Connection: close\r\n"
                               "Content-Length: {}\r\n"
                               "RP-Version: {}\r\n"
                               "\r\n",
                               request_path(kind), request.payload.size(), rp_version(kind));
    return request;
}

std::string describe(const RegistError& error) {
    switch (error.failure) {
        case RegistFailure::malformed:
            return "the console's answer could not be read";
        case RegistFailure::incomplete:
            return "the console's answer is missing the registration key, the secret or its address";
        case RegistFailure::refused:
            if (error.reason != 0) {
                return std::format("the console refused the registration (status {}, reason {:08x})", error.status,
                                   error.reason);
            }
            return std::format("the console refused the registration (status {})", error.status);
    }
    return "unknown registration error";
}

std::expected<std::size_t, RegistError> parse_regist_response_head(std::string_view head) {
    const auto message = parse_status_message(head);
    if (!message) return std::unexpected(RegistError{RegistFailure::malformed, 0, 0});
    if (message->status != 200) {
        RegistError error{RegistFailure::refused, message->status, 0};
        if (const auto reason = message->find("RP-Application-Reason")) parse_number(*reason, error.reason, 16);
        return std::unexpected(error);
    }
    std::size_t length = 0;
    auto announced = message->find("Content-Length");
    // A console pads the number with spaces on its left.
    while (announced && announced->starts_with(' ')) announced->remove_prefix(1);
    if (!announced || !parse_number(*announced, length, 10) || length == 0 || length > kMaxRegistBody) {
        return std::unexpected(RegistError{RegistFailure::malformed, 200, 0});
    }
    return length;
}

std::expected<RegisteredConsole, RegistError> parse_regist_response_body(const RpCipher& cipher,
                                                                        std::span<const std::uint8_t> body) {
    const std::vector<std::uint8_t> plain = decrypt(cipher, 0, body);
    const auto message =
        parse_header_block(std::string_view(reinterpret_cast<const char*>(plain.data()), plain.size()));
    if (!message) return std::unexpected(RegistError{RegistFailure::malformed, 200, 0});

    const std::string family(family_prefix(cipher.kind));
    RegisteredConsole console;
    console.kind = cipher.kind;
    bool have_mac = false;
    bool have_regist_key = false;
    bool have_key = false;

    for (const TextHeader& header : message->headers) {
        const std::string_view value = header.value;
        if (header.name == "AP-Ssid" && fits(value, 0x30)) console.ap_ssid = value;
        else if (header.name == "AP-Bssid" && fits(value, 0x20)) console.ap_bssid = value;
        else if (header.name == "AP-Key" && fits(value, 0x50)) console.ap_key = value;
        else if (header.name == "AP-Name" && fits(value, 0x20)) console.ap_name = value;
        else if (header.name == family + "-Nickname" && fits(value, 0x20)) console.nickname = value;
        else if (header.name == "RP-KeyType") parse_number(value, console.key_type, 10);
        else if (header.name == family + "-RegistKey") {
            // Hex of the key's characters; at most sixteen of them, none after a NUL.
            const auto bytes = hex_decode(value);
            if (!bytes || bytes->empty() || bytes->size() > 16) continue;
            std::string text(bytes->begin(), bytes->end());
            text.resize(std::min(text.find('\0'), text.size()));
            if (text.empty()) continue;
            console.regist_key = text;
            have_regist_key = true;
        } else if (header.name == "RP-Key") {
            const auto bytes = hex_decode(value);
            if (!bytes || bytes->size() != console.key.size()) continue;
            std::copy(bytes->begin(), bytes->end(), console.key.begin());
            have_key = true;
        } else if (header.name == family + "-Mac") {
            const auto bytes = hex_decode(value);
            if (!bytes || bytes->size() != console.mac.size()) continue;
            std::copy(bytes->begin(), bytes->end(), console.mac.begin());
            have_mac = true;
        }
    }
    if (!have_mac || !have_regist_key || !have_key) {
        return std::unexpected(RegistError{RegistFailure::incomplete, 200, 0});
    }
    return console;
}

}  // namespace psvp::proto
