#include "media/present_clock.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace psvp::media {

namespace {

// At most this many presents wait for their report: a driver that never reports does not grow it.
constexpr std::size_t kMostWaiting = 256;
constexpr std::uint64_t kHalfMillisecondNs = 500000;
constexpr std::uint64_t kEvenWithinNs = 1500000;

}  // namespace

bool PresentClock::divides(double pictures_per_second, std::uint64_t refresh_ns) noexcept {
    if (pictures_per_second <= 0.0 || refresh_ns == 0) return true;
    const double ratio = 1e9 / static_cast<double>(refresh_ns) / pictures_per_second;
    return ratio >= 0.995 && std::abs(ratio - std::round(ratio)) <= 0.005 * ratio;
}

void PresentClock::handed(std::uint64_t present_id, std::uint64_t at_ns) {
    ++handed_count_;
    waiting_.push_back({present_id, at_ns});
    if (waiting_.size() > kMostWaiting) waiting_.pop_front();
}

void PresentClock::reported(std::uint64_t present_id, std::uint64_t first_pixel_ns) {
    if (first_pixel_ns == 0) return;
    // The present it reports, and every older one still waiting: those were not reported.
    const auto it = std::ranges::find_if(waiting_, [&](const Handed& h) { return h.id == present_id; });
    if (it == waiting_.end()) return;
    const Handed handed = *it;
    waiting_.erase(waiting_.begin(), it + 1);
    ++reported_count_;
    if (steady_domain_ && first_pixel_ns >= handed.at_ns) {
        const std::uint64_t step = std::min<std::uint64_t>((first_pixel_ns - handed.at_ns) / 1000 / kLatencyStepUs, latency_steps_.size() - 1);
        ++latency_steps_[step];
        ++latency_count_;
    }
    // How long the picture before it stayed: only between presents one after the other.
    if (last_id_ != 0 && present_id == last_id_ + 1 && first_pixel_ns > last_pixel_ns_) {
        const std::uint64_t stayed = first_pixel_ns - last_pixel_ns_;
        longest_ns_ = std::max(longest_ns_, stayed);
        ++apart_;
        if (stream_rate_ > 0.0) {
            const auto period = static_cast<std::uint64_t>(1e9 / stream_rate_);
            if ((stayed > period ? stayed - period : period - stayed) <= kEvenWithinNs) ++even_;
        }
        if (refresh_ns_ != 0) {
            const std::uint64_t whole = std::max<std::uint64_t>(1, (stayed + refresh_ns_ / 2) / refresh_ns_);
            ++refreshes_[std::min<std::uint64_t>(whole, kBuckets) - 1];
            const std::uint64_t at = whole * refresh_ns_;
            if ((stayed > at ? stayed - at : at - stayed) <= kHalfMillisecondNs) ++on_refresh_;
        }
    }
    last_id_ = present_id;
    last_pixel_ns_ = first_pixel_ns;
}

PresentClock::Summary PresentClock::summary() const {
    Summary s;
    s.handed = handed_count_;
    s.reported = reported_count_;
    s.refreshes = refreshes_;
    s.on_refresh = on_refresh_;
    s.apart = apart_;
    s.longest_ns = longest_ns_;
    s.even = even_;
    s.latency_count = latency_count_;
    if (latency_count_ != 0) {
        const auto at_share = [&](std::uint64_t numerator, std::uint64_t denominator) {
            const std::uint64_t wanted = std::max<std::uint64_t>(1, (latency_count_ * numerator + denominator - 1) / denominator);
            std::uint64_t seen = 0;
            for (std::size_t i = 0; i < latency_steps_.size(); ++i) {
                seen += latency_steps_[i];
                if (seen >= wanted) return static_cast<std::uint64_t>(i) * kLatencyStepUs;
            }
            return static_cast<std::uint64_t>(latency_steps_.size() - 1) * kLatencyStepUs;
        };
        s.latency_median_us = at_share(1, 2);
        s.latency_99_us = at_share(99, 100);
    }
    return s;
}

std::string PresentClock::text() const {
    const Summary s = summary();
    if (s.reported == 0) return {};
    std::string line = std::format("On the screen (the driver's own times): {} of {} pictures reported", s.reported, s.handed);
    if (adaptive() && s.apart != 0) {
        line += std::format("; the refresh followed the pictures (adaptive sync, {:.1f} Hz at most); the longest a picture stayed {:.1f} ms",
                            refresh_ns_ != 0 ? 1e9 / static_cast<double>(refresh_ns_) : 0.0, static_cast<double>(s.longest_ns) / 1e6);
    } else if (refresh_ns_ != 0 && s.apart != 0) {
        line += std::format("; at {:.1f} Hz each stayed 1 refresh: {}, 2: {}, 3: {}, 4: {}, 5 or more: {} (the longest {:.1f} ms); {:.1f}% on a whole refresh",
                            1e9 / static_cast<double>(refresh_ns_), s.refreshes[0], s.refreshes[1], s.refreshes[2], s.refreshes[3], s.refreshes[4],
                            static_cast<double>(s.longest_ns) / 1e6, 100.0 * static_cast<double>(s.on_refresh) / static_cast<double>(s.apart));
        if (s.on_refresh * 10 < s.apart * 9) {
            line += " (under 90%: the refresh is not fixed, or the times are not the screen's: read the counts with care)";
        } else if (!divides(stream_rate_, refresh_ns_)) {
            line += std::format("; {:.0f} pictures a second do not divide {:.1f} Hz: adaptive sync (VRR) for the window, or a refresh that is a multiple of {:.0f}, "
                                "shows each the same time",
                                stream_rate_, 1e9 / static_cast<double>(refresh_ns_), stream_rate_);
        }
    } else if (s.apart != 0) {
        line += std::format("; the display's refresh is not known (variable?): the longest a picture stayed {:.1f} ms", static_cast<double>(s.longest_ns) / 1e6);
    }
    if (stream_rate_ > 0.0 && s.apart != 0) {
        line += std::format("; {:.1f}% stayed one picture's time ({:.1f} ms, within 1.5 ms)", 100.0 * static_cast<double>(s.even) / static_cast<double>(s.apart),
                            1000.0 / stream_rate_);
    }
    if (s.latency_count != 0) {
        line += std::format("; from the hand-off to the first pixel out: {:.1f} ms in the middle, {:.1f} ms at the 99th percentile",
                            static_cast<double>(s.latency_median_us) / 1000.0, static_cast<double>(s.latency_99_us) / 1000.0);
    } else {
        line += "; the times are not on this program's clock: no delay from the hand-off";
    }
    return line;
}

}  // namespace psvp::media
