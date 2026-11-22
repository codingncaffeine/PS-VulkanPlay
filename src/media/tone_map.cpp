#include "media/tone_map.h"

#include <algorithm>
#include <cmath>

namespace psvp::media {

namespace {
// SMPTE ST 2084 constants.
constexpr double kM1 = 2610.0 / 16384.0;
constexpr double kM2 = 2523.0 / 4096.0 * 128.0;
constexpr double kC1 = 3424.0 / 4096.0;
constexpr double kC2 = 2413.0 / 4096.0 * 32.0;
constexpr double kC3 = 2392.0 / 4096.0 * 32.0;
constexpr double kPqPeak = 10000.0;
// ITU-R BT.2100 HLG constants.
constexpr double kHlgA = 0.17883277;
constexpr double kHlgB = 0.28466892;  // 1 - 4a
constexpr double kHlgC = 0.55991073;  // 0.5 - a ln(4a)
constexpr double kHlgGamma = 1.2;     // the system gamma of a 1000 cd/m² display
}  // namespace

double pq_to_nits(double signal) {
    const double p = std::pow(std::clamp(signal, 0.0, 1.0), 1.0 / kM2);
    return kPqPeak * std::pow(std::max(p - kC1, 0.0) / (kC2 - kC3 * p), 1.0 / kM1);
}

double nits_to_pq(double nits) {
    const double y = std::pow(std::clamp(nits / kPqPeak, 0.0, 1.0), kM1);
    return std::pow((kC1 + kC2 * y) / (1.0 + kC3 * y), kM2);
}

double hlg_to_scene(double signal) {
    const double e = std::clamp(signal, 0.0, 1.0);
    return e <= 0.5 ? e * e / 3.0 : (std::exp((e - kHlgC) / kHlgA) + kHlgB) / 12.0;
}

double bt2390_eetf(double signal_pq, double source_peak_pq, double target_peak_pq) {
    if (source_peak_pq <= 0.0) return 0.0;
    const double max_lum = target_peak_pq / source_peak_pq;
    if (max_lum >= 1.0) return std::clamp(signal_pq, 0.0, source_peak_pq);
    const double e1 = std::clamp(signal_pq / source_peak_pq, 0.0, 1.0);
    const double ks = std::max(1.5 * max_lum - 0.5, 0.0);
    double e2 = e1;
    if (e1 >= ks) {
        const double t = (e1 - ks) / (1.0 - ks);
        const double t2 = t * t, t3 = t2 * t;
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * max_lum;
    }
    // Black is 0 on both sides: the step that lifts it (E3 in BT.2390) adds nothing.
    return e2 * source_peak_pq;
}

std::array<double, 3> bt2020_to_bt709(const std::array<double, 3>& c) {
    // From the two sets of primaries and D65 (ITU-R BT.2087 Table 2 gives the same to four places).
    return {
        1.6604910021 * c[0] - 0.5876411388 * c[1] - 0.0728498633 * c[2],
        -0.1245504745 * c[0] + 1.1328998971 * c[1] - 0.0083494226 * c[2],
        -0.0181507634 * c[0] - 0.1005788980 * c[1] + 1.1187296614 * c[2],
    };
}

std::array<double, 3> bt709_to_bt2020(const std::array<double, 3>& c) {
    // The inverse of bt2020_to_bt709's, from the same primaries and D65.
    return {
        0.6274038959 * c[0] + 0.3292830384 * c[1] + 0.0433130657 * c[2],
        0.0690972894 * c[0] + 0.9195403951 * c[1] + 0.0113623156 * c[2],
        0.0163914389 * c[0] + 0.0880133079 * c[1] + 0.8955952532 * c[2],
    };
}

namespace {
// HLG to display light in cd/m² (BT.2020 primaries): inverse OETF, then the OOTF of kHlgPeakNits.
std::array<double, 3> hlg_to_nits(const std::array<double, 3>& encoded) {
    std::array<double, 3> scene{};
    for (std::size_t i = 0; i < 3; ++i) scene[i] = hlg_to_scene(encoded[i]);
    const double ys = 0.2627 * scene[0] + 0.6780 * scene[1] + 0.0593 * scene[2];
    const double gain = ys > 0.0 ? kHlgPeakNits * std::pow(ys, kHlgGamma - 1.0) : 0.0;
    std::array<double, 3> nits{};
    for (std::size_t i = 0; i < 3; ++i) nits[i] = gain * scene[i];
    return nits;
}
}  // namespace

std::array<double, 3> hdr10_signal(const std::array<double, 3>& encoded, ColorTransfer transfer) {
    std::array<double, 3> out{};
    if (transfer == ColorTransfer::pq) {
        for (std::size_t i = 0; i < 3; ++i) out[i] = std::clamp(encoded[i], 0.0, 1.0);
        return out;
    }
    std::array<double, 3> nits{};
    if (transfer == ColorTransfer::hlg) {
        nits = hlg_to_nits(encoded);
    } else {
        std::array<double, 3> linear{};
        for (std::size_t i = 0; i < 3; ++i) linear[i] = srgb_decode(encoded[i]);
        nits = bt709_to_bt2020(linear);
        for (double& value : nits) value *= kSdrWhiteNits;
    }
    for (std::size_t i = 0; i < 3; ++i) out[i] = nits_to_pq(nits[i]);
    return out;
}

double srgb_encode(double linear) {
    const double l = std::clamp(linear, 0.0, 1.0);
    return l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

double srgb_decode(double encoded) {
    const double e = std::clamp(encoded, 0.0, 1.0);
    return e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4);
}

std::array<double, 3> hdr_to_sdr_linear(const std::array<double, 3>& encoded, const ToneMap& tone) {
    // 1. Light in cd/m², BT.2020 primaries.
    std::array<double, 3> nits{};
    double source_peak = kHlgPeakNits;
    if (tone.transfer == ColorTransfer::hlg) {
        nits = hlg_to_nits(encoded);
    } else {
        source_peak = std::clamp(tone.source_peak_nits, kSdrWhiteNits, kMostHdrPeakNits);  // a measured peak can be as low as white
        for (std::size_t i = 0; i < 3; ++i) nits[i] = pq_to_nits(encoded[i]);
    }
    // 2. BT.709 primaries, what is outside them cut off. First: converted after the tone mapping, a
    //    saturated color could come out above white in one channel, and cutting that off turns its hue.
    std::array<double, 3> out = bt2020_to_bt709(nits);
    for (double& value : out) value = std::max(value, 0.0);
    // 3. The brightest channel through the EETF, the others in proportion; 4. SDR white as 1.
    const double brightest = std::max({out[0], out[1], out[2]});
    if (brightest <= 0.0) return {0.0, 0.0, 0.0};
    const double mapped = pq_to_nits(bt2390_eetf(nits_to_pq(brightest), nits_to_pq(source_peak), nits_to_pq(kSdrWhiteNits)));
    const double scale = mapped / brightest / kSdrWhiteNits;
    for (double& value : out) value = std::clamp(value * scale, 0.0, 1.0);
    return out;
}

}  // namespace psvp::media
