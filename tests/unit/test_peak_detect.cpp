#include <array>
#include <cmath>
#include <cstdint>

#include "check.h"
#include "media/peak_detect.h"
#include "media/tone_map.h"

using namespace psvp::media;

namespace {
using Histogram = std::array<std::uint32_t, PeakDetector::kBins>;

// A picture whose samples are all at one light, and a few brighter strays.
Histogram picture(double nits, std::uint32_t count, double stray_nits = 0.0, std::uint32_t strays = 0) {
    Histogram h{};
    h[PeakDetector::bin_of(nits_to_pq(nits))] += count;
    if (strays != 0) h[PeakDetector::bin_of(nits_to_pq(stray_nits))] += strays;
    return h;
}

bool close(double got, double want, double within) { return std::abs(got - want) <= within; }
}  // namespace

// The percentile: strays under 0.005% of the samples do not count; the peak is its bin's upper edge.
PSVP_TEST(peak_detect_takes_the_percentile) {
    CHECK(PeakDetector::bin_of(0.0) == 0 && PeakDetector::bin_of(1.0) == PeakDetector::kBins - 1 && PeakDetector::bin_of(0.5) == 512);
    CHECK(!PeakDetector::measure(Histogram{}));
    const auto plain = PeakDetector::measure(picture(600.0, 2'000'000));
    const double edge = static_cast<double>(PeakDetector::bin_of(nits_to_pq(600.0)) + 1) / PeakDetector::kBins;
    CHECK(plain && close(plain->peak_pq, edge, 1e-12));
    // 50 strays of 2'000'000 (0.0025%) at 4000 cd/m²: still the 600 cd/m² bin; 200 of them (0.01%): theirs.
    CHECK(close(PeakDetector::measure(picture(600.0, 2'000'000, 4000.0, 50))->peak_pq, edge, 1e-12));
    CHECK(PeakDetector::measure(picture(600.0, 2'000'000, 4000.0, 200))->peak_pq > nits_to_pq(3900.0));
}

// Smoothed over about 20 pictures; a scene change (3 dB or more) at once; within reference white and 10000.
PSVP_TEST(peak_detect_smooths_and_follows_scene_changes) {
    PeakDetector detector;
    CHECK(!detector.peak_nits());
    const double first = detector.add(picture(800.0, 1000));
    CHECK(close(first, pq_to_nits(static_cast<double>(PeakDetector::bin_of(nits_to_pq(800.0)) + 1) / PeakDetector::kBins), 1e-9));
    // A highlight in an unchanged scene (the average moves under 1 dB): the peak moves a 20th of the way.
    Histogram highlight = picture(800.0, 1000);
    highlight[PeakDetector::bin_of(nits_to_pq(2000.0))] += 1;  // 0.1%: above the percentile's cut
    const double nudged = detector.add(highlight);
    CHECK(nudged > first && nits_to_pq(nudged) - nits_to_pq(first) < 0.1 * (nits_to_pq(2000.0) - nits_to_pq(800.0)));
    // A bright new scene (+10 dB): taken at once.
    const double cut = detector.add(picture(4000.0, 1000));
    CHECK(close(nits_to_pq(cut), static_cast<double>(PeakDetector::bin_of(nits_to_pq(4000.0)) + 1) / PeakDetector::kBins, 1e-9));
    // A dark scene: never below reference white.
    PeakDetector dark;
    CHECK(dark.add(picture(20.0, 1000)) == kSdrWhiteNits);
    // An empty histogram changes nothing.
    CHECK(detector.add(Histogram{}) == cut);
    detector.reset();
    CHECK(!detector.peak_nits());
}
