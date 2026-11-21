// When pictures really reached the screen, from the driver's own report of every present
// (VK_EXT_present_timing, PLAN D37 step 2): our own "went to the screen" is the hand-off to the
// presentation engine, which a busy compositor can hold on to (seen in the lab, 2026-11-21).
//
// Per present the window gives its id and the moment it handed the picture over (the steady clock);
// later the driver says when the queue was done with it and when its first pixel went out to the
// display, in a time domain of the swapchain's. From those this counts how many of the display's
// refreshes each picture stayed (the cadence a viewer sees: at 60 pictures on a 175 Hz display
// mostly 3, every so often 2), and, where the domain is the steady clock itself, how long a picture
// took from the hand-off to its first pixel. No Vulkan in here: the window feeds it numbers.
//
// The control (D37): on a display of fixed refresh every first pixel goes out on a refresh, so the
// times between them are whole refreshes; a report where they are not says the numbers cannot be
// read that way (a variable refresh, or a driver's guess), and is shown as such.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>

namespace psvp::media {

class PresentClock {
public:
    // The display's refresh as the swapchain says it, in nanoseconds (0: unknown or variable), and
    // whether the reported times are on the steady clock (CLOCK_MONOTONIC).
    void set_refresh(std::uint64_t refresh_ns) noexcept { refresh_ns_ = refresh_ns; }
    void set_steady_domain(bool steady) noexcept { steady_domain_ = steady; }

    // A picture handed over: its present id and when (steady clock, ns).
    void handed(std::uint64_t present_id, std::uint64_t at_ns);
    // The driver's report of a present: its first pixel out (0: not reported).
    void reported(std::uint64_t present_id, std::uint64_t first_pixel_ns);

    // Refreshes a picture stayed: 1, 2, 3, 4, 5 or more (index 0 = 1 refresh).
    static constexpr std::size_t kBuckets = 5;
    struct Summary {
        std::uint64_t handed = 0, reported = 0;
        std::array<std::uint64_t, kBuckets> refreshes{};  // pictures that stayed that many refreshes
        std::uint64_t on_refresh = 0, apart = 0;           // intervals on a whole refresh (within 0.5 ms), of all
        std::uint64_t longest_ns = 0;                       // the longest a picture stayed
        // Hand-off to first pixel out, where the domain is the steady clock; in microseconds (to 100 µs).
        std::uint64_t latency_count = 0, latency_median_us = 0, latency_99_us = 0;
    };
    [[nodiscard]] Summary summary() const;
    // One line for the log and the report; empty when nothing was reported.
    [[nodiscard]] std::string text() const;

private:
    std::uint64_t refresh_ns_ = 0;
    bool steady_domain_ = false;
    // Presents handed and not reported yet (a report can come many presents later), oldest first.
    struct Handed {
        std::uint64_t id = 0, at_ns = 0;
    };
    std::deque<Handed> waiting_;
    std::uint64_t handed_count_ = 0, reported_count_ = 0;
    std::uint64_t last_id_ = 0, last_pixel_ns_ = 0;
    std::array<std::uint64_t, kBuckets> refreshes_{};
    std::uint64_t on_refresh_ = 0, apart_ = 0, longest_ns_ = 0;
    // Latencies counted in steps of 100 µs up to 100 ms (more goes in the last).
    static constexpr std::uint64_t kLatencyStepUs = 100;
    std::array<std::uint32_t, 1001> latency_steps_{};
    std::uint64_t latency_count_ = 0;
};

}  // namespace psvp::media
