// The key exchange that gives a stream its keys: each side makes a key pair on
// the curve secp256k1, sends its public point with a signature, and both
// arrive at the same 32-byte secret. docs/protocol/stream-setup.md describes it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "proto/console.h"

namespace psvp::proto {

inline constexpr std::size_t kPublicKeySize = 65;  // 04, then X and Y, 32 bytes each
inline constexpr std::size_t kPrivateKeySize = 32;

using PublicKey = std::array<std::uint8_t, kPublicKeySize>;
using SharedSecret = std::array<std::uint8_t, 32>;
using KeySignature = std::array<std::uint8_t, 32>;

// One side's key pair.
class KeyExchange {
public:
    // A new pair from the system's cryptographic generator.
    [[nodiscard]] static KeyExchange generate();
    // The pair of a given private key, for tests. Nothing for a key that is zero or not below the curve's order.
    [[nodiscard]] static std::optional<KeyExchange> from_private_key(std::span<const std::uint8_t, kPrivateKeySize> key);

    [[nodiscard]] const PublicKey& public_key() const noexcept { return public_; }
    // The secret shared with the owner of `remote_public`. Nothing for bytes that are not a point of the curve.
    [[nodiscard]] std::optional<SharedSecret> shared_secret(std::span<const std::uint8_t> remote_public) const;

private:
    KeyExchange() = default;

    std::shared_ptr<void> key_;  // the library's key object
    PublicKey public_{};
};

// What a public key is sent with: HMAC-SHA256 of the key's bytes under the
// handshake key, which only the two sides of the session know.
[[nodiscard]] KeySignature sign_public_key(const Key16& handshake_key, std::span<const std::uint8_t> public_key);

}  // namespace psvp::proto
