// The two console families and the constants that differ between them.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace psvp::proto {

enum class ConsoleKind { ps4, ps5 };

using Key16 = std::array<std::uint8_t, 16>;

inline constexpr std::uint16_t kDiscoveryPortPs4 = 987;
inline constexpr std::uint16_t kDiscoveryPortPs5 = 9302;
// Registration and sessions, UDP and TCP, on both families.
inline constexpr std::uint16_t kSessionPort = 9295;

// The oldest PS4 system software this client speaks to (8.00, as discovery reports it).
// Older software uses an earlier protocol that is not implemented.
inline constexpr long kOldestPs4SystemVersion = 8000000;

[[nodiscard]] constexpr std::string_view to_string(ConsoleKind kind) noexcept {
    return kind == ConsoleKind::ps5 ? "PS5" : "PS4";
}

[[nodiscard]] constexpr std::uint16_t discovery_port(ConsoleKind kind) noexcept {
    return kind == ConsoleKind::ps5 ? kDiscoveryPortPs5 : kDiscoveryPortPs4;
}

// The version of the discovery protocol each family answers to.
[[nodiscard]] constexpr std::string_view discovery_version(ConsoleKind kind) noexcept {
    return kind == ConsoleKind::ps5 ? "00030010" : "00020020";
}

// The Remote Play protocol version sent in the RP-Version header.
[[nodiscard]] constexpr std::string_view rp_version(ConsoleKind kind) noexcept {
    return kind == ConsoleKind::ps5 ? "1.0" : "10.0";
}

}  // namespace psvp::proto
