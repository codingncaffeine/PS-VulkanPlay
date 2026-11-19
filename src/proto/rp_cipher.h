// The cipher that protects registration (and, later, the session handshake).
// docs/protocol/registration.md describes how its key and nonce are made.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "proto/console.h"

namespace psvp::proto {

// AES-128 in CFB mode. The key is secret; the nonce travels with the request in
// wrapped form (wrap_nonce) and gives each message its starting vector.
struct RpCipher {
    ConsoleKind kind = ConsoleKind::ps5;
    Key16 key{};
    Key16 nonce{};
};

// How many key choices each offset of a registration request can select.
inline constexpr std::size_t kKeyOffsets = 32;

// The cipher of a registration: the key is picked from a fixed table by
// `key_offset` (below kKeyOffsets) and mixed with the eight-digit PIN the
// console shows. Nothing is returned for an offset out of range.
[[nodiscard]] std::optional<RpCipher> make_regist_cipher(ConsoleKind kind, const Key16& nonce,
                                                         std::size_t key_offset, std::uint32_t pin);

// The form in which the nonce is written into a registration request, and the
// console's way back from it. `wrap_offset` is below kKeyOffsets.
[[nodiscard]] std::optional<Key16> wrap_nonce(ConsoleKind kind, const Key16& nonce, std::size_t wrap_offset);
[[nodiscard]] std::optional<Key16> unwrap_nonce(ConsoleKind kind, const Key16& wrapped, std::size_t wrap_offset);

// The cipher of a session: key and nonce both come from the nonce the console
// hands out when the session is requested and from the secret shared at registration.
[[nodiscard]] RpCipher make_session_cipher(ConsoleKind kind, const Key16& console_nonce, const Key16& secret);

// The starting vector of message number `counter`.
[[nodiscard]] Key16 cipher_iv(const RpCipher& cipher, std::uint64_t counter);

[[nodiscard]] std::vector<std::uint8_t> encrypt(const RpCipher& cipher, std::uint64_t counter,
                                                std::span<const std::uint8_t> plain);
[[nodiscard]] std::vector<std::uint8_t> decrypt(const RpCipher& cipher, std::uint64_t counter,
                                                std::span<const std::uint8_t> sealed);

// Sixteen bytes from the system's cryptographic generator.
[[nodiscard]] Key16 random_key();

}  // namespace psvp::proto
