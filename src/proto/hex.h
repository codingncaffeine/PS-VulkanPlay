// Hexadecimal text, as the protocol writes keys and addresses.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psvp::proto {

// Lowercase, two characters per byte.
[[nodiscard]] std::string hex_encode(std::span<const std::uint8_t> bytes);

// Accepts both cases. Nothing is returned for an odd length or a character that is not a hex digit.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> hex_decode(std::string_view text);

}  // namespace psvp::proto
