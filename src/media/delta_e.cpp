#include "media/delta_e.h"

#include <cmath>
#include <numbers>

#include "media/tone_map.h"

namespace psvp::media {

namespace {
constexpr double kDegree = std::numbers::pi / 180.0;
double degrees(double radians) {
    const double d = radians / kDegree;
    return d < 0.0 ? d + 360.0 : d;
}
}  // namespace

Lab lab_of_srgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    const double r = srgb_decode(red / 255.0), g = srgb_decode(green / 255.0), b = srgb_decode(blue / 255.0);
    // IEC 61966-2-1 primaries to CIE XYZ, then over the D65 white.
    const double x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047;
    const double y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
    const double z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883;
    const auto f = [](double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0; };
    const double fx = f(x), fy = f(y), fz = f(z);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

double delta_e_2000(const Lab& one, const Lab& two) {
    const double c1 = std::hypot(one.a, one.b), c2 = std::hypot(two.a, two.b);
    const double c_mean7 = std::pow((c1 + c2) / 2.0, 7.0);
    const double g = 0.5 * (1.0 - std::sqrt(c_mean7 / (c_mean7 + std::pow(25.0, 7.0))));
    const double a1 = (1.0 + g) * one.a, a2 = (1.0 + g) * two.a;
    const double c1p = std::hypot(a1, one.b), c2p = std::hypot(a2, two.b);
    const double h1 = (a1 == 0.0 && one.b == 0.0) ? 0.0 : degrees(std::atan2(one.b, a1));
    const double h2 = (a2 == 0.0 && two.b == 0.0) ? 0.0 : degrees(std::atan2(two.b, a2));
    const bool neutral = c1p * c2p == 0.0;

    const double dl = two.l - one.l, dc = c2p - c1p;
    double dh = 0.0;
    if (!neutral) {
        dh = h2 - h1;
        if (dh > 180.0) dh -= 360.0;
        else if (dh < -180.0) dh += 360.0;
    }
    const double dH = 2.0 * std::sqrt(c1p * c2p) * std::sin(dh / 2.0 * kDegree);

    const double l_mean = (one.l + two.l) / 2.0, c_mean = (c1p + c2p) / 2.0;
    double h_mean = h1 + h2;
    if (!neutral) {
        if (std::abs(h1 - h2) <= 180.0) h_mean = (h1 + h2) / 2.0;
        else h_mean = h1 + h2 < 360.0 ? (h1 + h2 + 360.0) / 2.0 : (h1 + h2 - 360.0) / 2.0;
    }
    const double t = 1.0 - 0.17 * std::cos((h_mean - 30.0) * kDegree) + 0.24 * std::cos(2.0 * h_mean * kDegree) +
                     0.32 * std::cos((3.0 * h_mean + 6.0) * kDegree) - 0.20 * std::cos((4.0 * h_mean - 63.0) * kDegree);
    const double turn = 30.0 * std::exp(-std::pow((h_mean - 275.0) / 25.0, 2.0));
    const double c_mean_7 = std::pow(c_mean, 7.0);
    const double rc = 2.0 * std::sqrt(c_mean_7 / (c_mean_7 + std::pow(25.0, 7.0)));
    const double sl = 1.0 + 0.015 * std::pow(l_mean - 50.0, 2.0) / std::sqrt(20.0 + std::pow(l_mean - 50.0, 2.0));
    const double sc = 1.0 + 0.045 * c_mean;
    const double sh = 1.0 + 0.015 * c_mean * t;
    const double rt = -std::sin(2.0 * turn * kDegree) * rc;
    return std::sqrt(std::pow(dl / sl, 2.0) + std::pow(dc / sc, 2.0) + std::pow(dH / sh, 2.0) + rt * (dc / sc) * (dH / sh));
}

}  // namespace psvp::media
