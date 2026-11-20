#include "proto/media_packet.h"

#include "proto/transport.h"

namespace psvp::proto::transport {

std::optional<StreamMediaPacket> parse_stream_media(ConsoleKind kind, std::span<const std::uint8_t> datagram) {
    if (datagram.empty()) return std::nullopt;
    const std::uint8_t packet_kind_byte = datagram[0] & 0x0f;
    if (packet_kind_byte != packet_kind::kVideo && packet_kind_byte != packet_kind::kAudio) return std::nullopt;
    StreamMediaPacket packet;
    packet.video = packet_kind_byte == packet_kind::kVideo;
    packet.unit_info = (datagram[0] & 0x10) != 0;
    const bool ps5 = kind == ConsoleKind::ps5;
    packet.header_size = stream_media_header_size(kind, packet.video, packet.unit_info);
    if (datagram.size() < packet.header_size + 1) return std::nullopt;
    const auto get16 = [&](std::size_t at) { return static_cast<std::uint16_t>((std::uint16_t{datagram[at]} << 8) | datagram[at + 1]); };
    const auto get32 = [&](std::size_t at) {
        return (std::uint32_t{datagram[at]} << 24) | (std::uint32_t{datagram[at + 1]} << 16) | (std::uint32_t{datagram[at + 2]} << 8) |
               std::uint32_t{datagram[at + 3]};
    };
    packet.packet_index = get16(1);
    packet.frame_index = get16(3);
    const std::uint32_t units = get32(5);
    if (packet.video) {
        packet.unit_index = static_cast<std::uint16_t>((units >> 21) & 0x7ffU);
        packet.units_total = static_cast<std::uint16_t>(((units >> 10) & 0x7ffU) + 1U);
        packet.units_repair = static_cast<std::uint16_t>(units & 0x3ffU);
        packet.video_word = get16(18);
        packet.adaptive_stream = static_cast<std::uint8_t>(datagram[20] >> 5);
    } else {
        packet.unit_index = static_cast<std::uint16_t>((units >> 24) & 0xffU);
        packet.units_total = static_cast<std::uint16_t>(((units >> 16) & 0xffU) + 1U);
        packet.unit_size = static_cast<std::uint8_t>((units >> 8) & 0xffU);
        packet.repeated_frames = static_cast<std::uint8_t>((units >> 4) & 0x0fU);
        packet.source_frames = static_cast<std::uint8_t>(units & 0x0fU);
        if (ps5) packet.haptics = datagram[packet.header_size - 1] == 0x02;
    }
    packet.codec = datagram[9];
    packet.key_position = get32(14);
    packet.payload = datagram.subspan(packet.header_size);
    return packet;
}

std::optional<std::vector<std::uint8_t>> encode_stream_media(ConsoleKind kind, const StreamMediaPacket& packet,
                                                             std::span<const std::uint8_t> payload) {
    const bool ps5 = kind == ConsoleKind::ps5;
    std::uint32_t units = 0;
    if (packet.video) {
        if (packet.unit_index > 0x7ff || packet.units_total < 1 || packet.units_total > 0x800 || packet.units_repair > 0x3ff ||
            packet.adaptive_stream > 7) {
            return std::nullopt;
        }
        units = (std::uint32_t{packet.unit_index} << 21) | (std::uint32_t{packet.units_total} - 1U) << 10 | packet.units_repair;
    } else {
        if (packet.unit_index > 0xff || packet.units_total < 1 || packet.units_total > 0x100 || packet.repeated_frames > 15 ||
            packet.source_frames > 15 || (packet.haptics && !ps5)) {
            return std::nullopt;
        }
        units = (std::uint32_t{packet.unit_index} << 24) | (std::uint32_t{packet.units_total} - 1U) << 16 |
                std::uint32_t{packet.unit_size} << 8 | std::uint32_t{packet.repeated_frames} << 4 | packet.source_frames;
    }
    const std::size_t header = stream_media_header_size(kind, packet.video, packet.unit_info);
    std::vector<std::uint8_t> out(header, 0);
    out[0] = static_cast<std::uint8_t>((packet.video ? packet_kind::kVideo : packet_kind::kAudio) | (packet.unit_info ? 0x10 : 0));
    const auto put16 = [&](std::size_t at, std::uint16_t value) {
        out[at] = static_cast<std::uint8_t>(value >> 8);
        out[at + 1] = static_cast<std::uint8_t>(value);
    };
    const auto put32 = [&](std::size_t at, std::uint32_t value) {
        put16(at, static_cast<std::uint16_t>(value >> 16));
        put16(at + 2, static_cast<std::uint16_t>(value));
    };
    put16(1, packet.packet_index);
    put16(3, packet.frame_index);
    put32(5, units);
    out[9] = packet.codec;
    put32(14, packet.key_position);
    if (packet.video) {
        put16(18, packet.video_word);
        out[20] = static_cast<std::uint8_t>(packet.adaptive_stream << 5);
    } else if (ps5) {
        out[header - 1] = packet.haptics ? 0x02 : 0x00;
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

}  // namespace psvp::proto::transport
