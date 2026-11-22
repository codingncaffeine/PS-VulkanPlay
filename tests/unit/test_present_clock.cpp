#include <cstdint>
#include <string>

#include "check.h"
#include "media/present_clock.h"

using namespace psvp::media;

namespace {
// 175 Hz: 5714286 ns a refresh.
constexpr std::uint64_t kRefresh = 5714286;
}  // namespace

// Sixty pictures a second on a 175 Hz display: each goes out on the first refresh after it is
// handed over, so they stay 3 refreshes, and every so often 2 (175/60 = 2.92 on average). The
// reported times wander up to 50 µs either side of the refresh, as a real clock's do.
PSVP_TEST(present_clock_counts_refreshes_per_picture) {
    PresentClock clock;
    clock.set_refresh(kRefresh);
    clock.set_steady_domain(true);
    std::uint64_t twos = 0, threes = 0, last_refresh = 0;
    for (std::uint64_t id = 1; id <= 600; ++id) {
        const std::uint64_t handed = id * 16666667;
        const std::uint64_t refresh = handed / kRefresh + 1;  // the next refresh
        clock.handed(id, handed);
        const auto wander = static_cast<std::int64_t>((id * 7919) % 101) - 50;
        clock.reported(id, static_cast<std::uint64_t>(static_cast<std::int64_t>(refresh * kRefresh) + wander * 1000));
        if (id > 1) (refresh - last_refresh == 2 ? twos : threes) += 1;
        last_refresh = refresh;
    }
    const PresentClock::Summary s = clock.summary();
    CHECK(s.handed == 600 && s.reported == 600);
    CHECK(s.apart == 599);
    CHECK(s.refreshes[1] == twos && s.refreshes[2] == threes);
    CHECK(twos > 30 && threes > 500);
    CHECK(s.on_refresh == 599);
    // From the hand-off to the next refresh: up to one refresh, about half of one in the middle.
    // A picture handed over within 50 µs of a refresh can be reported before its hand-off: not counted.
    CHECK(s.latency_count == 577);  // the 23 handed over within 50 µs of a refresh are not counted
    CHECK(s.latency_median_us >= 2000 && s.latency_median_us <= 3800);
    CHECK(s.latency_99_us <= 5800);
    const std::string text = clock.text();
    CHECK(text.find("175.0 Hz") != std::string::npos);
    CHECK(text.find("100.0% on a whole refresh") != std::string::npos);
    CHECK(text.find("with care") == std::string::npos);
}

// A fixed delay from the hand-off comes out as that delay.
PSVP_TEST(present_clock_measures_the_delay_to_the_first_pixel) {
    PresentClock clock;
    clock.set_refresh(kRefresh);
    clock.set_steady_domain(true);
    for (std::uint64_t id = 1; id <= 100; ++id) {
        clock.handed(id, id * 20000000);
        clock.reported(id, id * 20000000 + 5300000);
    }
    const PresentClock::Summary s = clock.summary();
    CHECK(s.latency_median_us == 5300 && s.latency_99_us == 5300);
}

// The control: times that are not on whole refreshes are said to be so.
PSVP_TEST(present_clock_says_when_times_are_off_the_refresh) {
    PresentClock clock;
    clock.set_refresh(kRefresh);
    std::uint64_t at = 1000000000;
    for (std::uint64_t id = 1; id <= 100; ++id) {
        clock.handed(id, 0);
        at += 15000000 + (id % 7) * 450000;  // 15 to 17.7 ms apart: seldom a whole number of refreshes
        clock.reported(id, at);
    }
    const PresentClock::Summary s = clock.summary();
    CHECK(s.on_refresh * 10 < s.apart * 9);
    CHECK(clock.text().find("with care") != std::string::npos);
    // Not on the steady clock: no delay is measured.
    CHECK(s.latency_count == 0);
    CHECK(clock.text().find("not on this program's clock") != std::string::npos);
}

// Reports that do not come: no interval across them, and the presents before a report that came
// are not waited for any more.
PSVP_TEST(present_clock_skips_presents_not_reported) {
    PresentClock clock;
    clock.set_refresh(kRefresh);
    for (std::uint64_t id = 1; id <= 10; ++id) clock.handed(id, id);
    clock.reported(1, 3 * kRefresh);
    clock.reported(2, 6 * kRefresh);   // 3 refreshes
    clock.reported(5, 15 * kRefresh);  // 3 and 4 never reported: no interval
    clock.reported(3, 9 * kRefresh);   // older than one reported: no longer waited for
    clock.reported(6, 17 * kRefresh);  // 2 refreshes
    clock.reported(6, 99 * kRefresh);  // twice: the second is not counted
    const PresentClock::Summary s = clock.summary();
    CHECK(s.reported == 4);
    CHECK(s.apart == 2);
    CHECK(s.refreshes[2] == 1 && s.refreshes[1] == 1);
    CHECK(PresentClock{}.text().empty());
}

// Sixty pictures a second at 175 Hz: the rate does not divide the refresh, and the report says
// what would show each picture the same time; 120 Hz and 59.94 Hz divide it.
PSVP_TEST(present_clock_says_when_the_rate_does_not_divide_the_refresh) {
    CHECK(!PresentClock::divides(60.0, kRefresh));
    CHECK(PresentClock::divides(60.0, 8333333));    // 120 Hz
    CHECK(PresentClock::divides(60.0, 16683350));   // 59.94 Hz
    CHECK(PresentClock::divides(30.0, 8333333));
    CHECK(!PresentClock::divides(60.0, 6944444));   // 144 Hz
    PresentClock clock;
    clock.set_refresh(kRefresh, kRefresh);
    clock.set_stream_rate(60.0);
    for (std::uint64_t id = 1; id <= 600; ++id) {
        const std::uint64_t handed = id * 16666667;
        clock.handed(id, handed);
        clock.reported(id, (handed / kRefresh + 1) * kRefresh);
    }
    const PresentClock::Summary s = clock.summary();
    // 3 refreshes (17.1 ms) is within 1.5 ms of a picture's time, 2 (11.4 ms) is not.
    CHECK(s.even == s.refreshes[2] && s.even + s.refreshes[1] == s.apart);
    const std::string text = clock.text();
    CHECK(text.find("60 pictures a second do not divide 175.0 Hz") != std::string::npos);
    CHECK(text.find("stayed one picture's time (16.7 ms") != std::string::npos);
    CHECK(text.find("adaptive sync, ") == std::string::npos);
}

// Adaptive sync: the swapchain's refresh interval is UINT64_MAX and each picture stays as long as
// the next one takes: no refresh counts, every picture one picture's time but the late one.
PSVP_TEST(present_clock_reads_adaptive_sync) {
    PresentClock clock;
    clock.set_refresh(kRefresh, UINT64_MAX);
    clock.set_stream_rate(60.0);
    CHECK(clock.adaptive());
    std::uint64_t at = 1000000;
    for (std::uint64_t id = 1; id <= 300; ++id) {
        at += id == 150 ? 33333333 : 16666667;
        clock.handed(id, at - 2000000);
        clock.reported(id, at);
    }
    const PresentClock::Summary s = clock.summary();
    CHECK(s.apart == 299 && s.even == 298);
    const std::string text = clock.text();
    CHECK(text.find("the refresh followed the pictures (adaptive sync, 175.0 Hz at most)") != std::string::npos);
    CHECK(text.find("the longest a picture stayed 33.3 ms") != std::string::npos);
    CHECK(text.find("on a whole refresh") == std::string::npos);
    CHECK(text.find("do not divide") == std::string::npos);
    CHECK(text.find("99.7% stayed one picture's time") != std::string::npos);
}
