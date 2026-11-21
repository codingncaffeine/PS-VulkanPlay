// From an HDR picture's colors to a screen that shows SDR: the numbers, worked in doubles.
// The stream window's shader (src/render/shaders/video.frag) does the same steps; this is what
// its pixels are checked against (psvp_render_check --pattern).
//
//   1. the transfer function undone: PQ (SMPTE ST 2084) to light in cd/m², or HLG (ITU-R BT.2100)
//      through its inverse OETF and the OOTF of a 1000 cd/m² display
//   2. BT.2020 primaries to BT.709 (ITU-R BT.2087), what is outside BT.709 cut off
//   3. the brightest channel brought into the screen's range with the EETF of ITU-R BT.2390
//      (section 5.4.1), in PQ, the other two channels kept in proportion (hue and saturation stay)
//   4. HDR reference white (203 cd/m², ITU-R BT.2408) as the screen's white
#pragma once

#include <array>

#include "media/video_decoder.h"

namespace psvp::media {

// ITU-R BT.2408: the HDR level of a white sheet of paper, shown as SDR white.
inline constexpr double kSdrWhiteNits = 203.0;
// What a PQ picture without a stated peak is taken to reach (the console's streams state none:
// no mastering display or light level message, seen 2026-11-19): ITU-R BT.2408's reference display.
inline constexpr double kAssumedHdrPeakNits = 1000.0;
inline constexpr double kLeastHdrPeakNits = 400.0;
inline constexpr double kMostHdrPeakNits = 10000.0;
// The display HLG is made for (ITU-R BT.2100 Table 5, nominal peak).
inline constexpr double kHlgPeakNits = 1000.0;

// SMPTE ST 2084: a signal from 0 to 1 and light from 0 to 10000 cd/m².
[[nodiscard]] double pq_to_nits(double signal);
[[nodiscard]] double nits_to_pq(double nits);

// ITU-R BT.2100: an HLG signal from 0 to 1 to scene light from 0 to 1.
[[nodiscard]] double hlg_to_scene(double signal);

// ITU-R BT.2390 EETF with black at 0 on both sides: a PQ signal of a picture that reaches
// `source_peak_pq` brought into a display that reaches `target_peak_pq`. Unchanged below the knee;
// a picture no brighter than the display is not changed at all.
[[nodiscard]] double bt2390_eetf(double signal_pq, double source_peak_pq, double target_peak_pq);

// Linear light, BT.2020 primaries to BT.709 primaries (D65 both).
[[nodiscard]] std::array<double, 3> bt2020_to_bt709(const std::array<double, 3>& linear);

// IEC 61966-2-1 (sRGB), linear light from 0 to 1 to the screen's encoding and back.
[[nodiscard]] double srgb_encode(double linear);
[[nodiscard]] double srgb_decode(double encoded);

struct ToneMap {
    ColorTransfer transfer = ColorTransfer::sdr;
    double source_peak_nits = kAssumedHdrPeakNits;  // for PQ; HLG's is kHlgPeakNits
};

// Red, green and blue as the picture encodes them (each 0 to 1, BT.2020 primaries) to linear light
// with BT.709 primaries where 1 is SDR white, each 0 to 1. Only for a transfer other than sdr.
[[nodiscard]] std::array<double, 3> hdr_to_sdr_linear(const std::array<double, 3>& encoded, const ToneMap& tone);

}  // namespace psvp::media
