#include "media/frame_pacer.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace psvp::media {

FramePacer::FramePacer(double frames_per_second, Duration cushion) noexcept
    : period_(static_cast<std::int64_t>(1e6 / (frames_per_second > 0.0 ? frames_per_second : 60.0))),
      nominal_(period_),
      cushion_(std::clamp(cushion, Duration{0}, kMostCushion)) {}

void FramePacer::follow_clock() noexcept {
    // The times in their order, as microseconds after the oldest; their numbers 0, 1, 2, ...
    const std::size_t n = times_count_;
    const std::size_t first = times_count_ < kRateMemory ? 0 : times_at_;
    const TimePoint origin = times_[first];
    const auto time_of = [&](std::size_t k) { return std::chrono::duration<double, std::micro>(times_[(first + k) % kRateMemory] - origin).count(); };
    const std::int64_t first_number = numbers_[first];
    const auto number_of = [&](std::size_t k) { return static_cast<double>(numbers_[(first + k) % kRateMemory] - first_number); };
    // A line through the points by least squares, of those no further above `line` than `above`.
    struct Line {
        double slope = 0.0, at_zero = 0.0;
    };
    const auto fit = [&](const Line* line, double above) -> std::optional<Line> {
        double count = 0.0, sum_k = 0.0, sum_t = 0.0, sum_kk = 0.0, sum_kt = 0.0;
        for (std::size_t k = 0; k < n; ++k) {
            const double kd = number_of(k), t = time_of(k);
            if (line != nullptr && t - (line->at_zero + line->slope * kd) > above) continue;
            count += 1.0;
            sum_k += kd;
            sum_t += t;
            sum_kk += kd * kd;
            sum_kt += kd * t;
        }
        const double spread = count * sum_kk - sum_k * sum_k;
        if (count < static_cast<double>(kRateLeast) || !(spread > 0.0)) return std::nullopt;
        const double slope = (count * sum_kt - sum_k * sum_t) / spread;
        return Line{slope, (sum_t - slope * sum_k) / count};
    };
    // A frame can come late, never early: the second line leaves out what lies more than a
    // millisecond above the first (the frames of a pause and the bunch behind it).
    const auto rough = fit(nullptr, 0.0);
    if (!rough) return;
    const auto line = fit(&*rough, 1000.0);
    if (!line) return;
    const auto nominal = static_cast<double>(nominal_.count());
    // Drift, followed (rounded down: a picture never waits a fraction longer with every frame);
    // further than that, a game's own rate: the frame time asked for.
    period_ = std::abs(line->slope - nominal) <= nominal * kMostDrift ? Duration(static_cast<std::int64_t>(std::floor(line->slope))) : nominal_;
}

FramePacer::Duration FramePacer::spare() const noexcept {
    if (waits_count_ < kMemory) return Duration{0};
    return *std::min_element(waits_.begin(), waits_.end());
}

std::optional<FramePacer::Duration> FramePacer::usual_least() const noexcept {
    if (waits_count_ < kLeastKnown) return std::nullopt;
    std::array<Duration, kMemory> sorted = waits_;
    const auto end = sorted.begin() + static_cast<std::ptrdiff_t>(waits_count_);
    const auto place = sorted.begin() + static_cast<std::ptrdiff_t>(kSeldom - 1);
    std::nth_element(sorted.begin(), place, end);
    return *place;
}

FramePacer::Slot FramePacer::schedule(TimePoint earliest, TimePoint ready, std::size_t waiting, std::optional<std::uint16_t> frame) noexcept {
    earliest = std::min(earliest, ready);
    // The frame's number, unwrapped: the one after the last, or as far after it as the stream says.
    // A jump back or of more than a few seconds (a new stream after a reconnect) starts the measuring again.
    std::int64_t step = 1;
    if (frame && last_frame_) {
        const auto ahead = static_cast<std::uint16_t>(*frame - *last_frame_);
        if (ahead == 0 || ahead > kRateMemory) {
            times_count_ = 0;
            times_at_ = 0;
            since_fit_ = 0;
        } else {
            step = ahead;
        }
    }
    last_frame_ = frame;
    number_ += step;
    numbers_[times_at_] = number_;
    times_[times_at_] = earliest;
    times_at_ = (times_at_ + 1) % kRateMemory;
    times_count_ = std::min(times_count_ + 1, kRateMemory);
    if (times_count_ >= kRateLeast && ++since_fit_ >= kRateEvery) {
        since_fit_ = 0;
        follow_clock();
    }
    Slot slot;
    slot.at = ready;
    // Frames that wait are stream that is not on the screen yet. A cushion is made of just that, so
    // only more of it than the cushion is long says the stream is behind; with none asked for, one
    // frame waiting does (half a frame's time is left for a frame that came a little early).
    slot.late = period_ * static_cast<std::int64_t>(std::min<std::size_t>(waiting, 1000)) > cushion_ + period_ / 2;
    if (last_at_ && !slot.late) {
        // A frame's time behind the picture before it: a little less while there is delay to give
        // back, a little more while the cushion asked for is not there yet.
        Duration spacing = period_;
        Duration taken_up{0};
        if (const Duration least = spare(); least > cushion_) {
            spacing -= std::min(least - cushion_, kGiveBack);
        } else if (cushion_ > Duration{0}) {
            if (const auto usual = usual_least(); usual && *usual < cushion_) {
                taken_up = std::min(cushion_ - *usual, kTakeUp);
                spacing += taken_up;
            }
        }
        const TimePoint place = *last_at_ + spacing;
        if (place > earliest) {
            const auto wait = std::chrono::duration_cast<Duration>(place - earliest);
            // Further behind than a picture is ever held: frames have piled up. It is shown at once,
            // and the pictures behind it take their distance from it.
            slot.late = wait > cushion_ + kMostWait;
            if (!slot.late && place > ready) {
                slot.at = place;
                // Every picture remembered would have waited that much longer at the delay there is now:
                // without this the cushion would be taken up again and again for the same pictures.
                for (std::size_t i = 0; i < waits_count_; ++i) waits_[i] += taken_up;
            }
        }
    }
    // A picture of a stream that is behind says nothing of how long pictures wait when it is not.
    if (!slot.late) {
        waits_[waits_at_] = std::chrono::duration_cast<Duration>(slot.at - earliest);
        waits_at_ = (waits_at_ + 1) % kMemory;
        waits_count_ = std::min(waits_count_ + 1, kMemory);
    }
    before_ = last_at_;
    last_at_ = slot.at;
    return slot;
}

void FramePacer::skipped() noexcept {
    // The picture before it is still the one on the screen.
    if (before_) last_at_ = before_;
}

}  // namespace psvp::media
