#include "media/sound_buffer.h"

#include <algorithm>
#include <utility>

namespace psvp::media {

void SoundBuffer::begin_period(std::size_t frames) noexcept {
    frames = std::max<std::size_t>(frames, 1);
    if (frames >= period_ || ++periods_since_ > kPeriodMemory) {
        period_ = frames;
        periods_since_ = 0;
    }
    if (!playing_) return;
    ++counters_.periods;
    counters_.depth_sum += line_.size();
    if (counters_.periods == 1) counters_.least_depth = counters_.most_depth = line_.size();
    counters_.least_depth = std::min(counters_.least_depth, line_.size());
    counters_.most_depth = std::max(counters_.most_depth, line_.size());
    counters_.largest_period = std::max(counters_.largest_period, frames);
    // Frames come two at a time and the device takes a piece at a time: the line is judged by
    // its mean length over a few pieces, not by where one of them finds it.
    smoothed_depth_ += (static_cast<double>(line_.size()) - smoothed_depth_) / 16.0;
}

void SoundBuffer::push(std::vector<std::uint8_t> frame, std::uint32_t lost_before, TimePoint now) {
    ++counters_.arrived;
    if (!playing_ && line_.empty()) last_change_ = now;
    // A frame that was lost keeps its place in the line: the sound around it stays where it belongs.
    for (std::uint32_t i = 0; i < std::min(lost_before, kMostFilled); ++i) line_.push_back(Entry{true, {}});
    line_.push_back(Entry{false, std::move(frame)});
    if (!playing_ && line_.size() >= aim()) playing_ = true;

    // Frames that come in a bunch after a pause: while they did not come, the device played on
    // (what the decoder made up, then silence). The oldest of the bunch belong to that time.
    if (line_.size() > aim() + kSurplus) {
        while (line_.size() > aim() + 1) {
            line_.pop_front();
            ++counters_.let_go;
        }
    }
    if (now - last_change_ >= kCalm && aim_ > kLeastDepth) {
        --aim_;
        last_change_ = now;
    }
}

SoundBuffer::Next SoundBuffer::pull(TimePoint now) {
    Next next;
    // Before the first frames are in line, and before any came: silence that counts for nothing.
    if (!playing_) return next;
    ++counters_.takes;
    if (!line_.empty()) {
        Entry entry = std::move(line_.front());
        line_.pop_front();
        empty_ = false;
        if (entry.lost) {
            next.kind = Kind::concealed;
            ++counters_.filled;
            return next;
        }
        concealed_run_ = 0;
        next.kind = Kind::frame;
        next.frame = std::move(entry.frame);
        ++counters_.played;
        return next;
    }
    // Nothing in line. Frames come less evenly than the line allowed for: it is aimed a frame longer.
    if (!empty_) {
        empty_ = true;
        ++counters_.ran_empty;
        aim_ = std::min(aim_ + 1, kMostDepth);
        last_change_ = now;
    }
    if (concealed_run_ < kMostConcealed) {
        ++concealed_run_;
        ++counters_.concealed;
        next.kind = Kind::concealed;
        return next;
    }
    ++counters_.silent;
    return next;
}

double SoundBuffer::speed() const noexcept {
    const double off = smoothed_depth_ - static_cast<double>(aim());
    if (off > kBand) return 1.0 + kMostChange * std::min(1.0, (off - kBand) / kFull);
    if (off < -kBand) return 1.0 - kMostChange * std::min(1.0, (-off - kBand) / kFull);
    return 1.0;
}

}  // namespace psvp::media
