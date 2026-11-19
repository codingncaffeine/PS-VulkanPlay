#include "proto/hex.h"

namespace psvp::proto {
namespace {

constexpr int nibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string hex_encode(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0xf]);
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> hex_decode(std::string_view text) {
    if (text.size() % 2 != 0) return std::nullopt;
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = nibble(text[i]);
        const int low = nibble(text[i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

}  // namespace psvp::proto
