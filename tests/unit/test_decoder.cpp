#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "media/sound_decoder.h"
#include "media/test_clip.h"
#include "media/video_decoder.h"

using namespace psvp::media;

PSVP_TEST(clip_shows_its_counter) {
    // Frame 0x8001: the first and the last of the sixteen blocks are white in the first row.
    CHECK(clip_block(0x8001, 0, 0, 26, 15).y == 235 && clip_block(0x8001, 15, 0, 26, 15).y == 235);
    CHECK(clip_block(0x8001, 1, 0, 26, 15).y == 16);
    // The second row shows the bits the other way round.
    CHECK(clip_block(0x8001, 0, 1, 26, 15).y == 16 && clip_block(0x8001, 1, 1, 26, 15).y == 235);
    // Beside the counter and in the patches' row beyond the patches: gray.
    CHECK(clip_block(0x8001, 16, 0, 26, 15).y == 64 && clip_block(0x8001, 8, 2, 26, 15).y == 64);
    // A red patch has more red difference than blue difference.
    CHECK(clip_block(7, 5, 2, 26, 15).cr > 200 && clip_block(7, 5, 2, 26, 15).cb < 128);
    // The moving block: frame 30 of a clip 26 wide is in the second row below the patches, fifth place.
    CHECK(clip_block(30, 4, 4, 26, 15).y == 235 && clip_block(31, 4, 4, 26, 15).y == 64 && clip_block(31, 5, 4, 26, 15).y == 235);
}

PSVP_TEST(codec_is_told_from_the_header) {
    const std::vector<std::uint8_t> h264{0, 0, 0, 1, 0x67, 0x42};
    const std::vector<std::uint8_t> h264_short{0, 0, 1, 0x27, 0x42};
    const std::vector<std::uint8_t> hevc{0, 0, 0, 1, 0x40, 0x01};
    const std::vector<std::uint8_t> slice{0, 0, 0, 1, 0x65, 0x88};
    const std::vector<std::uint8_t> no_start{0x67, 0x42, 0, 0, 1};
    CHECK(codec_of_header(h264) == VideoCodec::h264);
    CHECK(codec_of_header(h264_short) == VideoCodec::h264);
    CHECK(codec_of_header(hevc) == VideoCodec::hevc);
    CHECK(!codec_of_header(slice));
    CHECK(!codec_of_header(no_start));
    CHECK(!codec_of_header({}));
    CHECK(codec_of_header(ClipWriter(416, 240).parameter_sets()) == VideoCodec::h264);
}

// What goes into the clip comes out of the decoder, sample for sample, one picture per frame.
PSVP_TEST(clip_decodes_to_the_same_samples) {
    for (const auto& [width, height] : {std::pair{416U, 240U}, std::pair{320U, 180U}, std::pair{258U, 66U}}) {
        ClipWriter writer(width, height);
        auto decoder = VideoDecoder::open(VideoCodec::h264, writer.parameter_sets());
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
        // A frame that is not a keyframe carries only the blocks that changed, 384 bytes each. The most
        // change here from 255 to 256: nine bits in two rows, and the moving block's two places.
        CHECK(largest_later_frame > 20 * 384 && largest_later_frame < 20 * 390);
    }
}

// The checks above can fail: a picture is not another frame's, and a blank one shows no counter.
PSVP_TEST(clip_readers_see_a_wrong_picture) {
    ClipWriter writer(416, 240);
    auto decoder = VideoDecoder::open(VideoCodec::h264, writer.parameter_sets());
    CHECK(decoder.has_value());
    if (!decoder) return;
    const auto picture = decoder->decode(writer.frame(1000, true));
    CHECK(picture.has_value());
    if (!picture) return;
    CHECK(clip_differences(picture->view(), 1000) == 0);
    // Frame 1001 differs in one counter bit (two blocks) and in where the moving block is (two blocks).
    CHECK(clip_differences(picture->view(), 1001) == 4 * 256);
    std::vector<std::uint8_t> blank(416 * 240, 64);
    const PlanesView nothing{blank.data(), blank.data(), blank.data(), 416, 208, 208, 416, 240};
    CHECK(!read_clip_counter(nothing));
}

// A frame is lost. The frames after it still decode, but show what the decoder kept from
// before the loss wherever the lost frame changed something; the first frame written
// against what the decoder has puts the picture right again, without a keyframe.
PSVP_TEST(clip_recovers_from_a_lost_frame_without_a_keyframe) {
    ClipWriter writer(416, 240);
    auto decoder = VideoDecoder::open(VideoCodec::h264, writer.parameter_sets());
    CHECK(decoder.has_value());
    if (!decoder) return;
    for (std::uint32_t counter = 100; counter < 110; ++counter) {
        const auto picture = decoder->decode(writer.frame(counter, false));
        CHECK(picture && clip_differences(picture->view(), counter) == 0);
    }
    (void)writer.frame(110, false);  // written, never decoded
    for (std::uint32_t counter = 111; counter < 114; ++counter) {
        const auto picture = decoder->decode(writer.frame(counter, false));
        CHECK(picture.has_value());
        // The moving block's place of frame 109 was grayed by frame 110: it stays white.
        if (picture) CHECK(clip_differences(picture->view(), counter) >= 256);
    }
    writer.decoder_lost(110, 110);
    const auto repaired = decoder->decode(writer.frame(114, false));
    CHECK(repaired && !repaired->keyframe && clip_differences(repaired->view(), 114) == 0);
    const auto next = decoder->decode(writer.frame(115, false));
    CHECK(next && clip_differences(next->view(), 115) == 0);
    CHECK(decoder->counters().pictures == 15 && decoder->counters().refused == 0);

    // Two lost in a row, told of one after the other, and a frame older than any kept: nothing breaks.
    (void)writer.frame(116, false);
    (void)writer.frame(117, false);
    writer.decoder_lost(116, 116);
    writer.decoder_lost(117, 117);
    writer.decoder_lost(3, 4);
    const auto again = decoder->decode(writer.frame(118, false));
    CHECK(again && clip_differences(again->view(), 118) == 0);
}

PSVP_TEST(decoder_survives_what_is_not_a_frame) {
    ClipWriter writer(416, 240);
    auto decoder = VideoDecoder::open(VideoCodec::h264, writer.parameter_sets());
    CHECK(decoder.has_value());
    if (!decoder) return;
    std::vector<std::uint8_t> noise(700);
    for (std::size_t i = 0; i < noise.size(); ++i) noise[i] = static_cast<std::uint8_t>(i * 37 + 11);
    CHECK(!decoder->decode(noise));
    CHECK(!decoder->decode({}));
    // A frame that is not a keyframe, before any keyframe: nothing to build it on.
    ClipWriter other(416, 240);
    (void)other.frame(1, true);
    const auto orphan = decoder->decode(other.frame(2, false));
    CHECK(!orphan || orphan->damaged || clip_differences(orphan->view(), 2) != 0);
    // After a restart, a keyframe is decoded as if nothing had been.
    decoder->restart();
    const auto picture = decoder->decode(writer.frame(5, true));
    CHECK(picture && clip_differences(picture->view(), 5) == 0);
    CHECK(decoder->counters().refused >= 2);
}

namespace {

// tests/data/clip.h265 (tools/make-hevc-clip.sh): the three parameter sets, then a frame per unit.
struct HevcClip {
    std::vector<std::uint8_t> header;
    std::vector<std::vector<std::uint8_t>> frames;
};
HevcClip read_hevc_clip() {
    std::ifstream file(std::string(PSVP_TEST_DATA) + "/clip.h265", std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // Every unit begins with the bytes 0 0 1, some with one more 0 in front.
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 3 <= bytes.size(); ++i) {
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) starts.push_back(i > 0 && bytes[i - 1] == 0 ? i - 1 : i);
    }
    HevcClip clip;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(starts[i]);
        const auto end = i + 1 < starts.size() ? bytes.begin() + static_cast<std::ptrdiff_t>(starts[i + 1]) : bytes.end();
        if (i < 3) clip.header.insert(clip.header.end(), begin, end);
        else clip.frames.emplace_back(begin, end);
    }
    return clip;
}

}  // namespace

PSVP_TEST(hevc_clip_decodes) {
    const HevcClip clip = read_hevc_clip();
    CHECK(clip.header.size() == 87 && clip.frames.size() == 48);
    CHECK(codec_of_header(clip.header) == VideoCodec::hevc);
    auto decoder = VideoDecoder::open(VideoCodec::hevc, clip.header);
    CHECK(decoder.has_value());
    if (!decoder) return;
    std::size_t as_made = 0;
    for (std::size_t i = 0; i < clip.frames.size(); ++i) {
        const auto picture = decoder->decode(clip.frames[i]);
        if (picture && picture->width == 320 && picture->height == 180 && picture->bits == 8 && !picture->full_range &&
            picture->matrix == ColorMatrix::bt709 && picture->transfer == ColorTransfer::sdr && !picture->damaged && picture->keyframe == (i == 0)) {
            ++as_made;
        }
    }
    CHECK(as_made == 48);
    CHECK(decoder->counters().pictures == 48 && decoder->counters().refused == 0 && decoder->counters().damaged == 0);
}

// Every frame of this stream names the four frames before it, as a console's names nine.
// When one is lost, each later frame names a frame the decoder does not have, or one built
// on such a frame. A decoder that gives no picture for those never shows one again; this
// one must give a picture for every frame that arrives, as a console's repair needs it to.
PSVP_TEST(hevc_decoder_goes_on_after_a_lost_frame) {
    const HevcClip clip = read_hevc_clip();
    CHECK(clip.frames.size() == 48);
    auto decoder = VideoDecoder::open(VideoCodec::hevc, clip.header);
    CHECK(decoder.has_value());
    if (!decoder || clip.frames.size() != 48) return;
    std::size_t pictures_after = 0;
    for (std::size_t i = 0; i < clip.frames.size(); ++i) {
        if (i == 20) continue;  // lost
        const auto picture = decoder->decode(clip.frames[i]);
        if (i < 20) CHECK(picture && !picture->damaged);
        if (i > 20 && picture) ++pictures_after;
    }
    CHECK(pictures_after == 27);
    CHECK(decoder->counters().pictures == 47 && decoder->counters().refused == 0);
}

PSVP_TEST(tone_decodes_to_its_two_tones) {
    ToneWriter writer;
    auto decoder = SoundDecoder::open(ToneWriter::kRate, ToneWriter::kChannels, ToneWriter::kFrameSamples);
    CHECK(decoder.has_value());
    if (!decoder) return;
    std::vector<std::int16_t> samples;
    for (unsigned index = 0; index < 100; ++index) {
        const auto frame = writer.frame();
        CHECK(frame.size() == 80);
        const auto decoded = decoder->decode(frame);
        CHECK(decoded.size() == 960);
        if (index >= 10) samples.insert(samples.end(), decoded.begin(), decoded.end());  // past the codec's start
    }
    CHECK(tone_share(samples, 2, 0, 48000, 440.0) > 0.9);
    CHECK(tone_share(samples, 2, 1, 48000, 880.0) > 0.9);
    // Not the other channel's tone, and not silence.
    CHECK(tone_share(samples, 2, 0, 48000, 880.0) < 0.05);
    CHECK(tone_share(samples, 2, 1, 48000, 440.0) < 0.05);
    CHECK(decoder->counters().frames == 100 && decoder->counters().refused == 0);

    // A frame that did not arrive is filled with sound that goes on, not with silence.
    const auto filled = decoder->conceal();
    CHECK(filled.size() == 960 && tone_share(filled, 2, 0, 48000, 440.0) > 0.5);
    CHECK(decoder->counters().concealed == 1);
    // Nothing is not a frame.
    const auto silence = decoder->decode({});
    CHECK(silence.size() == 960 && silence[0] == 0 && silence[959] == 0);
    CHECK(decoder->counters().refused == 1);
}

PSVP_TEST(sound_decoder_refuses_formats_opus_does_not_have) {
    CHECK(!SoundDecoder::open(44100, 2, 441));
    CHECK(!SoundDecoder::open(48000, 6, 480));
    CHECK(!SoundDecoder::open(48000, 2, 0));
    CHECK(SoundDecoder::open(48000, 2, 480).has_value());
    CHECK(tone_share({}, 2, 0, 48000, 440.0) == 0.0);
}
