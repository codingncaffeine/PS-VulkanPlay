#include <cmath>
#include <cstdint>
#include <format>
#include <string>

#include "check.h"
#include "media/test_clip.h"
#include "timing.h"

using namespace psvp::lab;

namespace {
constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kStart = 5'000 * kMs;
// Frame n leaves at sixty a second.
std::int64_t sent_at(int n) { return kStart + static_cast<std::int64_t>(n) * 1000 * kMs / 60; }
bool near(double a, double b) { return std::abs(a - b) < 0.01; }

// 240 frames; each on the screen 12 ms after it left, but frame 150, which a newer one replaced
// first, and frames 200 and 201, held up on the way: 40 and 24 ms, the one before them stood. One
// press, read by the console 3 ms after the client made it, between frames 99 and 100.
std::string session() {
    std::string text;
    const std::int64_t press = sent_at(99) + 5 * kMs, read = press + 3 * kMs;
    text += std::format("2026-11-21T14:00:00.000Z DEBUG pretend: pressed south at {} ns\n", press);
    text += std::format("press cross {}\n", read);
    for (int n = 1; n <= 240; ++n) {
        const bool mark = n >= 100;
        text += std::format("frame {} {} {}\n", n, sent_at(n), mark ? 1 : 0);
        if (n == 150) continue;
        const std::int64_t shown = sent_at(n) + (n == 200 ? 40 : n == 201 ? 24 : 12) * kMs;
        text += std::format("seen {} {} {} 0\n", n, shown, mark ? 1 : 0);
    }
    return text;
}
}  // namespace

PSVP_TEST(screen_clock_scores_the_time_from_sent_to_seen) {
    Timing timing;
    read_lines(session(), timing);
    CHECK(timing.sent.size() == 240 && timing.seen.size() == 239 && timing.unread == 0);
    const Score s = score(timing);
    CHECK(s.problems.empty());
    // The first second after the first picture was seen is not scored: frames 62-240 were sent after it.
    CHECK(s.sent == 179);
    CHECK(s.reached == 178 && s.never == 1);
    CHECK(near(s.latency.median_ms, 12.0) && near(s.latency.most_ms, 40.0) && near(s.latency.least_ms, 12.0));
    // Frame 149 stood two frames' time (33.3 ms), frame 199 16.7 + 28 = 44.7 ms; from the picture seen
    // at the end of the first second (frame 61) on, every change is a gap.
    CHECK(s.stood == 2);
    CHECK(s.gaps == 178);
}

PSVP_TEST(screen_clock_takes_a_press_apart) {
    Timing timing;
    read_lines(session(), timing);
    const Score s = score(timing);
    CHECK(s.press_to_picture.count == 1 && s.presses_unanswered == 0);
    CHECK(near(s.press_to_console.median_ms, 3.0));
    // Read 8 ms after frame 99 left; frame 100 leaves 16.667 ms after 99.
    CHECK(near(s.console_wait.median_ms, 1000.0 / 60 - 8.0));
    CHECK(near(s.frame_to_screen.median_ms, 12.0));
    CHECK(near(s.press_to_picture.median_ms, 1000.0 / 60 - 5.0 + 12.0));
    const std::string text = describe(s);
    CHECK(text.find("Lab: 1 presses, press to picture: median 23.7 ms") != std::string::npos);
}

PSVP_TEST(screen_clock_says_when_a_score_cannot_be_trusted) {
    Timing timing;
    // A press the client made that the console never read, and a frame shown that was never sent.
    read_lines(session() + "seen 999 99999999999 1 0\n" + std::format("x DEBUG pretend: pressed east at {} ns\n", sent_at(10)) + "frame what\n", timing);
    const Score s = score(timing);
    CHECK(timing.unread == 1);
    CHECK(s.problems.size() == 3);
    CHECK(describe(s).find("PROBLEM: the screen showed frame 999, which was never sent.") != std::string::npos);
    Timing empty;
    CHECK(score(empty).problems.size() == 1 && score(empty).reached == 0);
}

PSVP_TEST(spread_takes_the_nearest_rank) {
    std::vector<double> values;
    for (int i = 100; i >= 1; --i) values.push_back(i);
    const Spread s = spread(values);
    CHECK(s.count == 100 && near(s.median_ms, 50.0) && near(s.p99_ms, 99.0) && near(s.most_ms, 100.0) && near(s.least_ms, 1.0));
    CHECK(spread({}).count == 0);
}

PSVP_TEST(clip_mark_is_the_last_block_of_the_patch_row) {
    using psvp::media::clip_block;
    CHECK(clip_block(5, 25, 2, 26, 15, true).y == 235 && clip_block(5, 25, 2, 26, 15, false).y == 64);
    // Nothing else changes with it.
    for (unsigned row = 0; row < 15; ++row) {
        for (unsigned column = 0; column < 26; ++column) {
            if (row == psvp::media::kClipMarkRow && column == 25) continue;
            CHECK(clip_block(5, column, row, 26, 15, true) == clip_block(5, column, row, 26, 15, false));
        }
    }
}

PSVP_TEST(screen_clock_counts_damaged_pictures_and_their_time) {
    Timing timing;
    std::string text = session();
    // After the first second: frame 151 damaged for 10 ms, then again for 6 ms; frame 152 too, right behind.
    text += std::format("seen 151 {} 1 1\n", sent_at(151) + 13 * kMs);
    text += std::format("seen 151 {} 1 0\n", sent_at(151) + 23 * kMs);
    read_lines(text, timing);
    const Score s = score(timing);
    CHECK(s.damaged == 1);
    CHECK(near(s.damaged_ms, 10.0));
    CHECK(describe(s).find("Lab: Damaged pictures (not the frame their number says) came on the screen 1 times, up for 10.0 ms in all.") != std::string::npos);
    Timing clean;
    read_lines(session(), clean);
    CHECK(score(clean).damaged == 0);
}
