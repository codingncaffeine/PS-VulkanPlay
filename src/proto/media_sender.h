// The console's side of the picture and sound packets: cuts a frame into
// units, adds the repair units, encrypts and seals each packet. For the mock
// console and the tests; a client never sends these.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "proto/media_packet.h"
#include "proto/probe.h"
#include "proto/transport.h"

namespace psvp::proto::transport {

class MediaSender {
public:
    MediaSender(ConsoleKind kind, std::shared_ptr<SendKeys> keys) : kind_(kind), keys_(std::move(keys)) {}

    // The packets of one picture frame: its source units in order, then `repair_units` repair units.
    // Each source unit carries up to `unit_data` bytes of the frame. Nothing for sizes the packets cannot say.
    [[nodiscard]] Datagrams video_frame(std::uint16_t index, std::uint8_t adaptive_stream, std::uint8_t codec,
                                        std::span<const std::uint8_t> frame, std::size_t unit_data, unsigned repair_units);
    // One sound packet: `fresh` frames from `index` on, then the `repeated` frames that came just before them.
    // All frames are of one size. Nothing for sizes the packet cannot say.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> sound_packet(std::uint16_t index, std::span<const std::vector<std::uint8_t>> fresh,
                                                                        std::span<const std::vector<std::uint8_t>> repeated, bool haptics,
                                                                        std::uint8_t stream = 0);

private:
    std::optional<std::vector<std::uint8_t>> finish(StreamMediaPacket& packet, std::vector<std::uint8_t> payload);

    ConsoleKind kind_;
    std::shared_ptr<SendKeys> keys_;
    std::uint16_t video_packets_ = 0;
    std::uint16_t audio_packets_ = 0;
};

// What the mock console sends: frame `index` of the stream with this seed. A picture frame begins
// like a coded H.264 picture (a start code, then a keyframe's first bytes or a later picture's),
// then names its index and its size; the rest, like all of a sound frame, is fixed by seed and index.
// A keyframe is told by its fifth byte: 0x65.
[[nodiscard]] std::vector<std::uint8_t> pattern_video_frame(std::uint64_t seed, std::uint16_t index, std::size_t about_bytes, bool keyframe);
[[nodiscard]] std::vector<std::uint8_t> pattern_sound_frame(std::uint64_t seed, std::uint16_t index, std::size_t bytes, bool haptics);

}  // namespace psvp::proto::transport
