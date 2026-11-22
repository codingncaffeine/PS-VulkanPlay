// The generated clip (media/test_clip.h) as HEVC, in the shape a PS5's HEVC
// stream was seen to have (docs/protocol/video.md, "After a lost frame"): the
// parameter sets carry no reference sets; every frame's own header names up to
// nine frames before it, one frame further back each, and marks one of them as
// used: the frame just before it, or, after a loss report, the last frame before
// the loss; the order count has four bits. Lossless: blocks are written as they
// are (PCM) or taken over from the frame used (skipped), so a decoder gives back
// sample for sample what went in. ITU-T H.265 throughout.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "media/test_clip.h"

namespace psvp::media {

class HevcClipWriter final : public ClipEncoder {
public:
    // `hdr`: the stream says it is PQ with BT.2020 primaries and matrix, as a PS5's HDR stream does (the
    // samples stay 8-bit: what is checked with it is what the player does with an HDR picture).
    HevcClipWriter(unsigned width, unsigned height, bool hdr = false);

    // The video, sequence and picture parameter sets.
    [[nodiscard]] std::vector<std::uint8_t> parameter_sets() const override;
    [[nodiscard]] std::vector<std::uint8_t> frame(std::uint32_t counter, bool keyframe) override;
    static constexpr std::size_t kNamed = kKept;  // earlier frames a frame names, at most

private:
    unsigned order_ = 0;  // of the next frame, since the last keyframe
    bool hdr_ = false;
};

}  // namespace psvp::media
