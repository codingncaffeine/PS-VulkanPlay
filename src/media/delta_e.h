// How different two colors look: CIEDE2000 (CIE 142-2001), as implemented and tested by
// Sharma, Wu and Dalal (2005). About 1 is the smallest difference an eye notices side by side.
// For checks: a tone mapped HDR picture drawn two ways (the graphics card's decoder, software)
// may differ by several steps of 255 in a channel near black that nobody can see.
#pragma once

#include <cstdint>

namespace psvp::media {

struct Lab {
    double l = 0.0, a = 0.0, b = 0.0;
};

// An sRGB screen color (IEC 61966-2-1, D65) in CIELAB, white = 100.
[[nodiscard]] Lab lab_of_srgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue);

[[nodiscard]] double delta_e_2000(const Lab& first, const Lab& second);

}  // namespace psvp::media
