#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "check.h"
#include "media/bit_writer.h"
#include "media/h264_refs.h"
#include "media/test_clip.h"
#include "media/video_decoder.h"

using namespace psvp::media;

namespace {

using Bytes = std::vector<std::uint8_t>;
using Way = H264Tracker::Way;
using detail::append_unit;
using detail::BitWriter;

// Parameter sets of a High profile stream: a scaling list to read past, order counts of their
// own (type 0, `order_bits`), the bottom field's order in the picture set, `kept` frames.
Bytes high_profile_header(bool frames_only = true, unsigned slice_groups = 0) {
    BitWriter sequence;
    sequence.bits(100, 8);  // profile_idc: High
    sequence.bits(0, 8);
    sequence.bits(41, 8);
    sequence.ue(0);         // seq_parameter_set_id
    sequence.ue(1);         // chroma_format_idc
    sequence.ue(0);
    sequence.ue(0);
    sequence.bits(0, 1);    // qpprime_y_zero_transform_bypass_flag
    sequence.bits(1, 1);    // seq_scaling_matrix_present_flag
    for (unsigned i = 0; i < 8; ++i) {
        sequence.bits(i == 1 ? 1 : 0, 1);
        if (i == 1) {
            // delta_scale: 8 -> 13, 13, 13, then 0: the rest of the list repeats the last, nothing more is written.
            for (const int delta : {5, 0, 0, -13}) sequence.se(delta);
        }
    }
    sequence.ue(3);         // log2_max_frame_num_minus4: 7 bits
    sequence.ue(0);         // pic_order_cnt_type
    sequence.ue(4);         // log2_max_pic_order_cnt_lsb_minus4: 8 bits
    sequence.ue(9);         // max_num_ref_frames
    sequence.bits(0, 1);
    sequence.ue(79);
    sequence.ue(44);
    sequence.bits(frames_only ? 1 : 0, 1);
    sequence.bits(1, 1);
    sequence.bits(0, 1);
    sequence.bits(0, 1);
    sequence.finish();
    BitWriter picture;
    picture.ue(0);
    picture.ue(0);
    picture.bits(1, 1);     // entropy_coding_mode_flag
    picture.bits(1, 1);     // bottom_field_pic_order_in_frame_present_flag
    picture.ue(slice_groups);
    picture.ue(0);          // one frame used by default
    picture.ue(0);
    picture.bits(0, 3);
    picture.se(0);
    picture.se(0);
    picture.se(0);
    picture.bits(1, 1);
    picture.bits(0, 1);
    picture.bits(0, 1);     // redundant_pic_cnt_present_flag
    picture.finish();
    Bytes out;
    append_unit(out, {0x67}, sequence.bytes());
    append_unit(out, {0x68}, picture.bytes());
    return out;
}

// A P slice of that stream: `number`, and the list changes given as (idc, value) pairs.
Bytes high_profile_slice(unsigned number, std::vector<std::pair<unsigned, unsigned>> changes = {}, unsigned type = 5, unsigned override_used = 0) {
    BitWriter slice;
    slice.ue(0);
    slice.ue(type);
    slice.ue(0);
    slice.bits(number, 7);
    slice.bits((number * 2) & 0xff, 8);  // pic_order_cnt_lsb
    slice.se(0);                         // delta_pic_order_cnt_bottom
    if (type % 5 == 1) slice.bits(1, 1); // direct_spatial_mv_pred_flag
    slice.bits(override_used != 0 ? 1 : 0, 1);
    if (override_used != 0) slice.ue(override_used - 1);
    slice.bits(changes.empty() ? 0 : 1, 1);
    for (const auto& [idc, value] : changes) {
        slice.ue(idc);
        slice.ue(value);
    }
    if (!changes.empty()) slice.ue(3);
    slice.bits(0x5a5a, 16);  // what follows does not matter here
    slice.finish();
    Bytes out;
    append_unit(out, {0x41}, slice.bytes());
    return out;
}

}  // namespace

PSVP_TEST(h264_layout_is_read_from_the_parameter_sets) {
    const auto clip = read_h264_layout(ClipWriter(416, 240).parameter_sets());
    CHECK(clip.has_value());
    if (clip) {
        CHECK(clip->frame_number_bits == 7 && clip->order_type == 2 && clip->frames_kept == 9 && clip->default_used == 1);
        CHECK(clip->frames_only && !clip->bottom_order_present && !clip->redundant_counts);
    }
    const auto high = read_h264_layout(high_profile_header());
    CHECK(high.has_value());
    if (!high) std::fprintf(stderr, "    %s\n", high.error().c_str());
    if (high) CHECK(high->frame_number_bits == 7 && high->order_type == 0 && high->order_bits == 8 && high->bottom_order_present && high->frames_kept == 9);
    // Refused: fields, slice groups, no parameter sets, two of a kind.
    CHECK(!read_h264_layout(high_profile_header(false)));
    CHECK(!read_h264_layout(high_profile_header(true, 1)));
    CHECK(!read_h264_layout(Bytes{0, 0, 0, 1, 0x65, 0x88}));
    Bytes twice = high_profile_header();
    const Bytes again = high_profile_header();
    twice.insert(twice.end(), again.begin(), again.end());
    CHECK(!read_h264_layout(twice));
}

PSVP_TEST(h264_frames_tell_which_frames_they_use) {
    const auto layout = read_h264_layout(high_profile_header());
    CHECK(layout.has_value());
    if (!layout) return;
    const auto plain = read_h264_frame(*layout, high_profile_slice(20));
    CHECK(plain && !plain->restart && !plain->intra && plain->reference && plain->number == 20 && plain->used == std::vector<unsigned>{1});
    // A frame number below its own, four below (abs_diff_pic_num_minus1 3): frame 16 is put first.
    const auto repair = read_h264_frame(*layout, high_profile_slice(20, {{0, 3}}));
    CHECK(repair && repair->used == std::vector<unsigned>{4});
    // Three used: the one put first, then the list as it was without it.
    const auto three = read_h264_frame(*layout, high_profile_slice(20, {{0, 1}}, 5, 3));
    CHECK(three && three->used == (std::vector<unsigned>{2, 1, 3}));
    // Changes go on from the last: down 2, then up 1 = 1 back.
    const auto twice = read_h264_frame(*layout, high_profile_slice(20, {{0, 1}, {1, 0}}, 5, 2));
    CHECK(twice && twice->used == (std::vector<unsigned>{2, 1}));
    // Across the wrap of seven bits: frame 2 asking for 6 below is frame 124.
    const auto wrapped = read_h264_frame(*layout, high_profile_slice(2, {{0, 5}}));
    CHECK(wrapped && wrapped->used == std::vector<unsigned>{6});
    // An I slice uses nothing.
    const auto intra = read_h264_frame(*layout, high_profile_slice(20, {}, 7));
    CHECK(intra && intra->intra && intra->used.empty());
    // Refused: B frames, long-term frames in the changes, a change to the frame itself.
    CHECK(!read_h264_frame(*layout, high_profile_slice(20, {}, 6)));
    CHECK(!read_h264_frame(*layout, high_profile_slice(20, {{2, 0}})));
    CHECK(!read_h264_frame(*layout, high_profile_slice(20, {{0, 127}})));
    CHECK(!read_h264_frame(*layout, {}));
    // Cut short anywhere: refused or read the same, and never past the end.
    const Bytes frame = high_profile_slice(20, {{0, 3}});
    for (std::size_t size = 0; size < frame.size(); ++size) {
        const auto cut = read_h264_frame(*layout, std::span(frame).first(size));
        CHECK(!cut || cut->used == std::vector<unsigned>{4});
    }

    // The clip's own frames: a keyframe, then each uses the one before; a repair the last before a loss.
    ClipWriter writer(416, 240);
    const auto clip = read_h264_layout(writer.parameter_sets());
    CHECK(clip.has_value());
    if (!clip) return;
    const auto key = read_h264_frame(*clip, writer.frame(0, false));
    CHECK(key && key->restart && key->intra && key->number == 0);
    for (std::uint32_t counter = 1; counter < 5; ++counter) {
        const auto read = read_h264_frame(*clip, writer.frame(counter, false));
        CHECK(read && read->number == counter && read->used == std::vector<unsigned>{1});
    }
    writer.decoder_lost(3, 3);
    const auto after = read_h264_frame(*clip, writer.frame(5, false));
    CHECK(after && after->number == 5 && after->used == std::vector<unsigned>{3});
}

// The tracker, frame by frame: what is built on a lost frame is not the console's picture until
// a frame that is built on exact ones only.
PSVP_TEST(h264_tracker_follows_a_loss_and_the_repair) {
    const Bytes header = high_profile_header();
    H264Tracker tracker(header);
    CHECK(tracker.unread_reason().empty());
    const auto take = [&](std::uint16_t index, const Bytes& frame, bool decodes = true) {
        auto verdict = tracker.look(index, frame);
        if (verdict.way == Way::as_sent || verdict.way == Way::changed) tracker.decoded(index, decodes);
        return verdict;
    };
    // Before a keyframe nothing is held.
    CHECK(take(1, high_profile_slice(9)).way == Way::no_reference);
    Bytes idr;
    {
        BitWriter slice;
        slice.ue(0);
        slice.ue(7);
        slice.ue(0);
        slice.bits(0, 7);
        slice.ue(0);  // idr_pic_id
        slice.bits(0, 8);
        slice.se(0);
        slice.bits(0x5a5a, 16);
        slice.finish();
        append_unit(idr, {0x65}, slice.bytes());
    }
    const auto key = take(2, idr);
    CHECK(key.way == Way::as_sent && key.exact);
    for (unsigned number = 1; number <= 10; ++number) {
        const auto verdict = take(static_cast<std::uint16_t>(number + 2), high_profile_slice(number));
        CHECK(verdict.way == Way::as_sent && verdict.exact);
    }
    // Frame number 11 is lost. 12 uses it: the decoder puts 10 in its place.
    const auto behind = take(14, high_profile_slice(12));
    CHECK(behind.way == Way::changed && !behind.exact && behind.changed.empty());
    const auto built = take(15, high_profile_slice(13));
    CHECK(built.way == Way::as_sent && !built.exact);
    // The repair uses frame 10, four below 14.
    const auto repair = take(16, high_profile_slice(14, {{0, 3}}));
    CHECK(repair.way == Way::as_sent && repair.exact);
    const auto next = take(17, high_profile_slice(15));
    CHECK(next.way == Way::as_sent && next.exact);
    // A repair that uses the lost frame itself is no repair.
    const auto wrong = take(18, high_profile_slice(16, {{0, 4}}));
    CHECK(wrong.way == Way::changed && !wrong.exact);
    // A frame the decoder gave no picture for is as good as lost.
    (void)take(19, high_profile_slice(17), false);
    CHECK(take(20, high_profile_slice(18)).way == Way::changed);
    // Further back than the decoder keeps: frame 1 is not held any more.
    CHECK(take(21, high_profile_slice(19, {{0, 17}})).way == Way::no_reference);
    // A frame of a shape not read: nothing is read until a keyframe.
    CHECK(take(22, high_profile_slice(20, {}, 6)).way == Way::unread && !tracker.unread_reason().empty());
    CHECK(take(23, high_profile_slice(21)).way == Way::unread);
    CHECK(take(24, idr).way == Way::as_sent && tracker.unread_reason().empty());
}

// The whole way, with FFmpeg's decoder: a lost H.264 frame, the pictures behind it counted as
// built on it, the console's repair exact again; the clip's samples say the same.
PSVP_TEST(h264_pictures_behind_a_lost_frame_are_counted) {
    ClipWriter writer(416, 240);
    auto decoder = VideoDecoder::open(VideoCodec::h264, writer.parameter_sets());
    CHECK(decoder.has_value());
    if (!decoder) return;
    CHECK(decoder->unfollowed_reason().empty());
    const auto take = [&](std::uint32_t counter, const Bytes& frame) {
        auto decoded = decoder->decode(static_cast<std::uint16_t>(counter), frame);
        const bool exact_samples = decoded.picture && clip_differences(decoded.picture->view(), counter) == 0;
        return std::pair{decoded.picture.has_value() && !decoded.picture->approximate && !decoded.picture->damaged, exact_samples};
    };
    for (std::uint32_t counter = 100; counter < 110; ++counter) CHECK(take(counter, writer.frame(counter, counter == 100)) == std::pair(true, true));
    (void)writer.frame(110, false);  // never decoded
    for (std::uint32_t counter = 111; counter < 114; ++counter) CHECK(take(counter, writer.frame(counter, false)) == std::pair(false, false));
    writer.decoder_lost(110, 110);
    CHECK(take(114, writer.frame(114, false)) == std::pair(true, true));
    CHECK(take(115, writer.frame(115, false)) == std::pair(true, true));
    CHECK(decoder->counters().changed == 1 && decoder->counters().approximate == 3 && decoder->counters().undecodable == 0);
    CHECK(decoder->unfollowed_reason().empty());
}
