// The stream's picture and sound packets: their headers. The payload behind
// the header is encrypted. docs/protocol/video.md describes the layouts.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "proto/console.h"

namespace psvp::proto::transport {

struct StreamMediaPacket {
    bool video = false;
    bool unit_info = false;         // three more header bytes precede the payload
    std::uint16_t packet_index = 0;
    std::uint16_t frame_index = 0;
    std::uint16_t unit_index = 0;
    std::uint16_t units_total = 1;  // in the frame (picture) or in the packet (sound)
    std::uint8_t codec = 0;
    std::uint32_t key_position = 0; // its low 32 bits, as the packet carries them
    // Picture packets.
    std::uint16_t units_repair = 0;
    std::uint16_t video_word = 0;      // not known
    std::uint8_t adaptive_stream = 0;  // which of the stream information's picture sizes
    // Sound packets.
    std::uint8_t unit_size = 0;        // bytes per frame in the payload
    std::uint8_t source_frames = 0;    // frames first sent in this packet
    std::uint8_t repeated_frames = 0;  // earlier frames sent again
    bool haptics = false;              // PS5 only: the payload drives a controller's haptics
    std::uint8_t sound_stream = 0;     // PS5 only: the byte that says so (0 the sound, 2 the haptics)
    std::size_t header_size = 0;
    std::span<const std::uint8_t> payload;  // a view into the datagram, still encrypted
};

inline constexpr std::uint8_t kCodecOpus = 5;
// How far behind a packet's key position the key stream of its payload begins.
inline constexpr std::uint64_t kPayloadKeyOffset = 16;
// The bytes of a header for these kinds of console.
[[nodiscard]] constexpr std::size_t stream_media_header_size(ConsoleKind kind, bool video, bool unit_info) noexcept {
    return 18U + (video ? 3U : (kind == ConsoleKind::ps5 ? 2U : 1U)) + (unit_info ? 3U : 0U);
}

// Nothing for a datagram that is not a picture or sound packet, or is shorter than its header and one byte.
[[nodiscard]] std::optional<StreamMediaPacket> parse_stream_media(ConsoleKind kind, std::span<const std::uint8_t> datagram);

// A packet as a console sends it, for the mock console and the tests: the header from `packet`
// (its code left zero), then `payload` as it is. Nothing for values that do not fit their fields.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_stream_media(ConsoleKind kind, const StreamMediaPacket& packet,
                                                                          std::span<const std::uint8_t> payload);

}  // namespace psvp::proto::transport
