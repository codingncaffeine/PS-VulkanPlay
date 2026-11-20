#include <chrono>
#include <cstdint>
#include <vector>

#include "check.h"
#include "media/frame_pacer.h"

using namespace psvp::media;
using namespace std::chrono_literals;

namespace {
const FramePacer::TimePoint kStart{};
// The time of tick `n` of sixty a second.
FramePacer::TimePoint tick(std::int64_t n) { return kStart + std::chrono::microseconds(n * 1000000 / 60); }
double ms(std::chrono::steady_clock::duration time) { return std::chrono::duration<double, std::milli>(time).count(); }
}  // namespace

PSVP_TEST(pacer_adds_nothing_to_a_steady_stream) {
    FramePacer pacer(60.0);
    double most = 0.0;
    for (int n = 0; n < 600; ++n) {
        const auto slot = pacer.schedule(tick(n));
        CHECK(!slot.late);
        most = std::max(most, ms(slot.at - tick(n)));
    }
    CHECK(most < 0.05);
    // Nor to a stream of thirty a second, nor to one that leaves ticks out: pictures that come
    // further apart than a frame's time are shown as they come.
    FramePacer slow(60.0);
    std::int64_t n = 0;
    for (int i = 0; i < 300; ++i) {
        const auto slot = slow.schedule(tick(n));
        CHECK(!slot.late && slot.at == tick(n));
        n += i % 5 == 0 ? 2 : 1;
    }
    FramePacer half(60.0);
    for (int i = 0; i < 100; ++i) CHECK(half.schedule(tick(2 * i)).at == tick(2 * i));
}

// Frames that arrive two at a time: every other one is held up by 12 ms and comes right before
// the next. Shown as they come they would be 4.7 and 28.7 ms apart; paced, a frame's time.
PSVP_TEST(pacer_evens_out_frames_that_come_in_pairs) {
    FramePacer pacer(60.0);
    std::vector<FramePacer::TimePoint> shown;
    double waited = 0.0;
    for (int n = 0; n < 600; ++n) {
        const auto ready = tick(n) + (n % 2 == 0 ? 12ms : 0ms);
        const auto slot = pacer.schedule(ready);
        CHECK(!slot.late && slot.at >= ready);
        shown.push_back(slot.at);
        if (n >= 2) waited += ms(slot.at - ready);
    }
    double least = 1e9, most = 0.0;
    for (std::size_t i = 2; i < shown.size(); ++i) {
        least = std::min(least, ms(shown[i] - shown[i - 1]));
        most = std::max(most, ms(shown[i] - shown[i - 1]));
    }
    CHECK(least > 16.5 && most < 16.9);
    // The price: the picture that came right behind a late one waits 12 ms, the late one not at all.
    CHECK(waited / 598.0 > 5.9 && waited / 598.0 < 6.1);
    CHECK(pacer.spare() < 100us);
}

PSVP_TEST(pacer_does_not_play_a_pause_back) {
    FramePacer pacer(60.0);
    for (int n = 0; n < 120; ++n) (void)pacer.schedule(tick(n));
    // Nothing arrives for 100 ms; then the six frames of that time come at once.
    const auto burst = tick(119) + 100ms;
    const auto first = pacer.schedule(burst);
    CHECK(!first.late && first.at == burst);
    const auto second = pacer.schedule(burst);
    CHECK(!second.late && ms(second.at - burst) > 16.6 && ms(second.at - burst) < 16.7);
    // The third would have to wait two frames' time: more than the most. With newer ones there it is left out.
    for (int i = 0; i < 3; ++i) {
        const auto slot = pacer.schedule(burst);
        CHECK(slot.late);
        pacer.skipped();
    }
    // The last of the bunch has none behind it: it is shown at once.
    const auto last = pacer.schedule(burst);
    CHECK(last.late && last.at == burst);
    // The stream goes on evenly, and on time: the pictures of the pause were not waited behind, so none of it stays as delay.
    std::int64_t n = 126;
    double end_wait = 99.0;
    for (int i = 0; i < 120; ++i, ++n) {
        const auto ready = tick(n) + 2ms;  // the tick the stream is at, 2 ms after the bunch's time
        const auto slot = pacer.schedule(ready);
        CHECK(!slot.late);
        end_wait = ms(slot.at - ready);
    }
    CHECK(end_wait < 0.1);
}

PSVP_TEST(pacer_keeps_what_uneven_frames_need_and_gives_back_the_rest) {
    FramePacer pacer(60.0);
    std::int64_t n = 0;
    for (; n < 60; ++n) (void)pacer.schedule(tick(n));
    // One frame 10 ms late: it is shown as it comes, and the pictures behind it are a frame's time apart, 10 ms later than they came.
    const auto late = pacer.schedule(tick(n) + 10ms);
    CHECK(late.at == tick(n) + 10ms && !late.late);
    ++n;
    const auto next = pacer.schedule(tick(n));
    CHECK(ms(next.at - tick(n)) > 9.9 && ms(next.at - tick(n)) < 10.1);
    ++n;
    // While not every picture of the last second waited, nothing is given back; then it is, and a second later it is gone.
    CHECK(pacer.spare() == 0us);
    double wait = 0.0;
    for (int i = 0; i < 59; ++i, ++n) wait = ms(pacer.schedule(tick(n)).at - tick(n));
    CHECK(wait > 9.0);
    for (int i = 0; i < 60; ++i, ++n) wait = ms(pacer.schedule(tick(n)).at - tick(n));
    CHECK(wait < 0.1);
    // A frame that late every half second keeps its allowance: the pictures stay even.
    FramePacer uneven(60.0);
    std::vector<FramePacer::TimePoint> shown;
    for (int i = 0; i < 600; ++i) shown.push_back(uneven.schedule(tick(i) + (i % 30 == 0 ? 10ms : 0ms)).at);
    double most = 0.0;
    for (std::size_t i = 120; i < shown.size(); ++i) most = std::max(most, ms(shown[i] - shown[i - 1]));
    CHECK(most < 16.9);
}
