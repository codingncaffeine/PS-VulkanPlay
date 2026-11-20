#include "proto/sound.h"

namespace psvp::proto::transport {

void SoundReceiver::on_packet(const StreamMediaPacket& packet, std::span<const std::uint8_t> payload, std::vector<SoundFrame>& out) {
    if (packet.video) return;
    ++counters_.packets;
    const unsigned source = packet.source_frames;
    const unsigned repeated = packet.repeated_frames;
    if (packet.codec != kCodecOpus || payload.empty() || packet.unit_size == 0 || source + repeated != packet.units_total ||
        payload.size() != std::size_t{packet.unit_size} * packet.units_total) {
        ++counters_.packets_unusable;
        return;
    }
    if (packet.frame_index > 0x8000) startup_ = false;
    // The payload holds the new frames first and then the repeated ones; in time the repeated ones
    // come first: they are the frames just before this packet's.
    for (unsigned i = 0; i < repeated + source; ++i) {
        const bool again = i < repeated;
        const unsigned slot = again ? source + i : i - repeated;
        const int signed_index = again ? int{packet.frame_index} - static_cast<int>(repeated) + static_cast<int>(i)
                                       : int{packet.frame_index} + static_cast<int>(i - repeated);
        // A stream's frames count from 1: at its start a repeated frame may name one from before it.
        if (again && startup_ && signed_index < 1) continue;
        const auto index = static_cast<std::uint16_t>(signed_index);
        std::uint32_t gap = 0;
        if (last_) {
            const auto ahead = static_cast<std::uint16_t>(index - *last_);
            if (ahead == 0 || ahead >= 0x8000U) continue;  // handed on already
            gap = ahead - 1U;
        }
        last_ = index;
        counters_.frames_lost += gap;
        ++counters_.frames;
        if (again) ++counters_.frames_recovered;
        const auto data = payload.subspan(std::size_t{slot} * packet.unit_size, packet.unit_size);
        out.push_back(SoundFrame{index, packet.haptics, again, gap, {data.begin(), data.end()}});
    }
}

}  // namespace psvp::proto::transport
