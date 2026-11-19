// The keys of one direction of a stream connection: a key stream for what is
// encrypted and a four-byte authentication code for every packet.
// docs/protocol/stream-setup.md describes how they are made and used.
#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>

#include "proto/console.h"
#include "proto/key_exchange.h"

namespace psvp::proto {

using Mac4 = std::array<std::uint8_t, 4>;

// Which direction a set of keys belongs to.
inline constexpr std::uint8_t kClientDirection = 2;   // what the client sends
inline constexpr std::uint8_t kConsoleDirection = 3;  // what the console sends

// The authentication key changes every this many bytes of key position.
inline constexpr std::uint64_t kMacKeySpan = 45000;

class StreamCipher {
public:
    StreamCipher(std::uint8_t direction, const Key16& handshake_key, const SharedSecret& secret);

    [[nodiscard]] const Key16& key() const noexcept { return key_; }
    [[nodiscard]] const Key16& iv() const noexcept { return iv_; }

    // The authentication key in force at a key position.
    [[nodiscard]] Key16 mac_key(std::uint64_t key_position) const;
    // The authentication code of `parts`, taken as one run of bytes, at a key position.
    [[nodiscard]] Mac4 mac(std::uint64_t key_position, std::initializer_list<std::span<const std::uint8_t>> parts) const;
    // Encrypts or decrypts in place: the same operation both ways.
    void apply_key_stream(std::uint64_t key_position, std::span<std::uint8_t> data) const;

private:
    Key16 key_{};
    Key16 iv_{};
    Key16 mac_base_{};
};

// A packet carries the low 32 bits of its key position. This follows them
// through their wrap-around, forwards and (for a late packet) backwards.
class KeyPositionTracker {
public:
    // The full position a packet's 32 bits stand for, given what was last accepted.
    [[nodiscard]] std::uint64_t extend(std::uint32_t low) const noexcept;
    // To be called once the packet proved authentic.
    void accept(std::uint64_t position) noexcept { last_ = position; }
    [[nodiscard]] std::uint64_t last() const noexcept { return last_; }

private:
    std::uint64_t last_ = 0;
};

}  // namespace psvp::proto
