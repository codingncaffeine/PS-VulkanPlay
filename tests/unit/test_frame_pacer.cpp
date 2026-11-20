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

// A player as it is: one decoder, which takes 3.5 ms a frame and gets to a frame only when the
// picture before it has gone to the screen. Frames arrive in their order.
struct Shown {
    FramePacer::TimePoint arrived, at;
    bool late = false;
    [[nodiscard]] double delay() const { return ms(at - arrived); }
};
constexpr auto kDecode = 3500us;
std::vector<Shown> play(FramePacer& pacer, const std::vector<FramePacer::TimePoint>& arrivals) {
    std::vector<Shown> shown;
    FramePacer::TimePoint free = arrivals.front();
    for (std::size_t n = 0; n < arrivals.size(); ++n) {
        const auto ready = std::max(arrivals[n], free) + kDecode;
        std::size_t waiting = 0;
        for (std::size_t m = n + 1; m < arrivals.size() && arrivals[m] <= ready; ++m) ++waiting;
        const auto slot = pacer.schedule(arrivals[n] + kDecode, ready, waiting);
        CHECK(slot.at >= ready);
        shown.push_back({arrivals[n], slot.at, slot.late});
        free = slot.at;
    }
    return shown;
}
// Frames at sixty a second; `late(n)` is how much later than its tick frame n arrives. A frame
// never arrives before the one before it.
template <typename Late>
std::vector<FramePacer::TimePoint> arrivals(std::size_t count, Late late) {
    std::vector<FramePacer::TimePoint> out;
    for (std::size_t n = 0; n < count; ++n) {
        auto at = tick(static_cast<std::int64_t>(n)) + late(n);
        if (!out.empty()) at = std::max(at, out.back());
        out.push_back(at);
    }
    return out;
}
double most_apart(const std::vector<Shown>& shown, std::size_t from, std::size_t to) {
    double most = 0.0;
    for (std::size_t i = from; i < to; ++i) most = std::max(most, ms(shown[i].at - shown[i - 1].at));
    return most;
}
double least_apart(const std::vector<Shown>& shown, std::size_t from, std::size_t to) {
    double least = 1e9;
    for (std::size_t i = from; i < to; ++i) least = std::min(least, ms(shown[i].at - shown[i - 1].at));
    return least;
}
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

// What a player sees behind a pause: the frames of the pause are there before the first of them is
// decoded. Each picture with a newer frame behind it is shown at once; only the last takes its
// distance, and that is all the delay there is.
PSVP_TEST(pacer_does_not_wait_while_newer_frames_are_there) {
    FramePacer pacer(60.0);
    for (int n = 0; n < 120; ++n) (void)pacer.schedule(tick(n));
    const auto burst = tick(119) + 100ms;
    // Six frames, decoded one after the other, 4 ms each; five of them with newer ones behind.
    for (int i = 0; i < 5; ++i) {
        const auto ready = burst + i * 4ms;
        const auto slot = pacer.schedule(ready, 1);
        CHECK(slot.late && slot.at == ready);
    }
    const auto last_ready = burst + 20ms;
    const auto last = pacer.schedule(last_ready, 0);
    CHECK(!last.late && ms(last.at - last_ready) > 12.6 && ms(last.at - last_ready) < 12.7);  // a frame's time behind the fifth
    // The next frame comes at its own time, 15 ms behind the last one of the bunch: it waits less than a frame's
    // time, and the hundred milliseconds of the pause are nowhere.
    const auto next = pacer.schedule(tick(127) + 2ms);
    CHECK(!next.late && ms(next.at - (tick(127) + 2ms)) < 16.7);
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

// The same pairs through a player, whose decoder gets to the second frame of a pair only when
// it is done with the first: evenly shown, and nothing kept that the pairs do not need.
PSVP_TEST(pacer_evens_out_pairs_in_a_player) {
    FramePacer pacer(60.0);
    const auto shown = play(pacer, arrivals(600, [](std::size_t n) { return n % 2 == 0 ? 12ms : 0ms; }));
    CHECK(least_apart(shown, 4, shown.size()) > 16.5 && most_apart(shown, 4, shown.size()) < 16.9);
    double delay = 0.0;
    for (std::size_t i = 4; i < shown.size(); ++i) {
        CHECK(!shown[i].late);
        delay += shown[i].delay();
    }
    // The late one of a pair goes to the screen when it is decoded, the other one 12 ms after it is.
    CHECK(delay / 596.0 > 9.4 && delay / 596.0 < 9.6);
}

PSVP_TEST(pacer_keeps_the_cushion_asked_for) {
    // 20 ms is less than a frame's time; with 30 and 50 the next frames wait to be decoded while a
    // picture waits for its time, which is what a cushion that long is made of.
    for (const auto cushion : {20ms, 30ms, 50ms}) {
        FramePacer pacer(60.0, cushion);
        CHECK(pacer.cushion() == cushion);
        const auto shown = play(pacer, arrivals(600, [](std::size_t) { return 0ms; }));
        const double wanted = ms(cushion) + ms(kDecode);
        double most = 0.0;
        for (const Shown& one : shown) {
            CHECK(!one.late);
            most = std::max(most, one.delay());
        }
        // It is taken up over the first second, to the millisecond and no further.
        CHECK(most < wanted + 0.1);
        for (std::size_t i = 120; i < shown.size(); ++i) CHECK(shown[i].delay() > wanted - 0.1);
        CHECK(most_apart(shown, 1, shown.size()) < 18.3);    // while it is taken up: 1.5 ms a picture
        CHECK(most_apart(shown, 120, shown.size()) < 16.7 && least_apart(shown, 1, shown.size()) > 16.6);
    }
    // No cushion is longer than three frames' time, and none is less than nothing.
    CHECK(FramePacer(60.0, 80ms).cushion() == 50ms);
    CHECK(FramePacer(60.0, -5ms).cushion() == 0ms);
    CHECK(FramePacer(60.0).cushion() == 0ms);
}

PSVP_TEST(a_cushion_hides_a_frame_that_comes_late) {
    const auto with_one_late = [](std::chrono::milliseconds by) {
        return arrivals(600, [by](std::size_t n) { return n == 200 ? by : 0ms; });
    };
    // 15 ms late with a cushion of 20: it is shown in its place, and nobody sees that it was late.
    FramePacer cushioned(60.0, 20ms);
    auto shown = play(cushioned, with_one_late(15ms));
    CHECK(most_apart(shown, 120, shown.size()) < 16.7 && least_apart(shown, 120, shown.size()) > 16.6);
    CHECK(shown[200].delay() > 8.4 && shown[200].delay() < 8.6);   // 15 of its 20 are used up
    CHECK(shown[599].delay() > 23.4 && shown[599].delay() < 23.6);  // and the one frame changes nothing after it
    // The same stream with none: the picture before it stands for a frame's time and those 15 ms.
    FramePacer bare(60.0);
    shown = play(bare, with_one_late(15ms));
    CHECK(ms(shown[200].at - shown[199].at) > 31.6 && ms(shown[200].at - shown[199].at) < 31.7);
    // 30 ms late is more than the cushion: the picture stands for the 10 ms that are missing, and no
    // longer. The pictures behind it follow evenly, 10 ms later than before, and that is given back.
    FramePacer short_by_ten(60.0, 20ms);
    shown = play(short_by_ten, with_one_late(30ms));
    CHECK(ms(shown[200].at - shown[199].at) > 26.6 && ms(shown[200].at - shown[199].at) < 26.7);
    CHECK(most_apart(shown, 201, shown.size()) < 16.7);
    CHECK(shown[230].delay() > 33.4 && shown[230].delay() < 33.6);
    CHECK(shown[599].delay() > 23.4 && shown[599].delay() < 23.6);
    // Only the late one itself may count as behind (two frames wait behind it by the time it is decoded).
    for (std::size_t i = 0; i < shown.size(); ++i) CHECK(!shown[i].late || i == 200);
}

// Every three and a half seconds nothing arrives for 145 ms, and the nine frames of that time come
// at once: a wireless network's pauses. The cushion stays what it is: the frames behind a pause
// say nothing of how the others come, and it is not taken up again for them.
PSVP_TEST(a_cushion_does_not_grow_behind_pauses) {
    FramePacer pacer(60.0, 20ms);
    const auto shown = play(pacer, arrivals(2400, [](std::size_t n) {
        const std::size_t in_pause = n % 210;
        if (n < 210 || in_pause >= 9) return std::chrono::microseconds(0);
        return std::chrono::microseconds(145000) - std::chrono::microseconds(static_cast<std::int64_t>(in_pause) * 1000000 / 60);
    }));
    std::size_t late = 0;
    for (std::size_t n = 210; n < shown.size(); ++n) {
        const std::size_t since_pause = n % 210;
        if (shown[n].late) ++late;
        if (since_pause < 9) continue;
        // The frames that come at their own time are never further behind than the cushion and what
        // a picture is held at most, they follow one another by a frame's time and no more, and
        // two seconds after a pause the delay is the cushion again.
        CHECK(shown[n].delay() < 20.0 + 25.0 + 3.5 + 0.1);
        CHECK(ms(shown[n].at - shown[n - 1].at) < 16.7);
        if (since_pause >= 120) CHECK(shown[n].delay() > 23.4 && shown[n].delay() < 23.6);
    }
    // The frames of a pause have more behind them than the cushion is long: they are shown at once.
    CHECK(late >= 8 * 11 && late <= 10 * 11);
}

PSVP_TEST(pacer_counts_waiting_frames_against_the_cushion) {
    const auto late_with = [](std::chrono::milliseconds cushion, std::size_t waiting) {
        FramePacer pacer(60.0, cushion);
        for (int n = 0; n < 30; ++n) (void)pacer.schedule(tick(n));
        return pacer.schedule(tick(30), waiting).late;
    };
    CHECK(!late_with(0ms, 0) && late_with(0ms, 1));
    CHECK(!late_with(5ms, 0) && late_with(5ms, 1));
    CHECK(!late_with(20ms, 1) && late_with(20ms, 2));
    CHECK(!late_with(30ms, 2) && late_with(30ms, 3));
    CHECK(!late_with(50ms, 3) && late_with(50ms, 4));
}
