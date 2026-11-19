// The launch specification: the text in which the client tells the console
// what stream it wants (picture size, frame rate, bitrate, codec) and hands it
// the handshake key. docs/protocol/stream-setup.md describes it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "proto/console.h"
#include "proto/rp_cipher.h"
#include "proto/session.h"

namespace psvp::proto {

struct LaunchSpec {
    ConsoleKind kind = ConsoleKind::ps5;
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t max_fps = 60;
    std::uint32_t bitrate_kbps = 10000;
    std::uint32_t mtu = 1454;          // the largest packet the console may send
    std::uint32_t round_trip_ms = 1;
    Codec codec = Codec::h264;         // a PS5 is told; a PS4 always sends H.264
    Key16 handshake_key{};
};

// The text itself. Everything in it that is not one of the fields above is fixed.
[[nodiscard]] std::string format_launch_spec(const LaunchSpec& spec);

// The form it travels in: the text and a closing zero byte, masked with the
// session cipher's key stream for message number 0, in base64.
[[nodiscard]] std::string seal_launch_spec(const RpCipher& session_cipher, std::string_view text);
// The console's way back. Nothing for what is not base64 or does not end in the zero byte.
[[nodiscard]] std::optional<std::string> open_launch_spec(const RpCipher& session_cipher, std::string_view sealed);

// The handshake key inside an opened launch specification, if it has one.
[[nodiscard]] std::optional<Key16> launch_spec_handshake_key(std::string_view text);

}  // namespace psvp::proto
