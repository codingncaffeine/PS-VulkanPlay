// Base64 as the protocol uses it: the standard alphabet with '=' padding
// (RFC 4648 section 4). Account IDs and several session headers travel this way.
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psvp::proto {

enum class Base64Error {
    bad_length,     // not a multiple of four characters
    bad_character,  // a character outside the alphabet
    bad_padding,    // '=' anywhere but the last one or two places
    non_canonical,  // unused bits before the padding are not zero
};

[[nodiscard]] std::string_view to_string(Base64Error error) noexcept;

// Always padded, never wrapped.
[[nodiscard]] std::string base64_encode(std::span<const std::uint8_t> bytes);

// Strict: accepts exactly what base64_encode can produce. Text from the network
// that is anything else is refused rather than guessed at.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, Base64Error> base64_decode(std::string_view text);

}  // namespace psvp::proto
