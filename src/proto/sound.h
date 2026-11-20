// Sound and haptics packets: each carries whole frames, new ones and earlier
// ones sent again. Splits a packet into its frames and hands every frame on
// once, in order. docs/protocol/video.md describes the packet.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "proto/media_packet.h"

namespace psvp::proto::transport {

struct SoundFrame {
    std::uint16_t index = 0;
    bool haptics = false;
    bool recovered = false;         // taken from a later packet's repeated frames
    std::uint32_t lost_before = 0;  // frames that never came since the one handed on before
    std::vector<std::uint8_t> data;
};

struct SoundCounters {
    std::uint64_t frames = 0;
    std::uint64_t frames_recovered = 0;
    std::uint64_t frames_lost = 0;
    std::uint64_t packets = 0;
    std::uint64_t packets_unusable = 0;  // of another codec, or whose sizes do not add up
};

// One stream of frames: the sound, or the haptics.
class SoundReceiver {
public:
    // `payload` is the packet's payload, decrypted. Frames that are new are added to `out`, oldest first.
    void on_packet(const StreamMediaPacket& packet, std::span<const std::uint8_t> payload, std::vector<SoundFrame>& out);
    [[nodiscard]] const SoundCounters& counters() const noexcept { return counters_; }

private:
    bool startup_ = true;               // no frame index has passed half the number space yet
    std::optional<std::uint16_t> last_; // the newest frame handed on
    SoundCounters counters_;
};

}  // namespace psvp::proto::transport
