#include "proto/media_sender.h"

#include <algorithm>

#include "proto/erasure.h"

namespace psvp::proto::transport {
namespace {

std::uint64_t step(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

std::uint64_t pattern_state(std::uint64_t seed, std::uint16_t index, std::uint64_t stream) {
    std::uint64_t state = (seed * 0x9e3779b97f4a7c15ULL) ^ (std::uint64_t{index} << 20) ^ stream ^ 0x5053565061747465ULL;
    if (state == 0) state = 1;
    step(state);
    step(state);
    return state;
}

constexpr std::size_t kPatternHead = 13;

}  // namespace

std::optional<std::vector<std::uint8_t>> MediaSender::finish(StreamMediaPacket& packet, std::vector<std::uint8_t> payload) {
    // The payload's key stream begins sixteen bytes behind the packet's key position.
    const std::uint64_t position = keys_->take(payload.size() + kPayloadKeyOffset);
    packet.key_position = static_cast<std::uint32_t>(position);
    keys_->cipher.apply_key_stream(position + kPayloadKeyOffset, payload);
    auto datagram = encode_stream_media(kind_, packet, payload);
    if (!datagram || !seal_datagram(keys_->cipher, position, *datagram)) return std::nullopt;
    return datagram;
}

Datagrams MediaSender::video_frame(std::uint16_t index, std::uint8_t adaptive_stream, std::uint8_t codec, std::span<const std::uint8_t> frame,
                                   std::size_t unit_data, unsigned repair_units) {
    Datagrams out;
    if (frame.empty() || unit_data == 0 || unit_data > 0xffff) return out;
    const std::size_t source = (frame.size() + unit_data - 1) / unit_data;
    if (source + repair_units > 0x800 || repair_units > 0x3ff) return out;
    // Every unit has one size: two bytes that count its padding, then up to unit_data bytes of the frame.
    const std::size_t unit_size = 2 + std::min(unit_data, frame.size());
    const std::size_t stride = (unit_size + 15) / 16 * 16;
    std::vector<std::uint8_t> units((source + repair_units) * stride, 0);
    std::vector<std::size_t> sizes(source + repair_units, unit_size);
    for (std::size_t i = 0; i < source; ++i) {
        const auto part = frame.subspan(i * unit_data, std::min(unit_data, frame.size() - i * unit_data));
        const std::size_t padding = unit_size - 2 - part.size();
        units[i * stride] = static_cast<std::uint8_t>(padding >> 8);
        units[i * stride + 1] = static_cast<std::uint8_t>(padding);
        std::copy(part.begin(), part.end(), units.begin() + static_cast<std::ptrdiff_t>(i * stride + 2));
        sizes[i] = 2 + part.size();
    }
    if (repair_units > 0 &&
        !make_repair_units(units, unit_size, stride, static_cast<unsigned>(source), repair_units)) {
        return out;
    }
    for (std::size_t i = 0; i < source + repair_units; ++i) {
        StreamMediaPacket packet;
        packet.video = true;
        packet.packet_index = video_packets_++;
        packet.frame_index = index;
        packet.unit_index = static_cast<std::uint16_t>(i);
        packet.units_total = static_cast<std::uint16_t>(source + repair_units);
        packet.units_repair = static_cast<std::uint16_t>(repair_units);
        packet.codec = codec;
        packet.adaptive_stream = adaptive_stream;
        const auto unit = std::span<const std::uint8_t>(units).subspan(i * stride, sizes[i]);
        auto datagram = finish(packet, {unit.begin(), unit.end()});
        if (!datagram) return {};
        out.push_back(std::move(*datagram));
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> MediaSender::sound_packet(std::uint16_t index, std::span<const std::vector<std::uint8_t>> fresh,
                                                                   std::span<const std::vector<std::uint8_t>> repeated, bool haptics) {
    if (fresh.empty() || fresh.size() > 15 || repeated.size() > 15) return std::nullopt;
    const std::size_t size = fresh.front().size();
    if (size == 0 || size > 0xff) return std::nullopt;
    std::vector<std::uint8_t> payload;
    for (const auto* frames : {&fresh, &repeated}) {
        for (const auto& frame : *frames) {
            if (frame.size() != size) return std::nullopt;
            payload.insert(payload.end(), frame.begin(), frame.end());
        }
    }
    StreamMediaPacket packet;
    packet.packet_index = audio_packets_++;
    packet.frame_index = index;
    packet.units_total = static_cast<std::uint16_t>(fresh.size() + repeated.size());
    packet.codec = kCodecOpus;
    packet.unit_size = static_cast<std::uint8_t>(size);
    packet.source_frames = static_cast<std::uint8_t>(fresh.size());
    packet.repeated_frames = static_cast<std::uint8_t>(repeated.size());
    packet.haptics = haptics;
    return finish(packet, std::move(payload));
}

std::vector<std::uint8_t> pattern_video_frame(std::uint64_t seed, std::uint16_t index, std::size_t about_bytes, bool keyframe) {
    std::uint64_t state = pattern_state(seed, index, 0x7669);
    const std::size_t size = kPatternHead + about_bytes / 2 + (about_bytes == 0 ? 0 : step(state) % about_bytes);
    std::vector<std::uint8_t> frame(size);
    // The start of an H.264 slice, as far as a client looks into one: a start code; a keyframe's
    // unit (type 5, an I slice) or a later picture's (type 1, a P slice that refers to the picture
    // before it and changes nothing about that).
    const std::uint8_t head[kPatternHead] = {0, 0, 0, 1, static_cast<std::uint8_t>(keyframe ? 0x65 : 0x41),
                                             static_cast<std::uint8_t>(keyframe ? 0x88 : 0x9a), 0xe7,
                                             static_cast<std::uint8_t>(index >> 8), static_cast<std::uint8_t>(index),
                                             static_cast<std::uint8_t>(size >> 24), static_cast<std::uint8_t>(size >> 16),
                                             static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
    std::copy(std::begin(head), std::end(head), frame.begin());
    for (std::size_t i = kPatternHead; i < size; ++i) frame[i] = static_cast<std::uint8_t>(step(state) >> 24);
    return frame;
}

std::vector<std::uint8_t> pattern_sound_frame(std::uint64_t seed, std::uint16_t index, std::size_t bytes, bool haptics) {
    std::uint64_t state = pattern_state(seed, index, haptics ? 0x6861 : 0x736f);
    std::vector<std::uint8_t> frame(bytes);
    for (std::uint8_t& byte : frame) byte = static_cast<std::uint8_t>(step(state) >> 24);
    return frame;
}

}  // namespace psvp::proto::transport
