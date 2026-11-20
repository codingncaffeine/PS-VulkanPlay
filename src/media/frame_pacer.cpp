#include "media/frame_pacer.h"

#include <algorithm>

namespace psvp::media {

FramePacer::FramePacer(double frames_per_second, Duration cushion) noexcept
    : period_(static_cast<std::int64_t>(1e6 / (frames_per_second > 0.0 ? frames_per_second : 60.0))),
      cushion_(std::clamp(cushion, Duration{0}, kMostCushion)) {}

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

FramePacer::Slot FramePacer::schedule(TimePoint earliest, TimePoint ready, std::size_t waiting) noexcept {
    earliest = std::min(earliest, ready);
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
