#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#include "check.h"
#include "proto/erasure.h"
#include "proto/frame_assembler.h"
#include "proto/hex.h"
#include "proto/input.h"
#include "proto/media_packet.h"
#include "proto/sound.h"

using namespace psvp::proto;
using namespace psvp::proto::transport;
using namespace std::chrono_literals;

namespace {

std::string hex(const std::vector<std::uint8_t>& bytes) { return hex_encode(bytes); }

const TimePoint kT0{};

std::vector<std::uint8_t> bytes_of(std::size_t size, std::uint8_t first) {
    std::vector<std::uint8_t> out(size);
    std::iota(out.begin(), out.end(), first);
    return out;
}

// A frame cut into units the way docs/protocol/video.md says, written here a second time on
// purpose: units of `unit_data` bytes of the frame behind a two-byte padding count.
struct Units {
    std::vector<StreamMediaPacket> packets;
    std::vector<std::vector<std::uint8_t>> payloads;
};

Units cut(std::uint16_t frame_index, const std::vector<std::uint8_t>& frame, std::size_t unit_data, unsigned repair) {
    Units units;
    const std::size_t source = (frame.size() + unit_data - 1) / unit_data;
    const std::size_t unit_size = 2 + std::min(unit_data, frame.size());
    std::vector<std::uint8_t> all((source + repair) * unit_size, 0);
    for (std::size_t i = 0; i < source; ++i) {
        const std::size_t length = std::min(unit_data, frame.size() - i * unit_data);
        const std::size_t padding = unit_size - 2 - length;
        all[i * unit_size] = static_cast<std::uint8_t>(padding >> 8);
        all[i * unit_size + 1] = static_cast<std::uint8_t>(padding);
        std::copy_n(frame.begin() + static_cast<std::ptrdiff_t>(i * unit_data), length, all.begin() + static_cast<std::ptrdiff_t>(i * unit_size + 2));
        units.payloads.emplace_back(all.begin() + static_cast<std::ptrdiff_t>(i * unit_size),
                                    all.begin() + static_cast<std::ptrdiff_t>(i * unit_size + 2 + length));
    }
    if (repair > 0) {
        CHECK(make_repair_units(all, unit_size, unit_size, static_cast<unsigned>(source), repair));
        for (std::size_t i = source; i < source + repair; ++i) {
            units.payloads.emplace_back(all.begin() + static_cast<std::ptrdiff_t>(i * unit_size),
                                        all.begin() + static_cast<std::ptrdiff_t>((i + 1) * unit_size));
        }
    }
    for (std::size_t i = 0; i < source + repair; ++i) {
        StreamMediaPacket packet;
        packet.video = true;
        packet.frame_index = frame_index;
        packet.unit_index = static_cast<std::uint16_t>(i);
        packet.units_total = static_cast<std::uint16_t>(source + repair);
        packet.units_repair = static_cast<std::uint16_t>(repair);
        units.packets.push_back(packet);
    }
    return units;
}

// Feeds the units named, in that order.
void feed(FrameAssembler& assembler, const Units& units, std::initializer_list<unsigned> which, TimePoint now, FrameOutput& out) {
    for (const unsigned i : which) assembler.on_packet(units.packets[i], units.payloads[i], now, out);
}

StreamMediaPacket sound_packet(std::uint16_t index, std::uint8_t unit_size, std::uint8_t fresh, std::uint8_t repeated) {
    StreamMediaPacket packet;
    packet.frame_index = index;
    packet.units_total = static_cast<std::uint16_t>(fresh + repeated);
    packet.codec = kCodecOpus;
    packet.unit_size = unit_size;
    packet.source_frames = fresh;
    packet.repeated_frames = repeated;
    return packet;
}

// A sound packet's payload in which every frame is two bytes: its index.
std::vector<std::uint8_t> sound_payload(const StreamMediaPacket& packet) {
    std::vector<std::uint8_t> payload;
    const auto put = [&](int index) {
        payload.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(index) >> 8));
        payload.push_back(static_cast<std::uint8_t>(index));
    };
    for (int i = 0; i < packet.source_frames; ++i) put(packet.frame_index + i);
    for (int i = 0; i < packet.repeated_frames; ++i) put(packet.frame_index - packet.repeated_frames + i);
    return payload;
}

std::vector<std::uint16_t> indices(const std::vector<SoundFrame>& frames) {
    std::vector<std::uint16_t> out;
    for (const SoundFrame& frame : frames) {
        out.push_back(frame.index);
        // Every frame carries the bytes that were sent for its index.
        CHECK(frame.data.size() == 2 && frame.data[0] == (frame.index >> 8) && frame.data[1] == (frame.index & 0xff));
    }
    return out;
}

}  // namespace

// Byte for byte as docs/protocol/video.md lays them out.
PSVP_TEST(media_picture_packet_is_exact) {
    StreamMediaPacket packet;
    packet.video = true;
    packet.packet_index = 0x0102;
    packet.frame_index = 0x0304;
    packet.unit_index = 5;
    packet.units_total = 8;
    packet.units_repair = 2;
    packet.codec = 0x06;
    packet.key_position = 0x0a0b0c0d;
    packet.video_word = 0x1122;
    packet.adaptive_stream = 3;
    const std::vector<std::uint8_t> payload{0xaa, 0xbb, 0xcc, 0xdd};
    const auto datagram = encode_stream_media(ConsoleKind::ps5, packet, payload);
    CHECK(datagram.has_value());
    if (!datagram) return;
    CHECK(hex(*datagram) == "02"           // a picture packet
                            "0102"         // packet index
                            "0304"         // frame index
                            "00a01c02"     // unit 5 << 21 | (8 - 1) << 10 | 2 repair units
                            "06"           // codec
                            "00000000"     // authentication code
                            "0a0b0c0d"     // key position
                            "1122"         // not known
                            "60"           // adaptive stream 3 in the top three bits
                            "aabbccdd");
    const auto parsed = parse_stream_media(ConsoleKind::ps5, *datagram);
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(parsed->video && !parsed->unit_info && parsed->packet_index == 0x0102 && parsed->frame_index == 0x0304);
    CHECK(parsed->unit_index == 5 && parsed->units_total == 8 && parsed->units_repair == 2 && parsed->codec == 6);
    CHECK(parsed->key_position == 0x0a0b0c0d && parsed->video_word == 0x1122 && parsed->adaptive_stream == 3);
    CHECK(parsed->header_size == 21 && parsed->payload.size() == 4 && parsed->payload[0] == 0xaa);
    // A PS4's picture packet is the same.
    CHECK(encode_stream_media(ConsoleKind::ps4, packet, payload) == datagram);
    // The largest values the fields hold.
    packet.unit_index = 0x7ff;
    packet.units_total = 0x800;
    packet.units_repair = 0x3ff;
    const auto widest = encode_stream_media(ConsoleKind::ps5, packet, payload);
    CHECK(widest && hex(*widest).substr(10, 8) == "ffffffff");
    const auto back = parse_stream_media(ConsoleKind::ps5, *widest);
    CHECK(back && back->unit_index == 0x7ff && back->units_total == 0x800 && back->units_repair == 0x3ff);
    packet.units_total = 0x801;
    CHECK(!encode_stream_media(ConsoleKind::ps5, packet, payload));
}

PSVP_TEST(media_sound_packet_is_exact) {
    StreamMediaPacket packet = sound_packet(0x0304, 0x28, 1, 2);
    packet.packet_index = 0x0102;
    packet.key_position = 0x0a0b0c0d;
    const std::vector<std::uint8_t> payload{0xaa, 0xbb};
    const auto ps4 = encode_stream_media(ConsoleKind::ps4, packet, payload);
    CHECK(ps4.has_value());
    if (!ps4) return;
    CHECK(hex(*ps4) == "03"           // a sound packet
                       "0102" "0304"  // packet index, frame index
                       "00022821"     // unit 0, 3 - 1 units, frames of 0x28 bytes, 2 repeated, 1 new
                       "05"           // Opus
                       "00000000" "0a0b0c0d"
                       "00"           // not known
                       "aabb");
    // A PS5 has one more byte, which tells haptics from sound.
    const auto ps5 = encode_stream_media(ConsoleKind::ps5, packet, payload);
    packet.haptics = true;
    const auto haptics = encode_stream_media(ConsoleKind::ps5, packet, payload);
    CHECK(ps5 && haptics);
    if (!ps5 || !haptics) return;
    CHECK(hex(*ps5) == hex(*ps4).substr(0, 38) + "00" + "aabb");
    CHECK(hex(*haptics) == hex(*ps4).substr(0, 38) + "02" + "aabb");
    CHECK(!encode_stream_media(ConsoleKind::ps4, packet, payload));  // a PS4 has no haptics
    const auto parsed = parse_stream_media(ConsoleKind::ps5, *haptics);
    CHECK(parsed && !parsed->video && parsed->haptics && parsed->unit_size == 0x28 && parsed->source_frames == 1 &&
          parsed->repeated_frames == 2 && parsed->units_total == 3 && parsed->codec == kCodecOpus && parsed->header_size == 20);
    const auto plain = parse_stream_media(ConsoleKind::ps5, *ps5);
    CHECK(plain && !plain->haptics);
    // Read as a PS4's packet, the same bytes have a header of nineteen and no haptics.
    const auto as_ps4 = parse_stream_media(ConsoleKind::ps4, *haptics);
    CHECK(as_ps4 && !as_ps4->haptics && as_ps4->header_size == 19 && as_ps4->payload.size() == 3);
}

PSVP_TEST(media_packets_too_short_or_of_another_kind_are_refused) {
    StreamMediaPacket video;
    video.video = true;
    // A packet is its header and at least one byte: 22 bytes for a picture packet.
    const auto smallest = encode_stream_media(ConsoleKind::ps5, video, bytes_of(1, 0));
    CHECK(smallest && smallest->size() == 22 && parse_stream_media(ConsoleKind::ps5, *smallest));
    CHECK(!parse_stream_media(ConsoleKind::ps5, std::span<const std::uint8_t>(*smallest).first(21)));
    video.unit_info = true;  // three more header bytes
    const auto with_info = encode_stream_media(ConsoleKind::ps5, video, bytes_of(1, 0x77));
    CHECK(with_info && with_info->size() == 25);
    const auto parsed = parse_stream_media(ConsoleKind::ps5, *with_info);
    CHECK(parsed && parsed->unit_info && parsed->header_size == 24 && parsed->payload.size() == 1 && parsed->payload[0] == 0x77);
    // Sound: 20 bytes from a PS4, 21 from a PS5.
    const StreamMediaPacket sound = sound_packet(1, 1, 1, 0);
    const auto ps4 = encode_stream_media(ConsoleKind::ps4, sound, bytes_of(1, 0));
    CHECK(ps4 && ps4->size() == 20 && parse_stream_media(ConsoleKind::ps4, *ps4) && !parse_stream_media(ConsoleKind::ps5, *ps4));
    std::vector<std::uint8_t> shorter(ps4->begin(), ps4->end() - 1);
    CHECK(!parse_stream_media(ConsoleKind::ps4, shorter));
    CHECK(!parse_stream_media(ConsoleKind::ps5, {}));
    for (const int kind : {0, 1, 4, 5, 6, 8}) {
        std::vector<std::uint8_t> other(40, 0);
        other[0] = static_cast<std::uint8_t>(kind);
        CHECK(!parse_stream_media(ConsoleKind::ps5, other));
    }
}

PSVP_TEST(frames_whole_in_order) {
    FrameAssembler assembler;
    FrameOutput out;
    const auto first = bytes_of(250, 1);   // three units of 100: the last one padded by 50
    const auto second = bytes_of(40, 9);   // one unit, shorter than the unit size asked for
    const Units a = cut(1, first, 100, 1);
    CHECK(a.packets.size() == 4 && a.payloads[0].size() == 102 && a.payloads[2].size() == 52 && a.payloads[3].size() == 102);
    CHECK(a.payloads[2][0] == 0 && a.payloads[2][1] == 50);
    feed(assembler, a, {0, 1}, kT0, out);
    CHECK(out.frames.empty() && !assembler.deadline());
    feed(assembler, a, {2}, kT0, out);
    CHECK(out.frames.size() == 1 && out.frames[0].index == 1 && out.frames[0].data == first && !out.frames[0].repaired);
    // Its repair unit comes behind the whole frame: not needed, and not an error.
    feed(assembler, a, {3}, kT0, out);
    CHECK(out.frames.size() == 1 && assembler.counters().packets_spare == 1 && assembler.counters().packets_late == 0);
    const Units b = cut(2, second, 100, 0);
    feed(assembler, b, {0}, kT0 + 16ms, out);
    CHECK(out.frames.size() == 2 && out.frames[1].index == 2 && out.frames[1].data == second && out.lost.empty());
    CHECK(assembler.counters().frames == 2 && assembler.counters().frames_lost == 0 && assembler.counters().frames_repaired == 0);
    CHECK(!assembler.deadline());
}

PSVP_TEST(frames_lost_units_are_restored) {
    const auto frame = bytes_of(950, 3);
    const Units units = cut(1, frame, 100, 3);  // ten source units and three repair units
    // Any ten of the thirteen give the frame, in any order.
    const std::initializer_list<unsigned> orders[] = {
        {1, 2, 3, 4, 5, 6, 7, 8, 9, 10},          // the first lost
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 12},          // the padded last one lost
        {12, 11, 10, 9, 8, 7, 6, 5, 4, 3},        // three lost, backwards
        {10, 11, 12, 0, 2, 4, 6, 8, 9, 1},        // repair units first
    };
    for (const auto& order : orders) {
        FrameAssembler assembler;
        FrameOutput out;
        feed(assembler, units, order, kT0, out);
        CHECK(out.frames.size() == 1 && out.frames[0].data == frame && out.frames[0].repaired);
        CHECK(assembler.counters().frames_repaired == 1 && out.lost.empty());
    }
    // Nine are not enough, whichever they are.
    FrameAssembler assembler;
    FrameOutput out;
    feed(assembler, units, {0, 1, 2, 3, 4, 5, 6, 10, 11}, kT0, out);
    CHECK(out.frames.empty() && out.lost.empty());
    // The tenth completes it.
    feed(assembler, units, {12}, kT0, out);
    CHECK(out.frames.size() == 1 && out.frames[0].data == frame && assembler.counters().units_repaired == 3);
}

PSVP_TEST(frames_given_up_after_the_wait) {
    FrameAssembler assembler;
    FrameOutput out;
    const auto f1 = bytes_of(300, 1);
    const auto f2 = bytes_of(300, 2);
    const auto f3 = bytes_of(300, 3);
    const Units a = cut(1, f1, 100, 1);
    const Units b = cut(2, f2, 100, 1);
    const Units c = cut(3, f3, 100, 1);
    feed(assembler, a, {0, 1, 2}, kT0, out);
    CHECK(out.frames.size() == 1);
    // Frame 2 loses two of its four packets: one more than its repair unit covers.
    feed(assembler, b, {0, 3}, kT0 + 16ms, out);
    // Its last unit has come, so its wait has begun: sixteen milliseconds from then.
    CHECK(assembler.deadline() == kT0 + 32ms);
    feed(assembler, c, {0, 1, 2}, kT0 + 20ms, out);
    // Frame 3 is whole, and is held back behind frame 2.
    CHECK(out.frames.size() == 1 && out.lost.empty() && assembler.deadline() == kT0 + 32ms);
    assembler.on_timeout(kT0 + 31ms, out);
    CHECK(out.frames.size() == 1 && out.lost.empty());
    assembler.on_timeout(kT0 + 32ms, out);
    CHECK((out.lost == std::vector<FrameRange>{FrameRange{2, 2}}));
    CHECK(out.frames.size() == 2 && out.frames[1].index == 3 && out.frames[1].data == f3 && out.frames[1].lost_before == 1);
    CHECK(assembler.counters().frames_lost == 1 && !assembler.deadline());
    // What still comes of frame 2 is late.
    feed(assembler, b, {1}, kT0 + 40ms, out);
    CHECK(out.frames.size() == 2 && assembler.counters().packets_late == 1 && assembler.counters().packets_spare == 0);
    // And a unit of frame 3, which left whole, is one it did not need.
    feed(assembler, c, {3}, kT0 + 40ms, out);
    CHECK(assembler.counters().packets_late == 1 && assembler.counters().packets_spare == 1);
}

PSVP_TEST(frames_wait_for_a_packet_that_was_overtaken) {
    FrameAssembler assembler;
    FrameOutput out;
    const auto f1 = bytes_of(300, 1);
    const auto f2 = bytes_of(300, 2);
    const Units a = cut(1, f1, 100, 0);
    const Units b = cut(2, f2, 100, 0);
    feed(assembler, a, {0, 1}, kT0, out);
    // Frame 2 begins before frame 1's last unit is there; frame 1 gets sixteen milliseconds from now.
    feed(assembler, b, {0, 1, 2}, kT0 + 5ms, out);
    CHECK(out.frames.empty() && assembler.deadline() == kT0 + 21ms);
    feed(assembler, a, {2}, kT0 + 9ms, out);
    // Both leave, in order, and nothing is lost.
    CHECK(out.frames.size() == 2 && out.frames[0].data == f1 && out.frames[1].data == f2 && out.lost.empty());
    CHECK(out.frames[1].lost_before == 0 && !assembler.deadline());
}

PSVP_TEST(frames_far_ahead_are_not_waited_behind) {
    FrameAssembler assembler;
    FrameOutput out;
    const auto f1 = bytes_of(200, 1);
    const Units a = cut(1, f1, 100, 0);
    feed(assembler, a, {0}, kT0, out);  // frame 1 stays half
    const auto f2 = bytes_of(100, 2);
    const Units b = cut(2, f2, 100, 0);
    feed(assembler, b, {0}, kT0, out);  // frame 2 whole, held behind frame 1
    CHECK(out.frames.empty());
    // Frame 9: only four frames are kept under way, 6 to 9. Frame 1 is given up, 2 leaves, 3 to 5 never came.
    const auto f9 = bytes_of(100, 9);
    const Units c = cut(9, f9, 100, 0);
    feed(assembler, c, {0}, kT0 + 1ms, out);
    CHECK(out.frames.size() == 1 && out.frames[0].index == 2 && out.frames[0].lost_before == 1);
    CHECK((out.lost == std::vector<FrameRange>{FrameRange{1, 1}, FrameRange{3, 5}}));
    CHECK(assembler.counters().frames_lost == 4);
    // 6, 7 and 8 get their wait, then 9 leaves.
    CHECK(assembler.deadline() == kT0 + 17ms);
    assembler.on_timeout(kT0 + 17ms, out);
    CHECK(out.frames.size() == 2 && out.frames[1].index == 9 && out.frames[1].lost_before == 6);
    CHECK((out.lost == std::vector<FrameRange>{FrameRange{1, 1}, FrameRange{3, 8}}));
    // A jump of thousands of frames costs no more than that.
    const Units far = cut(30000, f9, 100, 0);
    feed(assembler, far, {0}, kT0 + 20ms, out);
    CHECK(out.lost.size() == 3 && (out.lost[2] == FrameRange{10, 29996}));
    CHECK(assembler.counters().frames_lost == 7 + 29987);
}

PSVP_TEST(frames_before_the_first_one_seen_are_missing) {
    FrameAssembler assembler;
    FrameOutput out;
    const auto f3 = bytes_of(100, 3);
    feed(assembler, cut(3, f3, 100, 0), {0}, kT0, out);
    // A stream's frames count from 1: frames 1 and 2 are waited for, then reported.
    CHECK(out.frames.empty() && assembler.deadline() == kT0 + 16ms);
    assembler.on_timeout(kT0 + 16ms, out);
    CHECK(out.frames.size() == 1 && out.frames[0].index == 3 && out.frames[0].lost_before == 2);
    CHECK((out.lost == std::vector<FrameRange>{FrameRange{1, 2}}));
    // A first frame from the other half of the number space is taken as the start.
    FrameAssembler other;
    FrameOutput second;
    feed(other, cut(50000, f3, 100, 0), {0}, kT0, second);
    CHECK(second.frames.size() == 1 && second.frames[0].index == 50000 && second.lost.empty());
}

PSVP_TEST(frames_count_across_the_wrap) {
    FrameAssembler assembler;
    FrameOutput out;
    const auto data = bytes_of(100, 7);
    feed(assembler, cut(65534, data, 100, 0), {0}, kT0, out);
    feed(assembler, cut(65535, data, 100, 0), {0}, kT0, out);
    feed(assembler, cut(1, data, 100, 0), {0}, kT0, out);  // frame 0 is missing
    CHECK(out.frames.size() == 2 && assembler.deadline() == kT0 + 16ms);
    assembler.on_timeout(kT0 + 16ms, out);
    CHECK(out.frames.size() == 3 && out.frames[2].index == 1 && (out.lost == std::vector<FrameRange>{FrameRange{0, 0}}));
    // An old frame's packet from before the wrap is late, not far ahead.
    feed(assembler, cut(65530, data, 100, 0), {0}, kT0 + 17ms, out);
    CHECK(out.frames.size() == 3 && assembler.counters().packets_late == 1 && assembler.counters().frames_lost == 1);
}

PSVP_TEST(frames_refuse_what_contradicts) {
    const auto frame = bytes_of(300, 1);
    const Units units = cut(1, frame, 100, 1);
    {
        // The same unit twice.
        FrameAssembler assembler;
        FrameOutput out;
        feed(assembler, units, {0, 0, 1}, kT0, out);
        CHECK(assembler.counters().packets_duplicate == 1 && out.frames.empty());
        // A unit past its frame's end, one past its own count, one larger than the frame's units,
        // an empty one, a source unit too short for its padding count.
        StreamMediaPacket other = units.packets[2];
        other.unit_index = 4;
        other.units_total = 6;
        assembler.on_packet(other, units.payloads[2], kT0, out);
        other = units.packets[2];
        other.units_total = 2;
        assembler.on_packet(other, units.payloads[2], kT0, out);
        std::vector<std::uint8_t> longer = units.payloads[3];
        longer.push_back(0);
        assembler.on_packet(units.packets[3], longer, kT0, out);
        assembler.on_packet(units.packets[3], {}, kT0, out);
        assembler.on_packet(units.packets[2], std::span<const std::uint8_t>(units.payloads[2]).first(1), kT0, out);
        CHECK(assembler.counters().packets_unusable == 5 && assembler.counters().packets_odd == 0 && out.frames.empty());
        feed(assembler, units, {2}, kT0, out);
        CHECK(out.frames.size() == 1 && out.frames[0].data == frame);
    }
    {
        // Units that are not as their frame's first packet announced are used all the same, and counted:
        // one that counts the frame's units otherwise, one whose padding count does not add up.
        FrameAssembler assembler;
        FrameOutput out;
        feed(assembler, units, {0}, kT0, out);
        StreamMediaPacket other = units.packets[1];
        other.units_total = 5;
        other.units_repair = 2;
        assembler.on_packet(other, units.payloads[1], kT0, out);
        std::vector<std::uint8_t> shorter(units.payloads[2].begin(), units.payloads[2].end() - 1);
        assembler.on_packet(units.packets[2], shorter, kT0, out);
        CHECK(assembler.counters().packets_odd == 2 && assembler.counters().packets_unusable == 0);
        // The frame is what its source units carried behind their first two bytes.
        std::vector<std::uint8_t> expected(frame.begin(), frame.end() - 1);
        CHECK(out.frames.size() == 1 && out.frames[0].data == expected);
    }
    {
        // A frame whose first packet makes no sense is lost at once, and its other packets with it.
        FrameAssembler assembler;
        FrameOutput out;
        StreamMediaPacket bad = units.packets[0];
        bad.units_repair = bad.units_total;  // no source units
        assembler.on_packet(bad, units.payloads[0], kT0, out);
        CHECK((out.lost == std::vector<FrameRange>{FrameRange{1, 1}}) && assembler.counters().packets_unusable == 1);
        feed(assembler, units, {1, 2}, kT0, out);
        CHECK(out.frames.empty() && assembler.counters().packets_late == 2);
        // More units than there are slots for; a unit larger than a datagram.
        bad = units.packets[0];
        bad.frame_index = 2;
        bad.units_total = 513;
        assembler.on_packet(bad, units.payloads[0], kT0, out);
        bad.frame_index = 3;
        bad.units_total = 4;
        std::vector<std::uint8_t> huge = units.payloads[0];
        huge[0] = 0x10;  // says 4096 bytes of padding
        assembler.on_packet(bad, huge, kT0, out);
        CHECK((out.lost == std::vector<FrameRange>{FrameRange{1, 3}}) && assembler.counters().frames_lost == 3);
    }
    {
        // A restored unit whose padding count makes no sense: the frame is lost, never handed on damaged.
        FrameAssembler assembler;
        FrameOutput out;
        const Units one = cut(1, bytes_of(100, 1), 100, 1);
        std::vector<std::uint8_t> garbage(one.payloads[1].size(), 0xff);
        assembler.on_packet(one.packets[1], garbage, kT0, out);
        CHECK(out.frames.empty() && (out.lost == std::vector<FrameRange>{FrameRange{1, 1}}));
        CHECK(assembler.counters().frames == 0 && assembler.counters().frames_lost == 1);
    }
    {
        // More units than the repair code reaches: whole only by its source units.
        FrameAssembler assembler;
        FrameOutput out;
        const auto large = bytes_of(300 * 4, 1);
        Units big = cut(1, large, 4, 0);  // 300 source units
        CHECK(big.packets.size() == 300);
        for (auto& packet : big.packets) {
            packet.units_total = 302;
            packet.units_repair = 2;
        }
        StreamMediaPacket repair = big.packets[0];
        repair.unit_index = 300;
        assembler.on_packet(repair, bytes_of(6, 0), kT0, out);
        for (unsigned i = 1; i < 300; ++i) assembler.on_packet(big.packets[i], big.payloads[i], kT0, out);
        CHECK(out.frames.empty());  // 300 of 302 units, and still not whole
        assembler.on_packet(big.packets[0], big.payloads[0], kT0, out);
        CHECK(out.frames.size() == 1 && out.frames[0].data == large && !out.frames[0].repaired);
    }
}

PSVP_TEST(frames_unit_statistics) {
    FrameAssembler assembler;
    FrameOutput out;
    const auto data = bytes_of(300, 1);
    // Frame 1 whole with a spare repair unit that comes late; frame 2 loses one of four and is repaired;
    // frame 3 loses three of four and is given up.
    feed(assembler, cut(1, data, 100, 1), {0, 1, 2}, kT0, out);
    feed(assembler, cut(2, data, 100, 1), {0, 1, 3}, kT0, out);
    feed(assembler, cut(1, data, 100, 1), {3}, kT0, out);
    feed(assembler, cut(3, data, 100, 1), {3}, kT0, out);
    assembler.on_timeout(kT0 + 16ms, out);
    CHECK(out.frames.size() == 2 && assembler.counters().frames_lost == 1);
    // Counted once a frame has left the short list of recent ones: four more frames push these out.
    CHECK(assembler.take_unit_stats().received == 0);
    for (std::uint16_t index = 4; index <= 7; ++index) feed(assembler, cut(index, data, 100, 0), {0, 1, 2}, kT0 + 20ms, out);
    const UnitStats stats = assembler.take_unit_stats();
    CHECK(stats.received == 4 + 3 + 1 && stats.missing == 0 + 1 + 3);
    CHECK(assembler.take_unit_stats().received == 0 && assembler.take_unit_stats().missing == 0);
}

PSVP_TEST(sound_frames_once_and_in_order) {
    SoundReceiver receiver;
    std::vector<SoundFrame> out;
    // Every packet: one new frame and the two before it again.
    for (const int index : {1, 2, 3, 4}) {
        const StreamMediaPacket packet = sound_packet(static_cast<std::uint16_t>(index), 2, 1, 2);
        receiver.on_packet(packet, sound_payload(packet), out);
    }
    CHECK((indices(out) == std::vector<std::uint16_t>{1, 2, 3, 4}));
    CHECK(receiver.counters().frames == 4 && receiver.counters().frames_recovered == 0 && receiver.counters().frames_lost == 0);
    // Packets 5 and 6 are lost: packet 7 brings their frames again.
    out.clear();
    const StreamMediaPacket seven = sound_packet(7, 2, 1, 2);
    receiver.on_packet(seven, sound_payload(seven), out);
    CHECK((indices(out) == std::vector<std::uint16_t>{5, 6, 7}));
    CHECK(out[0].recovered && out[1].recovered && !out[2].recovered && out[0].lost_before == 0);
    CHECK(receiver.counters().frames_recovered == 2);
    // Three lost in a row: one of them is gone for good.
    out.clear();
    const StreamMediaPacket eleven = sound_packet(11, 2, 1, 2);
    receiver.on_packet(eleven, sound_payload(eleven), out);
    CHECK((indices(out) == std::vector<std::uint16_t>{9, 10, 11}) && out[0].lost_before == 1 && out[1].lost_before == 0);
    CHECK(receiver.counters().frames_lost == 1);
    // A packet that comes late brings nothing new.
    out.clear();
    const StreamMediaPacket eight = sound_packet(8, 2, 1, 2);
    receiver.on_packet(eight, sound_payload(eight), out);
    CHECK(out.empty() && receiver.counters().packets == 7 && receiver.counters().frames == 10);
    // Two new frames in one packet.
    const StreamMediaPacket pair = sound_packet(12, 2, 2, 1);
    receiver.on_packet(pair, sound_payload(pair), out);
    CHECK((indices(out) == std::vector<std::uint16_t>{12, 13}));
}

PSVP_TEST(sound_start_and_wrap) {
    {
        // At the start of a stream a repeated frame may name one from before frame 1: it is not a frame.
        SoundReceiver receiver;
        std::vector<SoundFrame> out;
        const StreamMediaPacket first = sound_packet(1, 2, 1, 2);
        receiver.on_packet(first, sound_payload(first), out);
        const StreamMediaPacket second = sound_packet(2, 2, 1, 2);
        receiver.on_packet(second, sound_payload(second), out);
        CHECK((indices(out) == std::vector<std::uint16_t>{1, 2}));
    }
    {
        // After the wrap the same indices are frames.
        SoundReceiver receiver;
        std::vector<SoundFrame> out;
        for (const int index : {65534, 65535, 0, 1, 2}) {
            const StreamMediaPacket packet = sound_packet(static_cast<std::uint16_t>(index), 2, 1, 2);
            receiver.on_packet(packet, sound_payload(packet), out);
        }
        CHECK((indices(out) == std::vector<std::uint16_t>{65532, 65533, 65534, 65535, 0, 1, 2}));
        // Packet 4 after packet 2: frame 3 comes from its repeats.
        out.clear();
        const StreamMediaPacket four = sound_packet(4, 2, 1, 2);
        receiver.on_packet(four, sound_payload(four), out);
        CHECK((indices(out) == std::vector<std::uint16_t>{3, 4}) && out[0].recovered);
        CHECK(receiver.counters().frames_lost == 0);
    }
}

PSVP_TEST(sound_refuses_what_does_not_add_up) {
    SoundReceiver receiver;
    std::vector<SoundFrame> out;
    StreamMediaPacket packet = sound_packet(1, 2, 1, 2);
    const auto payload = sound_payload(packet);
    packet.codec = 4;  // not Opus
    receiver.on_packet(packet, payload, out);
    packet = sound_packet(1, 2, 1, 2);
    packet.units_total = 4;  // new and repeated frames do not add up to the packet's units
    receiver.on_packet(packet, payload, out);
    packet = sound_packet(1, 2, 1, 2);
    receiver.on_packet(packet, std::span<const std::uint8_t>(payload).first(5), out);  // not three frames of two bytes
    receiver.on_packet(packet, {}, out);
    packet.unit_size = 0;
    receiver.on_packet(packet, payload, out);
    CHECK(out.empty() && receiver.counters().packets_unusable == 5 && receiver.counters().frames == 0);
    // A picture packet is none of its business.
    packet.video = true;
    receiver.on_packet(packet, payload, out);
    CHECK(receiver.counters().packets == 5);
}

// Byte for byte as docs/protocol/input.md lays it out.
PSVP_TEST(input_state_of_a_resting_controller_is_exact) {
    const std::string resting = "a0"
                                "ff7f" "ff7f" "ff7f"  // nothing turns: the middle of -30 to 30, low byte first
                                "ff7f" "9999" "ff7f"  // gravity along y: 1 in -5 to 5 is 0x9999
                                "fef7ef1f"            // no rotation: the fourth part leads, the others are in the middle
                                "0000" "0000" "0000" "0000";  // both sticks in the middle
    CHECK(hex(encode_input_state(ConsoleKind::ps4, InputState{})) == resting);
    CHECK(hex(encode_input_state(ConsoleKind::ps5, InputState{})) == resting + "000001");
    // The packet around it: kind 6, a sequence number, one state; code and key position are written when it is sealed.
    const std::vector<std::uint8_t> state{0xaa, 0xbb};
    CHECK(hex(encode_input_packet(0x0102, state)) == "06" "0102" "00" "00000000" "00000000" "aabb");
}

PSVP_TEST(input_state_carries_sticks_and_sensors) {
    InputState state;
    state.left_x = 0x1234;
    state.left_y = -2;
    state.right_x = -32768;
    state.right_y = 32767;
    state.gyro_x = -30.0F;   // the ends of the ranges
    state.gyro_y = 30.0F;
    state.gyro_z = 100.0F;   // beyond the range: held at its end
    state.accel_x = -5.0F;
    state.accel_y = 5.0F;
    state.accel_z = -100.0F;
    const auto bytes = encode_input_state(ConsoleKind::ps5, state);
    CHECK(hex(bytes).substr(2, 24) == "0000" "ffff" "ffff" "0000" "ffff" "0000");
    CHECK(hex(bytes).substr(34, 16) == "1234" "fffe" "8000" "7fff");  // the sticks, high byte first
    const auto fields = parse_input_state(ConsoleKind::ps5, bytes);
    CHECK(fields.has_value());
    if (!fields) return;
    CHECK(fields->left_x == 0x1234 && fields->left_y == -2 && fields->right_x == -32768 && fields->right_y == 32767);
    CHECK(fields->gyro[0] == 0 && fields->gyro[1] == 0xffff && fields->accel[1] == 0xffff && fields->orientation == 0x1feff7fe);
    // A rotation whose first part leads: its index 0 and sign in the low three bits, the other three in the middle.
    state.orient_x = 1.0F;
    state.orient_w = 0.0F;
    CHECK(parse_input_state(ConsoleKind::ps5, encode_input_state(ConsoleKind::ps5, state)).value().orientation == 0x1feff7f8);
    state.orient_x = -1.0F;
    CHECK(parse_input_state(ConsoleKind::ps5, encode_input_state(ConsoleKind::ps5, state)).value().orientation == 0x1feff7f9);
    // Not a state: another length, another first byte, a PS5's without its last three bytes' values.
    CHECK(!parse_input_state(ConsoleKind::ps4, bytes) && !parse_input_state(ConsoleKind::ps5, std::span<const std::uint8_t>(bytes).first(25)));
    auto changed = bytes;
    changed[0] = 0xa1;
    CHECK(!parse_input_state(ConsoleKind::ps5, changed));
    changed = bytes;
    changed[27] = 0;
    CHECK(!parse_input_state(ConsoleKind::ps5, changed));
}
