#include "media/frame_pacer.h"

#include <algorithm>

namespace psvp::media {

FramePacer::FramePacer(double frames_per_second) noexcept
    : period_(static_cast<std::int64_t>(1e6 / (frames_per_second > 0.0 ? frames_per_second : 60.0))) {}

FramePacer::Duration FramePacer::spare() const noexcept {
    if (waits_count_ < kMemory) return Duration{0};
    return *std::min_element(waits_.begin(), waits_.end());
}

FramePacer::Slot FramePacer::schedule(TimePoint ready, bool newer_waiting) noexcept {
    Slot slot;
    slot.at = ready;
    slot.late = newer_waiting;
    if (last_at_ && !newer_waiting) {
        // A frame's time behind the picture before it; a little less while there is delay to give back.
        const Duration spacing = period_ - std::min(spare(), kGiveBack);
        const TimePoint place = *last_at_ + spacing;
        if (place > ready) {
            const auto wait = std::chrono::duration_cast<Duration>(place - ready);
            // Further behind than a picture is ever held: frames have piled up. It is shown at once,
            // if at all, and the pictures behind it take their distance from it.
            slot.late = wait > kMostWait;
            if (!slot.late) slot.at = place;
        }
    }
    waits_[waits_at_] = std::chrono::duration_cast<Duration>(slot.at - ready);
    waits_at_ = (waits_at_ + 1) % kMemory;
    waits_count_ = std::min(waits_count_ + 1, kMemory);
    before_ = last_at_;
    last_at_ = slot.at;
    return slot;
}

void FramePacer::skipped() noexcept {
    // The picture before it is still the one on the screen.
    if (before_) last_at_ = before_;
}

}  // namespace psvp::media
