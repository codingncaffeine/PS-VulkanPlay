// From a picture's samples to the screen's colors: the numbers of the conversion.
#pragma once

#include <array>

#include "media/tone_map.h"
#include "media/video_decoder.h"

namespace psvp::media {

// Sixteen numbers, column by column: multiplied with (y, cb, cr, 1), each sample
// as a share of its largest value, they give red, green and blue from 0 to 1, still
// encoded the way the picture's transfer function encodes them. Rows: red, green, blue, 1.
using ColorTransform = std::array<float, 16>;

// For samples of `bits` bits. The limited range holds black at 16 and white at 235
// of 255 (color differences from 16 to 240), scaled up for more bits; the full range
// uses every value (ITU-R BT.601, BT.709, BT.2020; ITU-T H.273 for the ranges).
[[nodiscard]] ColorTransform color_transform(ColorMatrix matrix, bool full_range, unsigned bits);

// What the transform makes of one sample triple, as the bytes of an 8-bit screen that shows SDR
// (sRGB). An HDR picture's colors go through `tone` first (media/tone_map.h).
[[nodiscard]] std::array<int, 3> screen_color(const ColorTransform& transform, unsigned bits, unsigned y, unsigned cb, unsigned cr, const ToneMap& tone = {});

// Where a picture goes in a target of another shape: as large as fits, centered, bars beside or above it.
struct Placement {
    int x = 0, y = 0;
    unsigned width = 0, height = 0;
    friend bool operator==(const Placement&, const Placement&) = default;
};
[[nodiscard]] Placement place_picture(unsigned picture_width, unsigned picture_height, unsigned target_width, unsigned target_height);

}  // namespace psvp::media
