#include "proto/base64.h"

#include <array>

namespace psvp::proto {
namespace {

constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::uint8_t kInvalid = 0xff;

constexpr std::array<std::uint8_t, 256> make_reverse_table() {
    std::array<std::uint8_t, 256> table{};
    for (auto& entry : table) entry = kInvalid;
    for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
        table[static_cast<unsigned char>(kAlphabet[i])] = static_cast<std::uint8_t>(i);
    }
    return table;
}

constexpr std::array<std::uint8_t, 256> kReverse = make_reverse_table();

}  // namespace

std::string_view to_string(Base64Error error) noexcept {
    switch (error) {
        case Base64Error::bad_length: return "length is not a multiple of four";
        case Base64Error::bad_character: return "character outside the alphabet";
        case Base64Error::bad_padding: return "padding in the wrong place";
        case Base64Error::non_canonical: return "unused bits are not zero";
    }
    return "unknown error";
}

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t n = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8) | std::uint32_t{bytes[i + 2]};
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(kAlphabet[(n >> 6) & 63]);
        out.push_back(kAlphabet[n & 63]);
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t n = std::uint32_t{bytes[i]} << 16;
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.append("==");
    } else if (rest == 2) {
        const std::uint32_t n = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8);
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(kAlphabet[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::expected<std::vector<std::uint8_t>, Base64Error> base64_decode(std::string_view text) {
    if (text.size() % 4 != 0) return std::unexpected(Base64Error::bad_length);
    std::vector<std::uint8_t> out;
    if (text.empty()) return out;

    std::size_t padding = 0;
    if (text.back() == '=') padding = text[text.size() - 2] == '=' ? 2 : 1;
    const std::size_t data_chars = text.size() - padding;
    out.reserve(text.size() / 4 * 3);

    std::uint32_t buffer = 0;
    std::size_t held = 0;
    for (std::size_t i = 0; i < data_chars; ++i) {
        const char c = text[i];
        if (c == '=') return std::unexpected(Base64Error::bad_padding);
        const std::uint8_t value = kReverse[static_cast<unsigned char>(c)];
        if (value == kInvalid) return std::unexpected(Base64Error::bad_character);
        buffer = (buffer << 6) | std::uint32_t{value};
        if (++held == 4) {
            out.push_back(static_cast<std::uint8_t>(buffer >> 16));
            out.push_back(static_cast<std::uint8_t>(buffer >> 8));
            out.push_back(static_cast<std::uint8_t>(buffer));
            buffer = 0;
            held = 0;
        }
    }
    if (padding == 1) {
        // Three characters carry 18 bits: two bytes and two unused bits.
        if ((buffer & 0x3) != 0) return std::unexpected(Base64Error::non_canonical);
        out.push_back(static_cast<std::uint8_t>(buffer >> 10));
        out.push_back(static_cast<std::uint8_t>(buffer >> 2));
    } else if (padding == 2) {
        // Two characters carry 12 bits: one byte and four unused bits.
        if ((buffer & 0xf) != 0) return std::unexpected(Base64Error::non_canonical);
        out.push_back(static_cast<std::uint8_t>(buffer >> 4));
    }
    return out;
}

}  // namespace psvp::proto
