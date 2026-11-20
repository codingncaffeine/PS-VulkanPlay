// Picture and sound packets: our reading of their headers, our frame assembly
// and the reports to the console, against the reference's.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include <chiaki/fec.h>
#include <chiaki/controller.h>
#include <chiaki/feedback.h>
#include <chiaki/frameprocessor.h>
#include <chiaki/gkcrypt.h>
#include <chiaki/log.h>
#include <chiaki/takion.h>

#include "oracle.h"
#include "proto/frame_assembler.h"
#include "proto/input.h"
#include "proto/media_packet.h"
#include "proto/media_sender.h"
#include "proto/stream_message.h"
#include "proto/transport.h"
#include "reference_messages.h"

namespace psvp::oracle {
namespace {

using namespace proto;
using namespace proto::transport;

struct Compare {
    Outcome outcome;
    void operator()(bool equal, const char* what) {
        ++outcome.compared;
        if (!equal && ++outcome.mismatches <= 12) std::fprintf(stderr, "    MISMATCH media %s\n", what);
    }
};

// Any bytes as a picture or sound packet, for both families.
void diff_headers(Compare& compare, Random& random, bool plant) {
    int read = 0;
    int refused = 0;
    int ours_only_refused = 0;
    int ours_only_read = 0;
    for (int round = 0; round < 30000; ++round) {
        const bool ps5 = round % 2 == 0;
        const ConsoleKind kind = ps5 ? ConsoleKind::ps5 : ConsoleKind::ps4;
        std::vector<std::uint8_t> datagram(random.below(round % 3 == 0 ? 30 : 80));
        for (std::uint8_t& byte : datagram) byte = static_cast<std::uint8_t>(random.below(256));
        if (!datagram.empty() && round % 16 != 0) {
            // Mostly a kind that is one of the two, with and without the three extra header bytes.
            datagram[0] = static_cast<std::uint8_t>((random.below(2) == 0 ? 2 : 3) | (random.below(4) == 0 ? 0x10 : 0) | (random.below(8) == 0 ? 0xe0 : 0));
        }
        if (datagram.size() > 19 && random.below(3) == 0) datagram[19] = 0x02;  // the haptics mark, more often than chance
        std::vector<std::uint8_t> copy = datagram;
        ChiakiKeyState key_state;
        chiaki_key_state_init(&key_state);
        ChiakiTakionAVPacket theirs;
        const bool theirs_ok = (ps5 ? chiaki_takion_v12_av_packet_parse : chiaki_takion_v9_av_packet_parse)(&theirs, &key_state, copy.data(), copy.size()) ==
                               CHIAKI_ERR_SUCCESS;
        auto ours = parse_stream_media(kind, datagram);
        if (!theirs_ok && ours) {
            // The other chosen difference: a picture packet of fewer than 25 bytes, which a frame's short
            // last unit makes. The reference asks 25 of every picture packet.
            compare(ours->video && datagram.size() < 25, "a packet only the reference refuses is the one we chose to take");
            ++ours_only_read;
            continue;
        }
        if (theirs_ok && !ours) {
            // The one chosen difference: a sound packet that says it has three more header bytes and is
            // too short to hold them. The reference goes on with a payload that begins behind the packet.
            const bool named = !theirs.is_video && theirs.uses_nalu_info_structs &&
                               datagram.size() < stream_media_header_size(kind, false, true) + 1;
            compare(named, "a packet only we refuse is the one we chose to refuse");
            ++ours_only_refused;
            continue;
        }
        compare(theirs_ok == ours.has_value(), "packet accepted or refused");
        if (!theirs_ok || !ours) {
            ++refused;
            continue;
        }
        ++read;
        // Written back before anything is planted: one planted difference is one report.
        const auto written = encode_stream_media(kind, *ours, ours->payload);
        if (plant && ours->video) {
            ours->adaptive_stream ^= 1;
            plant = false;
        }
        bool same = theirs.is_video == ours->video && theirs.uses_nalu_info_structs == ours->unit_info && theirs.packet_index == ours->packet_index &&
                    theirs.frame_index == ours->frame_index && theirs.unit_index == ours->unit_index &&
                    theirs.units_in_frame_total == ours->units_total && theirs.codec == ours->codec &&
                    static_cast<std::uint32_t>(theirs.key_pos) == ours->key_position &&
                    static_cast<std::size_t>(theirs.data - copy.data()) == ours->header_size && theirs.data_size == ours->payload.size();
        if (ours->video) {
            same = same && theirs.units_in_frame_fec == ours->units_repair && theirs.word_at_0x18 == ours->video_word &&
                   theirs.adaptive_stream_index == ours->adaptive_stream;
        } else {
            same = same && chiaki_takion_av_packet_audio_unit_size(&theirs) == ours->unit_size &&
                   chiaki_takion_av_packet_audio_source_units_count(&theirs) == ours->source_frames &&
                   chiaki_takion_av_packet_audio_fec_units_count(&theirs) == ours->repeated_frames && theirs.is_haptics == ours->haptics;
        }
        compare(same, "packet header fields");
        // And back: our writer gives the bytes the reference read, as far as the header's known fields go.
        ChiakiTakionAVPacket again;
        chiaki_key_state_init(&key_state);
        std::vector<std::uint8_t> written_copy = written.value_or(std::vector<std::uint8_t>{});
        const bool again_ok = written && (ps5 ? chiaki_takion_v12_av_packet_parse : chiaki_takion_v9_av_packet_parse)(
                                             &again, &key_state, written_copy.data(), written_copy.size()) == CHIAKI_ERR_SUCCESS;
        compare(again_ok && again.packet_index == theirs.packet_index && again.frame_index == theirs.frame_index &&
                    again.unit_index == theirs.unit_index && again.units_in_frame_total == theirs.units_in_frame_total &&
                    again.units_in_frame_fec == theirs.units_in_frame_fec && again.codec == theirs.codec && again.key_pos == theirs.key_pos &&
                    again.adaptive_stream_index == theirs.adaptive_stream_index && again.is_haptics == theirs.is_haptics &&
                    again.data_size == theirs.data_size && again.word_at_0x18 == theirs.word_at_0x18,
                "packet written and read back by the reference");
    }
    std::fprintf(stderr, "  media headers: %d read alike, %d refused alike, %d refused and %d read by choice\n", read, refused, ours_only_refused,
                 ours_only_read);
}

// Frames cut into units by our sender's rule, some units lost, put together both ways.
void diff_frames(Compare& compare, Random& random, bool plant) {
    int whole = 0;
    int repaired = 0;
    ChiakiLog quiet;
    chiaki_log_init(&quiet, 0, nullptr, nullptr);
    for (int round = 0; round < 3000; ++round) {
        std::vector<std::uint8_t> frame(1 + random.below(round % 10 == 0 ? 60000 : 4000));
        for (std::uint8_t& byte : frame) byte = static_cast<std::uint8_t>(random.below(256));
        const std::size_t unit_data = 1 + random.below(round % 4 == 0 ? 1400 : 300);
        const std::size_t source = (frame.size() + unit_data - 1) / unit_data;
        if (source > 250) continue;
        const unsigned repair = std::min<unsigned>(random.below(7), static_cast<unsigned>(256 - source));
        // The units, by the rule of docs/protocol/video.md, repair units by the reference's own encoder.
        const std::size_t unit_size = 2 + std::min(unit_data, frame.size());
        std::vector<std::uint8_t> units((source + std::max(repair, 1U)) * unit_size, 0);
        std::vector<std::size_t> sizes(source + repair, unit_size);
        for (std::size_t i = 0; i < source; ++i) {
            const std::size_t length = std::min(unit_data, frame.size() - i * unit_data);
            const std::size_t padding = unit_size - 2 - length;
            units[i * unit_size] = static_cast<std::uint8_t>(padding >> 8);
            units[i * unit_size + 1] = static_cast<std::uint8_t>(padding);
            std::memcpy(units.data() + i * unit_size + 2, frame.data() + i * unit_data, length);
            sizes[i] = 2 + length;
        }
        if (repair > 0 && chiaki_fec_encode(units.data(), unit_size, unit_size, static_cast<unsigned>(source), repair) != CHIAKI_ERR_SUCCESS) {
            compare(false, "the reference made the repair units");
            continue;
        }
        // Lose no more units than there are repair units: beyond that the reference's library is not asked.
        std::vector<bool> lost(source + repair, false);
        for (unsigned n = random.below(repair + 1); n > 0; --n) lost[random.below(static_cast<std::uint32_t>(source + repair))] = true;

        ChiakiFrameProcessor processor;
        chiaki_frame_processor_init(&processor, &quiet);
        FrameAssembler assembler;
        FrameOutput out;
        bool allocated = false;
        const auto index = static_cast<std::uint16_t>(1 + random.below(60000));
        for (std::size_t i = 0; i < source + repair; ++i) {
            if (lost[i]) continue;
            ChiakiTakionAVPacket theirs{};
            theirs.is_video = true;
            theirs.frame_index = index;
            theirs.unit_index = static_cast<ChiakiSeqNum16>(i);
            theirs.units_in_frame_total = static_cast<std::uint16_t>(source + repair);
            theirs.units_in_frame_fec = static_cast<std::uint16_t>(repair);
            theirs.data = units.data() + i * unit_size;
            theirs.data_size = sizes[i];
            if (!std::exchange(allocated, true) && chiaki_frame_processor_alloc_frame(&processor, &theirs) != CHIAKI_ERR_SUCCESS) break;
            (void)chiaki_frame_processor_put_unit(&processor, &theirs);

            StreamMediaPacket ours;
            ours.video = true;
            ours.frame_index = index;
            ours.unit_index = static_cast<std::uint16_t>(i);
            ours.units_total = theirs.units_in_frame_total;
            ours.units_repair = theirs.units_in_frame_fec;
            assembler.on_packet(ours, std::span<const std::uint8_t>(units).subspan(i * unit_size, sizes[i]), TimePoint{}, out);
        }
        // The frames before this one never come: their wait runs out, and this one leaves.
        assembler.on_timeout(TimePoint{} + std::chrono::seconds(1), out);
        std::uint8_t* theirs_frame = nullptr;
        std::size_t theirs_size = 0;
        const auto result = chiaki_frame_processor_flush(&processor, &theirs_frame, &theirs_size);
        const bool theirs_ok = result == CHIAKI_FRAME_PROCESSOR_FLUSH_RESULT_SUCCESS || result == CHIAKI_FRAME_PROCESSOR_FLUSH_RESULT_FEC_SUCCESS;
        if (!out.frames.empty() && std::exchange(plant, false)) out.frames[0].data[0] ^= 1;
        const bool same = theirs_ok && out.frames.size() == 1 && out.frames[0].index == index && out.frames[0].data.size() == theirs_size &&
                          std::memcmp(out.frames[0].data.data(), theirs_frame, theirs_size) == 0 && out.frames[0].data.size() == frame.size() &&
                          (result == CHIAKI_FRAME_PROCESSOR_FLUSH_RESULT_FEC_SUCCESS) == out.frames[0].repaired;
        compare(same, "frame put together");
        if (same) ++(out.frames[0].repaired ? repaired : whole);
        chiaki_frame_processor_fini(&processor);
    }
    std::fprintf(stderr, "  media frames: %d whole and %d repaired, alike\n", whole, repaired);
}

// What the client tells the console about the stream.
void diff_reports(Compare& compare, Random& random, bool plant) {
    for (int round = 0; round < 500; ++round) {
        const auto received = static_cast<std::uint16_t>(random.below(0x10000));
        const auto lost = static_cast<std::uint16_t>(random.below(round % 2 == 0 ? 4 : 0x10000));
        ChiakiTakionCongestionPacket packet{0, received, lost};
        std::uint8_t theirs[CHIAKI_TAKION_CONGESTION_PACKET_SIZE];
        chiaki_takion_format_congestion(theirs, &packet, 0);
        std::vector<std::uint8_t> ours = encode_congestion(received, lost);
        if (round == 0 && plant) ours[4] ^= 1;
        compare(ours.size() == sizeof(theirs) && std::memcmp(ours.data(), theirs, sizeof(theirs)) == 0, "congestion report");
        const auto back = parse_congestion(std::span<const std::uint8_t>(theirs));
        compare(back && back->received == received && back->lost == lost, "congestion report read back");

        const std::uint32_t start = random.below(0x10000);
        const std::uint32_t end = random.below(0x10000);
        std::uint8_t message[64];
        const std::size_t size = psvp_ref_encode_corrupt_frames(start, end, message, sizeof(message));
        std::vector<std::uint8_t> mine = encode_message(make_message(CorruptFrames{start, end}));
        if (round == 0 && plant) mine.back() ^= 1;
        compare(size != 0 && mine.size() == size && std::memcmp(mine.data(), message, size) == 0, "frames-not-arrived message");
        const auto parsed = parse_message(std::span<const std::uint8_t>(message, size));
        compare(parsed && parsed->as<CorruptFrames>() != nullptr && *parsed->as<CorruptFrames>() == CorruptFrames{start, end},
                "frames-not-arrived message read back");
    }
    // The console's view of the connection, as the reference's coder writes it and as ours reads it.
    for (int round = 0; round < 500; ++round) {
        ConnectionQuality quality;
        quality.target_bitrate = random.below(round % 2 == 0 ? 200 : 0x7fffffff);
        quality.upstream_bitrate = random.below(0x10000);
        quality.upstream_loss = static_cast<float>(random.below(1001)) / 1000.0F;
        quality.disable_upstream_audio = random.below(2) == 1;
        quality.rtt = static_cast<double>(random.below(1000000)) / 997.0;
        quality.loss = random.next() >> (round % 64);
        std::uint8_t theirs[96];
        const std::size_t theirs_size = psvp_ref_encode_connection_quality(*quality.target_bitrate, *quality.upstream_bitrate, *quality.upstream_loss,
                                                                           *quality.disable_upstream_audio, *quality.rtt, *quality.loss, theirs,
                                                                           sizeof(theirs));
        std::vector<std::uint8_t> ours = encode_message(make_message(quality));
        if (round == 0 && plant) ours.back() ^= 1;
        compare(theirs_size != 0 && ours.size() == theirs_size && std::memcmp(ours.data(), theirs, theirs_size) == 0, "connection quality message");
        const auto parsed = parse_message(std::span<const std::uint8_t>(theirs, theirs_size));
        compare(parsed && parsed->as<ConnectionQuality>() != nullptr && *parsed->as<ConnectionQuality>() == quality,
                "connection quality message read back");
    }
    std::uint8_t message[16];
    const std::size_t size = psvp_ref_encode_plain(message_type::kKeyframeRequest, message, sizeof(message));
    const auto mine = encode_message(make_plain_message(message_type::kKeyframeRequest));
    compare(size != 0 && mine.size() == size && std::memcmp(mine.data(), message, size) == 0, "keyframe request");
}

// The controller's state as it travels, for both families.
void diff_input(Compare& compare, Random& random, bool plant) {
    const auto real = [&](float range) { return (static_cast<float>(random.below(2000001)) / 1000000.0F - 1.0F) * range; };
    for (int round = 0; round < 6000; ++round) {
        InputState ours;
        ours.gyro_x = real(30.0F);
        ours.gyro_y = real(30.0F);
        ours.gyro_z = real(30.0F);
        ours.accel_x = real(5.0F);
        ours.accel_y = real(5.0F);
        ours.accel_z = real(5.0F);
        float q[4] = {real(1.0F), real(1.0F), real(1.0F), real(1.0F)};
        if (round % 5 == 0) q[random.below(4)] = round % 10 == 0 ? 1.0F : -1.0F;  // one part that leads clearly
        if (round % 7 == 0) q[random.below(4)] = 0.0F;
        if (round % 3 != 0) {
            // Mostly a rotation proper: of length 1.
            const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (length > 0.0F) {
                for (float& part : q) part /= length;
            }
        }
        ours.orient_x = q[0];
        ours.orient_y = q[1];
        ours.orient_z = q[2];
        ours.orient_w = q[3];
        ours.left_x = static_cast<std::int16_t>(random.below(0x10000));
        ours.left_y = static_cast<std::int16_t>(random.below(0x10000));
        ours.right_x = static_cast<std::int16_t>(random.below(0x10000));
        ours.right_y = static_cast<std::int16_t>(random.below(0x10000));
        if (round == 0) ours = InputState{};  // the resting controller
        ChiakiFeedbackState theirs{ours.gyro_x, ours.gyro_y, ours.gyro_z, ours.accel_x, ours.accel_y, ours.accel_z, ours.orient_x,
                                   ours.orient_y, ours.orient_z, ours.orient_w, ours.left_x, ours.left_y, ours.right_x, ours.right_y};
        std::uint8_t v9[CHIAKI_FEEDBACK_STATE_BUF_SIZE_V9];
        std::uint8_t v12[CHIAKI_FEEDBACK_STATE_BUF_SIZE_V12];
        chiaki_feedback_state_format_v9(v9, &theirs);
        chiaki_feedback_state_format_v12(v12, &theirs);
        std::vector<std::uint8_t> ps4 = encode_input_state(ConsoleKind::ps4, ours);
        const std::vector<std::uint8_t> ps5 = encode_input_state(ConsoleKind::ps5, ours);
        if (round == 1 && plant) ps4[13] ^= 0x08;
        compare(ps4.size() == sizeof(v9) && std::memcmp(ps4.data(), v9, sizeof(v9)) == 0, "input state");
        compare(ps5.size() == sizeof(v12) && std::memcmp(ps5.data(), v12, sizeof(v12)) == 0, "input state of a PS5");
        const auto fields = parse_input_state(ConsoleKind::ps5, std::span<const std::uint8_t>(v12));
        compare(fields && fields->left_x == ours.left_x && fields->left_y == ours.left_y && fields->right_x == ours.right_x &&
                    fields->right_y == ours.right_y,
                "input state read back");
    }
}

// What the reference's history buffer holds after each event, as its sender would put it in a packet.
struct ReferencePackets {
    ChiakiFeedbackHistoryBuffer* buffer = nullptr;
    std::vector<std::vector<std::uint8_t>> payloads;
    std::vector<std::size_t> counts;
};

void reference_event_pushed(void* user) {
    auto* packets = static_cast<ReferencePackets*>(user);
    std::uint8_t bytes[0x300];
    std::size_t size = sizeof(bytes);
    if (chiaki_feedback_history_buffer_format(packets->buffer, bytes, &size) != CHIAKI_ERR_SUCCESS) size = 0;
    packets->payloads.emplace_back(bytes, bytes + size);
    packets->counts.push_back(packets->buffer->len);
    // The reference's sender keeps eight events for the next packet (FEEDBACK_HISTORY_RESEND_EVENT_COUNT).
    if (packets->buffer->len > 8) packets->buffer->len = 8;
}

// Buttons, triggers and touches: single events, then a long row of controller changes.
void diff_history(Compare& compare, Random& random, bool plant) {
    for (unsigned i = 0; i < kButtons; ++i) {
        for (const bool pressed : {true, false}) {
            ChiakiFeedbackHistoryEvent theirs;
            const bool ok = chiaki_feedback_history_event_set_button(&theirs, std::uint64_t{1} << i, pressed ? 0xff : 0) == CHIAKI_ERR_SUCCESS;
            const HistoryEvent ours = button_event(static_cast<Button>(i), pressed);
            compare(ok && theirs.len == ours.size && std::memcmp(theirs.buf, ours.bytes.data(), ours.size) == 0, "input history button event");
        }
    }
    for (unsigned value = 0; value < 256; ++value) {
        for (const bool right : {false, true}) {
            ChiakiFeedbackHistoryEvent theirs;
            const bool ok = chiaki_feedback_history_event_set_button(&theirs, right ? CHIAKI_CONTROLLER_ANALOG_BUTTON_R2 : CHIAKI_CONTROLLER_ANALOG_BUTTON_L2,
                                                                     static_cast<std::uint8_t>(value)) == CHIAKI_ERR_SUCCESS;
            const HistoryEvent ours = trigger_event(right, static_cast<std::uint8_t>(value));
            compare(ok && theirs.len == ours.size && std::memcmp(theirs.buf, ours.bytes.data(), ours.size) == 0, "input history trigger event");
        }
    }
    for (int round = 0; round < 2000; ++round) {
        const Touch touch{random.below(2) == 1, static_cast<std::uint8_t>(random.below(128)), static_cast<std::uint16_t>(random.below(4096)),
                          static_cast<std::uint16_t>(random.below(4096))};
        ChiakiFeedbackHistoryEvent theirs;
        chiaki_feedback_history_event_set_touchpad(&theirs, touch.down, touch.id, touch.x, touch.y);
        const HistoryEvent ours = touch_event(touch);
        compare(theirs.len == ours.size && std::memcmp(theirs.buf, ours.bytes.data(), ours.size) == 0, "input history touch event");
        const auto back = parse_history_events(std::span<const std::uint8_t>(theirs.buf, theirs.len));
        compare(back && back->size() == 1 && (*back)[0].what == ReadEvent::What::touch && (*back)[0].touch == touch, "input history touch event read back");
    }

    // A controller that keeps changing: after every change both must want the same packets.
    ChiakiFeedbackHistoryBuffer buffer;
    if (chiaki_feedback_history_buffer_init(&buffer, 16) != CHIAKI_ERR_SUCCESS) {
        compare(false, "the reference's history buffer");
        return;
    }
    ChiakiLog quiet;
    chiaki_log_init(&quiet, 0, nullptr, nullptr);
    ChiakiControllerState theirs_before;
    chiaki_controller_state_set_idle(&theirs_before);
    Buttons before;
    InputHistory history;
    std::uint16_t next_number = 0;
    for (int round = 0; round < 4000; ++round) {
        Buttons now = before;
        // A few buttons, a trigger, a touch: sometimes several at once, sometimes nothing.
        for (unsigned n = random.below(4); n > 0; --n) now.pressed = static_cast<std::uint16_t>(now.pressed ^ (1U << random.below(16)));
        if (random.below(4) == 0) now.l2 = static_cast<std::uint8_t>(random.below(256));
        if (random.below(4) == 0) now.r2 = static_cast<std::uint8_t>(random.below(256));
        for (Touch& touch : now.touches) {
            switch (random.below(8)) {
            case 0: touch = Touch{}; break;                                                                    // lifted
            case 1: touch = Touch{true, static_cast<std::uint8_t>(random.below(128)), static_cast<std::uint16_t>(random.below(1920)),
                                  static_cast<std::uint16_t>(random.below(1080))}; break;                    // down, or another finger
            case 2: if (touch.down) touch.x = static_cast<std::uint16_t>(random.below(1920)); break;           // moved
            default: break;
            }
        }
        ChiakiControllerState theirs_now = theirs_before;
        theirs_now.buttons = now.pressed;
        theirs_now.l2_state = now.l2;
        theirs_now.r2_state = now.r2;
        for (std::size_t i = 0; i < now.touches.size(); ++i) {
            theirs_now.touches[i].id = now.touches[i].down ? static_cast<std::int8_t>(now.touches[i].id) : std::int8_t{-1};
            theirs_now.touches[i].x = now.touches[i].down ? now.touches[i].x : std::uint16_t{0};
            theirs_now.touches[i].y = now.touches[i].down ? now.touches[i].y : std::uint16_t{0};
        }
        ReferencePackets theirs{&buffer, {}, {}};
        chiaki_feedback_history_buffer_record_state(&buffer, &quiet, &theirs_before, &theirs_now, reference_event_pushed, &theirs);
        std::vector<InputHistory::Packet> ours = history.on_change(before, now, TimePoint{});
        if (round == 0 && plant && !ours.empty()) ours[0].payload[1] ^= 1;
        bool same = ours.size() == theirs.payloads.size();
        for (std::size_t i = 0; same && i < ours.size(); ++i) {
            same = ours[i].payload == theirs.payloads[i] && ours[i].events == theirs.counts[i] && ours[i].sequence == next_number++;
        }
        if (!same) next_number = static_cast<std::uint16_t>(next_number + theirs.payloads.size());
        compare(same, "input history after a change");
        // A lifted finger keeps no place in our state; the reference's keeps the place it had.
        before = now;
        theirs_before = theirs_now;
    }
    chiaki_feedback_history_buffer_fini(&buffer);
}

}  // namespace

Outcome diff_media(bool plant) {
    Compare compare;
    Random random(0x6d656469'61212121ULL);
    diff_headers(compare, random, plant);
    diff_frames(compare, random, plant);
    diff_reports(compare, random, plant);
    diff_input(compare, random, plant);
    diff_history(compare, random, plant);
    std::fprintf(stderr, "  media: %d comparisons, %d mismatches\n", compare.outcome.compared, compare.outcome.mismatches);
    return compare.outcome;
}

}  // namespace psvp::oracle
