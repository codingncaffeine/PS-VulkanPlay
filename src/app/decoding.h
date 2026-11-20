// Decodes a stream's picture and sound frames as they come and counts what came
// of it: the check that what arrives is something a player can show and play.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "media/sound_decoder.h"
#include "media/video_decoder.h"

namespace psvp::app {

class Decoding {
public:
    struct Options {
        // The stream is the mock console's generated clip: every picture is compared with
        // the frame of the clip it should be, and the sound with the clip's tone.
        bool expect_clip = false;
    };
    explicit Decoding(Options options) : options_(options) {}

    // One picture frame, in the stream's order. `header` is what the console sent for the picture
    // size the frame is in; `size_index` tells one size from another; `lost_before` is the number
    // of frames that did not arrive since the frame before.
    void video(std::span<const std::uint8_t> header, unsigned size_index, std::uint16_t index, std::span<const std::uint8_t> frame,
               std::uint32_t lost_before);
    // One sound frame, in order.
    void sound(unsigned rate, unsigned channels, unsigned frame_samples, std::span<const std::uint8_t> frame, std::uint32_t lost_before);

    // What came of it, a line each.
    [[nodiscard]] std::vector<std::string> report() const;
    // With expect_clip: every picture that came out was its frame's, except the ones between a
    // lost frame and the console's repair, and the tone was heard. Without: something was decoded.
    [[nodiscard]] bool good() const;

private:
    Options options_;
    std::optional<media::VideoDecoder> video_;
    std::optional<unsigned> size_index_;
    std::string video_error_;
    std::string picture_text_;       // the first picture's size and colors
    std::uint64_t frames_ = 0, pictures_ = 0, damaged_ = 0, refused_ = 0, undecodable_ = 0;
    std::uint64_t microseconds_ = 0, slowest_microseconds_ = 0;
    std::string decoder_text_;
    // The clip.
    std::uint64_t clip_right_ = 0, clip_wrong_ = 0, clip_wrong_after_loss_ = 0;
    std::optional<std::uint16_t> clip_first_wrong_;
    bool after_loss_ = false;        // a frame was lost and no picture was right since
    std::uint64_t longest_repair_ = 0, repair_run_ = 0;  // pictures from a loss to the first right one

    std::optional<media::SoundDecoder> sound_;
    std::string sound_error_;
    std::string sound_text_;
    std::uint64_t sound_frames_ = 0;
    std::vector<std::int16_t> samples_;  // the first minute, for the tone
};

}  // namespace psvp::app
