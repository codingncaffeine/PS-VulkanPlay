// How bright an HDR picture really is (PLAN D41): the console's streams do not say how bright their
// pictures get, so instead of taking every PQ picture to reach one assumed peak, each picture's
// brightest channel is counted into a histogram (on the graphics card, src/render/shaders/peak.comp),
// and the peak the tone mapping fits into SDR is taken from it:
//
//   - the frame's peak: the PQ signal under which 99.995% of the picture's samples lie (a few
//     stray bright samples do not count), and its average;
//   - smoothed over about 20 pictures, so the curve does not flicker with every frame;
//   - a scene change (the average jumps by 3 dB or more) takes the new peak at once; between 1
//     and 3 dB, partly. (The percentile, the period and the two thresholds are the defaults
//     libplacebo documents for its peak detection.)
//   - never below HDR reference white (a dark scene is not stretched up) nor above 10000 cd/m².
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace psvp::media {

class PeakDetector {
public:
    static constexpr std::size_t kBins = 1024;  // over the PQ signal from 0 to 1
    static constexpr double kPercentile = 99.995;
    static constexpr double kPeriodFrames = 20.0;
    static constexpr double kSceneLowDb = 1.0, kSceneHighDb = 3.0;

    // The bin a PQ signal is counted in (as the shader counts it).
    [[nodiscard]] static std::size_t bin_of(double pq) noexcept;

    struct Frame {
        double peak_pq = 0.0;     // the percentile's PQ signal (the upper edge of its bin)
        double average_pq = 0.0;  // of the samples' bins' middles
    };
    // One picture's histogram; nothing when it counted no samples.
    [[nodiscard]] static std::optional<Frame> measure(std::span<const std::uint32_t, kBins> histogram) noexcept;

    // Takes a picture's histogram and gives the peak to tone map from, in cd/m².
    double add(std::span<const std::uint32_t, kBins> histogram);
    [[nodiscard]] std::optional<double> peak_nits() const noexcept { return peak_nits_; }
    void reset() noexcept { smoothed_.reset(); peak_nits_.reset(); }

private:
    std::optional<Frame> smoothed_;
    std::optional<double> peak_nits_;
};

}  // namespace psvp::media
