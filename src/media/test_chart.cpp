#include "media/test_chart.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "media/tone_map.h"

namespace psvp::media {

namespace {

using Rgb = std::array<double, 3>;

// 10-bit limited range codes of a signal (R'G'B', each 0 to 1) under a matrix's weights.
struct Codes {
    std::uint16_t y = 64, cb = 512, cr = 512;
};
Codes codes_of(const Rgb& signal, bool bt2020) {
    const double kr = bt2020 ? 0.2627 : 0.2126, kb = bt2020 ? 0.0593 : 0.0722, kg = 1.0 - kr - kb;
    const double y = kr * signal[0] + kg * signal[1] + kb * signal[2];
    const double cb = (signal[2] - y) / (2.0 * (1.0 - kb)), cr = (signal[0] - y) / (2.0 * (1.0 - kr));
    const auto code = [](double value) { return static_cast<std::uint16_t>(std::clamp(std::lround(value), 4L, 1019L)); };
    return {code(64.0 + 876.0 * y), code(512.0 + 896.0 * cb), code(512.0 + 896.0 * cr)};
}
// A gray by its code alone (below black and above white too).
Codes gray_code(long y) { return {static_cast<std::uint16_t>(std::clamp(y, 4L, 1019L)), 512, 512}; }
Codes gray_share(double share) { return gray_code(std::lround(64.0 + 876.0 * share)); }

// Linear BT.2020 light as shares of `nits` to PQ codes.
Codes pq_codes(const Rgb& linear, double nits) {
    return codes_of({nits_to_pq(linear[0] * nits), nits_to_pq(linear[1] * nits), nits_to_pq(linear[2] * nits)}, true);
}
Codes pq_gray(double nits) { return codes_of({nits_to_pq(nits), nits_to_pq(nits), nits_to_pq(nits)}, true); }

class Painter {
public:
    explicit Painter(TestChart& chart) : c_(chart) {}
    // A rectangle of one color; corners even (the color samples cover two by two).
    void fill(unsigned x0, unsigned y0, unsigned x1, unsigned y1, const Codes& codes) {
        for (unsigned y = y0; y < y1; ++y) {
            for (unsigned x = x0; x < x1; ++x) c_.y[std::size_t{y} * c_.width + x] = codes.y;
        }
        for (unsigned y = y0 / 2; y < y1 / 2; ++y) {
            for (unsigned x = x0 / 2; x < x1 / 2; ++x) {
                c_.cb[std::size_t{y} * (c_.width / 2) + x] = codes.cb;
                c_.cr[std::size_t{y} * (c_.width / 2) + x] = codes.cr;
            }
        }
    }
    // Patches side by side across [x0, x1), each as wide (the last takes what is left).
    template <typename List>
    void row(unsigned x0, unsigned x1, unsigned y0, unsigned y1, const List& list) {
        const unsigned count = static_cast<unsigned>(std::size(list));
        const unsigned each = (x1 - x0) / count / 2 * 2;
        unsigned at = x0;
        unsigned i = 0;
        for (const Codes& codes : list) {
            const unsigned end = ++i == count ? x1 : at + each;
            fill(at, y0, end, y1, codes);
            at = end;
        }
    }
    void luma(unsigned x, unsigned y, std::uint16_t code) { c_.y[std::size_t{y} * c_.width + x] = code; }

private:
    TestChart& c_;
};

}  // namespace

Picture TestChart::picture() const {
    Picture p;
    p.width = width;
    p.height = height;
    p.bits = 10;
    p.matrix = hdr ? ColorMatrix::bt2020 : ColorMatrix::bt709;
    p.transfer = hdr ? ColorTransfer::pq : ColorTransfer::sdr;
    p.keyframe = true;
    p.planes[0] = reinterpret_cast<const std::uint8_t*>(y.data());
    p.planes[1] = reinterpret_cast<const std::uint8_t*>(cb.data());
    p.planes[2] = reinterpret_cast<const std::uint8_t*>(cr.data());
    p.strides[0] = std::size_t{width} * 2;
    p.strides[1] = p.strides[2] = std::size_t{width / 2} * 2;
    return p;
}

std::string TestChart::notice() const {
    return hdr ? "HDR test picture: where the grays stop differing is your screen's brightest. Esc closes."
               : "SDR test picture: the darkest and the brightest squares must not show. Esc closes.";
}

TestChart make_test_chart(bool hdr) {
    TestChart chart;
    chart.width = kChartWidth;
    chart.height = kChartHeight;
    chart.hdr = hdr;
    chart.y.assign(std::size_t{kChartWidth} * kChartHeight, 64);
    chart.cb.assign(std::size_t{kChartWidth / 2} * (kChartHeight / 2), 512);
    chart.cr.assign(chart.cb.size(), 512);
    Painter paint(chart);
    constexpr unsigned w = kChartWidth;

    // The gradient: the signal from 0 to 1 across, one code at a time (PQ's own steps for HDR).
    for (unsigned y = 0; y < kChartGradientRows; ++y) {
        for (unsigned x = 0; x < w; ++x) paint.luma(x, y, static_cast<std::uint16_t>(64 + std::lround(876.0 * x / (w - 1))));
    }

    if (!hdr) {
        std::array<Codes, 11> grays;
        for (std::size_t i = 0; i < grays.size(); ++i) grays[i] = gray_share(static_cast<double>(i) / 10.0);
        paint.row(0, w, kChartGraysAt, kChartLevelsAt, grays);
        // Black levels on black: -2% (below black), +1%, +2%, +4%; white levels on white: 96%, 98%, 99%, 102% (above white).
        paint.fill(0, kChartLevelsAt, w / 2, kChartBarsAt, gray_share(0.0));
        paint.fill(w / 2, kChartLevelsAt, w, kChartBarsAt, gray_share(1.0));
        const double blacks[] = {-0.02, 0.01, 0.02, 0.04}, whites[] = {0.96, 0.98, 0.99, 1.02};
        for (unsigned i = 0; i < 4; ++i) {
            paint.fill(80 + i * 220, kChartLevelsAt + 70, 80 + i * 220 + 160, kChartLevelsAt + 230, gray_share(blacks[i]));
            paint.fill(w / 2 + 80 + i * 220, kChartLevelsAt + 70, w / 2 + 80 + i * 220 + 160, kChartLevelsAt + 230, gray_share(whites[i]));
        }
        const std::array<Codes, 7> bars{codes_of({0.75, 0.75, 0.75}, false), codes_of({0.75, 0.75, 0.0}, false), codes_of({0.0, 0.75, 0.75}, false),
                                        codes_of({0.0, 0.75, 0.0}, false),   codes_of({0.75, 0.0, 0.75}, false), codes_of({0.75, 0.0, 0.0}, false),
                                        codes_of({0.0, 0.0, 0.75}, false)};
        paint.row(0, w, kChartBarsAt, kChartPrimariesAt, bars);
        const std::array<Codes, 6> full{codes_of({1, 0, 0}, false), codes_of({0, 1, 0}, false), codes_of({0, 0, 1}, false),
                                        codes_of({0, 1, 1}, false), codes_of({1, 0, 1}, false), codes_of({1, 1, 0}, false)};
        paint.row(0, w * 3 / 4, kChartPrimariesAt, kChartHeight, full);
    } else {
        std::array<Codes, std::size(kChartHdrGrays)> grays;
        for (std::size_t i = 0; i < grays.size(); ++i) grays[i] = pq_gray(kChartHdrGrays[i]);
        paint.row(0, w, kChartGraysAt, kChartLevelsAt, grays);
        // Near black on black (0.01, 0.05, 0.1, 0.5 cd/m²); near the top on 10000 (1000, 2000, 4000, 6000).
        paint.fill(0, kChartLevelsAt, w / 2, kChartBarsAt, pq_gray(0.0));
        paint.fill(w / 2, kChartLevelsAt, w, kChartBarsAt, pq_gray(10000.0));
        const double blacks[] = {0.01, 0.05, 0.1, 0.5}, whites[] = {1000.0, 2000.0, 4000.0, 6000.0};
        for (unsigned i = 0; i < 4; ++i) {
            paint.fill(80 + i * 220, kChartLevelsAt + 70, 80 + i * 220 + 160, kChartLevelsAt + 230, pq_gray(blacks[i]));
            paint.fill(w / 2 + 80 + i * 220, kChartLevelsAt + 70, w / 2 + 80 + i * 220 + 160, kChartLevelsAt + 230, pq_gray(whites[i]));
        }
        // BT.709's primaries and secondaries at SDR white, then BT.2020's own.
        const Rgb colors[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 1, 1}, {1, 0, 1}, {1, 1, 0}};
        std::array<Codes, 7> bt709{};
        bt709[0] = pq_gray(203.0);
        for (std::size_t i = 0; i < 6; ++i) bt709[i + 1] = pq_codes(bt709_to_bt2020(colors[i]), 203.0);
        paint.row(0, w, kChartBarsAt, kChartPrimariesAt, bt709);
        std::array<Codes, 6> bt2020{};
        for (std::size_t i = 0; i < 6; ++i) bt2020[i] = pq_codes(colors[i], 203.0);
        paint.row(0, w * 3 / 4, kChartPrimariesAt, kChartHeight, bt2020);
    }
    // A one-pixel checkerboard (black and white, or 0 and 203 cd/m²): how the scaler keeps detail.
    const std::uint16_t dark = hdr ? pq_gray(0.0).y : 64, light = hdr ? pq_gray(203.0).y : 940;
    for (unsigned y = kChartPrimariesAt; y < kChartHeight; ++y) {
        for (unsigned x = w * 3 / 4; x < w; ++x) paint.luma(x, y, ((x + y) % 2 == 0) ? dark : light);
    }
    return chart;
}

}  // namespace psvp::media
