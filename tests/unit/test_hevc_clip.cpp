#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "check.h"
#include "media/frame_refs.h"
#include "media/hevc_clip.h"
#include "media/video_decoder.h"

using namespace psvp::media;

namespace {

using Way = ReferenceTracker::Way;

// How many frames back the frame a clip frame is predicted from is; 0 for a keyframe or an unread frame.
unsigned used_back(const HevcLayout& layout, std::span<const std::uint8_t> frame) {
    const auto read = read_hevc_frame(layout, frame);
    if (!read) return 0;
    for (const NamedFrame& named : read->named) {
        if (named.used) return named.back;
    }
    return 0;
}

}  // namespace

PSVP_TEST(hevc_clip_is_told_from_its_header) {
    CHECK(codec_of_header(HevcClipWriter(416, 240).parameter_sets()) == VideoCodec::hevc);
}

// What goes into the clip comes out of FFmpeg's HEVC decoder, sample for sample, one picture per frame.
PSVP_TEST(hevc_clip_decodes_to_the_same_samples) {
    // 258x66 is no whole number of blocks: the conformance window cuts the picture to its size.
    for (const auto& [width, height] : {std::pair{416U, 240U}, std::pair{320U, 180U}, std::pair{258U, 66U}}) {
        HevcClipWriter writer(width, height);
        auto decoder = VideoDecoder::open(VideoCodec::hevc, writer.parameter_sets());
        CHECK(decoder.has_value());
        if (!decoder) return;
        std::size_t right = 0, largest_later_frame = 0;
        for (std::uint32_t counter = 250; counter < 290; ++counter) {
            const auto frame = writer.frame(counter, counter == 270);
            if (counter != 250 && counter != 270) largest_later_frame = std::max(largest_later_frame, frame.size());
            const auto picture = decoder->decode(frame);
            CHECK(picture.has_value());
            if (!picture) continue;
            const bool as_sent = picture->width == width && picture->height == height && picture->bits == 8 && !picture->full_range &&
                                 picture->matrix == ColorMatrix::bt709 && picture->transfer == ColorTransfer::sdr && !picture->damaged &&
                                 picture->keyframe == (counter == 250 || counter == 270);
            if (as_sent && read_clip_counter(picture->view()) == counter && clip_differences(picture->view(), counter) == 0) ++right;
        }
        CHECK(right == 40);
        CHECK(decoder->counters().frames == 40 && decoder->counters().pictures == 40);
        CHECK(decoder->counters().refused == 0 && decoder->counters().damaged == 0);
        // A later frame carries only the blocks that changed, 384 bytes each (at most 20, from 255 to 256).
        CHECK(largest_later_frame > 20 * 384 && largest_later_frame < 20 * 390);
    }
}

// The checks can fail: a frame that carries a block wrongly shows up in the samples.
PSVP_TEST(hevc_clip_readers_see_a_wrong_picture) {
    HevcClipWriter writer(416, 240);
    auto decoder = VideoDecoder::open(VideoCodec::hevc, writer.parameter_sets());
    CHECK(decoder.has_value());
    if (!decoder) return;
    const auto picture = decoder->decode(writer.frame(1000, true));
    CHECK(picture && clip_differences(picture->view(), 1000) == 0);
    if (picture) CHECK(clip_differences(picture->view(), 1001) == 4 * 256);
    // One sample of the first block's luma (it begins right after the slice header) changed.
    auto frame = writer.frame(1001, true);
    for (std::size_t i = frame.size() / 2; i < frame.size(); ++i) {
        if (frame[i] == 64) {
            frame[i] = 65;
            break;
        }
    }
    const auto changed = decoder->decode(frame);
    CHECK(changed && clip_differences(changed->view(), 1001) == 1);
}

// The frames are shaped as a PS5's: what this client's frame reader reads from them.
PSVP_TEST(hevc_clip_is_shaped_like_a_console_stream) {
    HevcClipWriter writer(416, 240);
    const auto layout = read_hevc_layout(writer.parameter_sets());
    CHECK(layout.has_value());
    if (!layout) return;
    CHECK(layout->order_bits == 4 && !layout->lists_changeable && !layout->dependent_slices && !layout->output_flag);
    ReferenceTracker tracker(writer.parameter_sets());
    CHECK(tracker.unread_reason().empty());

    const auto key = writer.frame(0, false);  // the first frame is a keyframe whatever is asked
    const auto read_key = read_hevc_frame(*layout, key);
    CHECK(read_key && read_key->restart && read_key->intra && read_key->named.empty());
    for (std::uint32_t counter = 1; counter <= 20; ++counter) {
        const auto frame = writer.frame(counter, false);
        const auto read = read_hevc_frame(*layout, frame);
        CHECK(read.has_value());
        if (!read) continue;
        // Up to nine frames named, nearest first, the one before it used; four bits of order count.
        const std::size_t named = std::min<std::size_t>(counter, HevcClipWriter::kNamed);
        CHECK(!read->restart && !read->intra && read->order == counter % 16 && read->named.size() == named);
        for (std::size_t i = 0; i < read->named.size(); ++i) CHECK(read->named[i].back == i + 1 && read->named[i].used == (i == 0));
    }
}

// The whole way, as a session goes: frame 110 is lost. This client changes the frames
// behind it to use a frame the decoder holds, and shows them; they are not the clip's
// pictures. When the console hears of the loss, its next frame is predicted from the last
// frame before it, and the picture is the clip's again, without a keyframe.
PSVP_TEST(hevc_clip_recovers_from_a_lost_frame_without_a_keyframe) {
    HevcClipWriter writer(416, 240);
    const auto header = writer.parameter_sets();
    const auto layout = read_hevc_layout(header);
    auto decoder = VideoDecoder::open(VideoCodec::hevc, header);
    CHECK(decoder.has_value() && layout.has_value());
    if (!decoder || !layout) return;
    ReferenceTracker tracker(header);
    struct Seen {
        Way way = Way::unread;
        bool exact = false;
        std::size_t differences = 0;
        bool keyframe = false;
    };
    const auto take = [&](std::uint32_t counter, const std::vector<std::uint8_t>& frame) {
        const auto index = static_cast<std::uint16_t>(counter);
        auto verdict = tracker.look(index, frame);
        Seen seen{verdict.way, verdict.exact, ~std::size_t{0}, false};
        if (verdict.way != Way::as_sent && verdict.way != Way::changed) return seen;
        const auto picture = decoder->decode(verdict.way == Way::changed ? std::span<const std::uint8_t>(verdict.changed) : std::span<const std::uint8_t>(frame));
        tracker.decoded(index, picture.has_value());
        if (picture) {
            seen.differences = clip_differences(picture->view(), counter);
            seen.keyframe = picture->keyframe;
        }
        return seen;
    };

    for (std::uint32_t counter = 100; counter < 110; ++counter) {
        const auto seen = take(counter, writer.frame(counter, counter == 100));
        CHECK(seen.way == Way::as_sent && seen.exact && seen.differences == 0);
    }
    (void)writer.frame(110, false);  // written, never decoded
    // Frame 111 is predicted from 110: changed to use 109. Frames 112 and 113 are built on it.
    // The moving block's place of frame 110 was never drawn: every one of them shows a wrong block.
    for (std::uint32_t counter = 111; counter < 114; ++counter) {
        const auto seen = take(counter, writer.frame(counter, false));
        CHECK(seen.way == (counter == 111 ? Way::changed : Way::as_sent) && !seen.exact);
        CHECK(seen.differences >= 256 && seen.differences != ~std::size_t{0});
    }
    writer.decoder_lost(110, 110);
    const auto repair_frame = writer.frame(114, false);
    CHECK(used_back(*layout, repair_frame) == 5);  // frame 109
    const auto repair = take(114, repair_frame);
    CHECK(repair.way == Way::as_sent && repair.exact && repair.differences == 0 && !repair.keyframe);
    const auto next = take(115, writer.frame(115, false));
    CHECK(next.exact && next.differences == 0);
    const auto after = writer.frame(116, false);
    CHECK(used_back(*layout, after) == 1);
    const auto later = take(116, after);
    CHECK(later.exact && later.differences == 0);
    CHECK(decoder->counters().refused == 0 && decoder->counters().damaged == 0);
}

// A loss that covers every frame named: a keyframe. A report of frames older than any kept: nothing changes.
PSVP_TEST(hevc_clip_falls_back_to_a_keyframe) {
    HevcClipWriter writer(416, 240);
    const auto layout = read_hevc_layout(writer.parameter_sets());
    CHECK(layout.has_value());
    if (!layout) return;
    for (std::uint32_t counter = 0; counter < 30; ++counter) (void)writer.frame(counter, false);
    writer.decoder_lost(3, 4);
    CHECK(used_back(*layout, writer.frame(30, false)) == 1);
    // Frames 31 to 39 are all a frame names after them; all lost.
    for (std::uint32_t counter = 31; counter < 40; ++counter) (void)writer.frame(counter, false);
    writer.decoder_lost(31, 39);
    const auto after = writer.frame(40, false);
    const auto read = read_hevc_frame(*layout, after);
    CHECK(read && read->restart && read->intra);
    // Counted in sixteen bits: a loss across the count's wrap. Frames 1 and 2 are built on the
    // lost ones, 2 only through 1: the repair is predicted from frame 0xfffe, five frames back.
    for (std::uint32_t counter = 0xfffa; counter < 0x10003; ++counter) (void)writer.frame(counter & 0xffff, false);
    writer.decoder_lost(0xffff, 0x0000);
    CHECK(used_back(*layout, writer.frame(3, false)) == 5);
}
