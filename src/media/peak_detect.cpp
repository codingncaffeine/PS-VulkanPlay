#include "media/peak_detect.h"

#include <algorithm>
#include <cmath>

#include "media/tone_map.h"

namespace psvp::media {

std::size_t PeakDetector::bin_of(double pq) noexcept {
    const double at = std::floor(std::clamp(pq, 0.0, 1.0) * static_cast<double>(kBins));
    return std::min(static_cast<std::size_t>(at), kBins - 1);
}

std::optional<PeakDetector::Frame> PeakDetector::measure(std::span<const std::uint32_t, kBins> histogram) noexcept {
    std::uint64_t total = 0;
    double weighted = 0.0;
    for (std::size_t i = 0; i < kBins; ++i) {
        total += histogram[i];
        weighted += static_cast<double>(histogram[i]) * (static_cast<double>(i) + 0.5);
    }
    if (total == 0) return std::nullopt;
    const auto wanted = static_cast<std::uint64_t>(std::ceil(static_cast<double>(total) * kPercentile / 100.0));
    std::uint64_t seen = 0;
    std::size_t at = kBins - 1;
    for (std::size_t i = 0; i < kBins; ++i) {
        seen += histogram[i];
        if (seen >= wanted) {
            at = i;
            break;
        }
    }
    return Frame{static_cast<double>(at + 1) / static_cast<double>(kBins), weighted / static_cast<double>(total) / static_cast<double>(kBins)};
}

double PeakDetector::add(std::span<const std::uint32_t, kBins> histogram) {
    const auto frame = measure(histogram);
    if (!frame) return peak_nits_.value_or(kAssumedHdrPeakNits);
    if (!smoothed_) {
        smoothed_ = *frame;
    } else {
        // How far the scene's average moved, in dB of light (10 log10 of the ratio); darkness floored at 0.01 cd/m².
        const double now = std::max(pq_to_nits(frame->average_pq), 0.01), before = std::max(pq_to_nits(smoothed_->average_pq), 0.01);
        const double moved = std::abs(10.0 * std::log10(now / before));
        const double steady = 1.0 - std::exp(-1.0 / kPeriodFrames);
        const double cut = std::clamp((moved - kSceneLowDb) / (kSceneHighDb - kSceneLowDb), 0.0, 1.0);
        const double weight = std::max(steady, cut);
        smoothed_->peak_pq += weight * (frame->peak_pq - smoothed_->peak_pq);
        smoothed_->average_pq += weight * (frame->average_pq - smoothed_->average_pq);
    }
    peak_nits_ = std::clamp(pq_to_nits(smoothed_->peak_pq), kSdrWhiteNits, kMostHdrPeakNits);
    return *peak_nits_;
}

}  // namespace psvp::media
