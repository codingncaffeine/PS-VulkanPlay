#include <array>
#include <cmath>

#include "check.h"
#include "media/color.h"
#include "media/delta_e.h"
#include "media/tone_map.h"

using namespace psvp::media;

namespace {
bool close(double got, double want, double within) { return std::abs(got - want) <= within; }
}  // namespace

// SMPTE ST 2084's own table: 100 cd/m² is 0.5081, 1000 is 0.7518, 10000 is 1; and back.
PSVP_TEST(tone_map_pq_matches_st2084) {
    CHECK(close(nits_to_pq(100.0), 0.508078, 1e-6));
    CHECK(close(nits_to_pq(1000.0), 0.751827, 1e-6));
    CHECK(close(nits_to_pq(10000.0), 1.0, 1e-12));
    CHECK(close(nits_to_pq(0.0), 0.0, 1e-6));  // 7.3e-7: ST 2084's offset c1 to the power m2
    CHECK(close(pq_to_nits(0.0), 0.0, 1e-12));
    // ITU-R BT.2408: HDR reference white, 203 cd/m², is 58% of the PQ signal.
    CHECK(close(nits_to_pq(kSdrWhiteNits), 0.5807, 1e-4));
    for (const double nits : {0.01, 1.0, 50.0, 203.0, 640.0, 4000.0, 9999.0}) CHECK(close(pq_to_nits(nits_to_pq(nits)), nits, nits * 1e-9));
}

// ITU-R BT.2100 HLG: the two pieces meet at 0.5 (1/12), 1 is 1; BT.2408: a 75% signal is
// reference white, 203 cd/m², on the 1000 cd/m² display HLG is made for.
PSVP_TEST(tone_map_hlg_matches_bt2100_and_bt2408) {
    CHECK(close(hlg_to_scene(0.5), 1.0 / 12.0, 1e-7));
    CHECK(close(hlg_to_scene(0.5 + 1e-9), 1.0 / 12.0, 1e-7));
    CHECK(close(hlg_to_scene(1.0), 1.0, 1e-6));
    CHECK(close(kHlgPeakNits * std::pow(hlg_to_scene(0.75), 1.2), 203.0, 0.5));
    // So a 75% gray comes out as a PQ gray of 203 cd/m² does (both above the EETF's knee: below white).
    const auto white = hdr_to_sdr_linear({0.75, 0.75, 0.75}, ToneMap{ColorTransfer::hlg});
    const double pq_white = nits_to_pq(203.0);
    CHECK(close(white[0], hdr_to_sdr_linear({pq_white, pq_white, pq_white}, ToneMap{ColorTransfer::pq, kHlgPeakNits})[0], 2e-3));
    CHECK(close(white[0], white[1], 1e-12) && close(white[1], white[2], 1e-12));
    // Its brightest is the display's peak: the screen's white.
    CHECK(close(hdr_to_sdr_linear({1.0, 1.0, 1.0}, ToneMap{ColorTransfer::hlg})[1], 1.0, 1e-9));
}

// ITU-R BT.2390 5.4.1: below the knee nothing changes, the source's peak becomes the target's,
// the curve rises all the way and has no step or bend at the knee; a source no brighter than
// the target is left alone.
PSVP_TEST(tone_map_bt2390_eetf) {
    const double source = nits_to_pq(1000.0), target = nits_to_pq(203.0);
    const double knee = (1.5 * target / source - 0.5) * source;
    CHECK(close(bt2390_eetf(0.3, source, target), 0.3, 1e-12));
    CHECK(close(bt2390_eetf(knee - 1e-9, source, target), knee - 1e-9, 1e-12));
    CHECK(close(bt2390_eetf(source, source, target), target, 1e-12));
    CHECK(close(bt2390_eetf(1.0, source, target), target, 1e-12));  // beyond the stated peak: the peak
    double previous = -1.0;
    for (int i = 0; i <= 1000; ++i) {
        const double out = bt2390_eetf(source * i / 1000.0, source, target);
        CHECK(out >= previous);
        CHECK(out <= target + 1e-12);
        previous = out;
    }
    const double step = 1e-6;
    const double slope_after = (bt2390_eetf(knee + step, source, target) - bt2390_eetf(knee, source, target)) / step;
    CHECK(close(slope_after, 1.0, 1e-3));
    CHECK(close(bt2390_eetf(0.4, target, source), 0.4, 1e-12));
}

// ITU-R BT.2087 Table 2 (to its four places); white stays white.
PSVP_TEST(tone_map_bt2020_to_bt709_matches_bt2087) {
    const auto red = bt2020_to_bt709({1.0, 0.0, 0.0});
    const auto green = bt2020_to_bt709({0.0, 1.0, 0.0});
    const auto blue = bt2020_to_bt709({0.0, 0.0, 1.0});
    CHECK(close(red[0], 1.6605, 5e-5) && close(red[1], -0.1246, 5e-5) && close(red[2], -0.0182, 5e-5));
    CHECK(close(green[0], -0.5876, 5e-5) && close(green[1], 1.1329, 5e-5) && close(green[2], -0.1006, 5e-5));
    CHECK(close(blue[0], -0.0728, 5e-5) && close(blue[1], -0.0083, 5e-5) && close(blue[2], 1.1187, 5e-5));
    const auto white = bt2020_to_bt709({1.0, 1.0, 1.0});
    for (const double value : white) CHECK(close(value, 1.0, 1e-9));
}

PSVP_TEST(tone_map_pq_to_sdr) {
    const ToneMap tone{ColorTransfer::pq, 1000.0};
    const auto gray = [&](double nits) {
        const double e = nits_to_pq(nits);
        return hdr_to_sdr_linear({e, e, e}, tone);
    };
    // Below the knee (about 95 cd/m² for a 1000 cd/m² source) a gray is its light over SDR white.
    CHECK(close(gray(50.0)[0], 50.0 / 203.0, 1e-9));
    CHECK(close(gray(5.0)[1], 5.0 / 203.0, 1e-9));
    // The source's peak and anything brighter is the screen's white; reference white sits below it.
    CHECK(close(gray(1000.0)[0], 1.0, 1e-9));
    CHECK(close(gray(4000.0)[2], 1.0, 1e-9));
    CHECK(gray(203.0)[0] > 0.6 && gray(203.0)[0] < 0.8);
    CHECK(close(gray(0.0)[0], 0.0, 1e-6));
    // A brighter stated peak leaves less room below white: the same light comes out darker.
    CHECK(hdr_to_sdr_linear({nits_to_pq(400.0), nits_to_pq(400.0), nits_to_pq(400.0)}, ToneMap{ColorTransfer::pq, 4000.0})[0] < gray(400.0)[0]);
    // A peak out of range is taken as the nearest that is not: reference white at the least (a measured
    // peak can be that low), where the curve leaves the picture as it is.
    CHECK(close(hdr_to_sdr_linear({0.7, 0.7, 0.7}, ToneMap{ColorTransfer::pq, 5.0})[0],
                hdr_to_sdr_linear({0.7, 0.7, 0.7}, ToneMap{ColorTransfer::pq, kSdrWhiteNits})[0], 1e-12));
    // A color keeps its proportions: all three channels scaled by the same amount (hue and saturation stay).
    const std::array<double, 3> nits{600.0, 300.0, 120.0};
    const auto out = hdr_to_sdr_linear({nits_to_pq(nits[0]), nits_to_pq(nits[1]), nits_to_pq(nits[2])}, tone);
    const auto unscaled = bt2020_to_bt709(nits);
    CHECK(close(out[0] / unscaled[0], out[1] / unscaled[1], 1e-9) && close(out[1] / unscaled[1], out[2] / unscaled[2], 1e-9));
    // A color outside BT.709 (BT.2020's own green) is cut to it, never below 0 or above 1.
    const auto wide = hdr_to_sdr_linear({0.0, nits_to_pq(500.0), 0.0}, tone);
    for (const double value : wide) CHECK(value >= 0.0 && value <= 1.0);
    CHECK(wide[0] == 0.0 && wide[2] == 0.0 && wide[1] > 0.5);
}

// The screen colors the stream window's pixels are checked against: a 10-bit, limited range PQ
// gray of 50 cd/m² (code 450) is 50/203 of white in light, sRGB-encoded.
PSVP_TEST(tone_map_screen_color) {
    const ColorTransform t = color_transform(ColorMatrix::bt2020, false, 10);
    const ToneMap tone{ColorTransfer::pq, 1000.0};
    const int want = static_cast<int>(std::lround(srgb_encode(pq_to_nits((450.0 - 64.0) / 876.0) / 203.0) * 255.0));
    const auto got = screen_color(t, 10, 450, 512, 512, tone);
    CHECK(got[0] == want && got[1] == want && got[2] == want);
    CHECK(want > 128 && want < 140);  // 50/203 of white is 0.246 in light, 0.534 encoded
    CHECK(screen_color(t, 10, 940, 512, 512, tone)[0] == 255);
    CHECK(screen_color(t, 10, 64, 512, 512, tone)[0] == 0);
    // SDR pictures are as before.
    CHECK(screen_color(t, 10, 450, 512, 512)[0] == static_cast<int>(std::lround((450.0 - 64.0) / 876.0 * 255.0)));
}

// CIEDE2000 against the test data of Sharma, Wu and Dalal (2005), pairs 1-4, 7, 17 and 25,
// to the four places they give, both ways round.
PSVP_TEST(delta_e_2000_matches_sharma_wu_dalal) {
    struct Pair {
        Lab one, two;
        double want;
    };
    const Pair pairs[] = {
        {{50.0, 2.6772, -79.7751}, {50.0, 0.0, -82.7485}, 2.0425},
        {{50.0, 3.1571, -77.2803}, {50.0, 0.0, -82.7485}, 2.8615},
        {{50.0, 2.8361, -74.0200}, {50.0, 0.0, -82.7485}, 3.4412},
        {{50.0, -1.3802, -84.2814}, {50.0, 0.0, -82.7485}, 1.0000},
        {{50.0, 0.0, 0.0}, {50.0, -1.0, 2.0}, 2.3669},
        {{50.0, 2.5, 0.0}, {73.0, 25.0, -18.0}, 27.1492},
        {{60.2574, -34.0099, 36.2677}, {60.4626, -34.1751, 39.4387}, 1.2644},
    };
    for (const Pair& pair : pairs) {
        CHECK(close(delta_e_2000(pair.one, pair.two), pair.want, 1e-4));
        CHECK(close(delta_e_2000(pair.two, pair.one), pair.want, 1e-4));
    }
    // sRGB white is L 100, a and b 0; black is 0.
    const Lab white = lab_of_srgb(255, 255, 255), black = lab_of_srgb(0, 0, 0);
    CHECK(close(white.l, 100.0, 1e-3) && close(white.a, 0.0, 1e-3) && close(white.b, 0.0, 1e-3));
    CHECK(close(black.l, 0.0, 1e-9));
    // Seen on a tone mapped picture of a console (2026-11-21): 15 steps apart in red, unseen.
    CHECK(delta_e_2000(lab_of_srgb(5, 238, 121), lab_of_srgb(20, 237, 121)) < 0.3);
}

// HDR10 to the screen (PLAN D42): PQ as it is; SDR white at HDR reference white (BT.2408: 58% of
// the PQ signal) in every channel, its primaries moved into BT.2020 (a pure BT.709 red is inside
// BT.2020: no channel below 0); an HLG 75% gray at reference white too; the two primaries matrices
// each other's inverse.
PSVP_TEST(tone_map_hdr10_signal_follows_bt2100_and_bt2408) {
    const auto pq = hdr10_signal({0.1, 0.5, 0.9}, ColorTransfer::pq);
    CHECK(close(pq[0], 0.1, 1e-12) && close(pq[1], 0.5, 1e-12) && close(pq[2], 0.9, 1e-12));
    const auto white = hdr10_signal({1.0, 1.0, 1.0}, ColorTransfer::sdr);
    for (const double channel : white) CHECK(close(channel, nits_to_pq(kSdrWhiteNits), 1e-9) && close(channel, 0.5807, 1e-4));
    const auto black = hdr10_signal({0.0, 0.0, 0.0}, ColorTransfer::sdr);
    for (const double channel : black) CHECK(channel < 1e-6);
    const auto red = hdr10_signal({1.0, 0.0, 0.0}, ColorTransfer::sdr);
    CHECK(close(red[0], nits_to_pq(0.6274038959 * kSdrWhiteNits), 1e-9));
    CHECK(red[1] > 0.0 && red[2] > 0.0 && red[0] > red[1] && red[1] > red[2]);
    const auto hlg = hdr10_signal({0.75, 0.75, 0.75}, ColorTransfer::hlg);
    CHECK(close(pq_to_nits(hlg[0]), 203.0, 0.5) && close(hlg[0], hlg[1], 1e-12) && close(hlg[1], hlg[2], 1e-12));
    CHECK(close(hdr10_signal({1.0, 1.0, 1.0}, ColorTransfer::hlg)[1], nits_to_pq(kHlgPeakNits), 1e-6));  // BT.2100 gives 1 at 1 to 1e-6
    for (const auto& c : {std::array<double, 3>{1.0, 0.0, 0.0}, std::array<double, 3>{0.2, 0.7, 0.4}}) {
        const auto back = bt2020_to_bt709(bt709_to_bt2020(c));
        for (std::size_t i = 0; i < 3; ++i) CHECK(close(back[i], c[i], 1e-9));
    }
}
