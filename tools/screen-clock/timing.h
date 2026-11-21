// Scores what a reader of the screen saw against what the console stand-in sent: how long a
// frame took from leaving the console to being on the screen, how evenly pictures followed one
// another there, which frames never got there, and how long a button press took to show.
// No I/O and no clock in here: the lines come in as text, the times as nanoseconds on the
// steady clock that every process of a check reads alike.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace psvp::lab {

// A picture frame as the stand-in sent it ("frame INDEX NS MARK").
struct Sent {
    std::uint16_t index = 0;
    std::int64_t ns = 0;
    bool mark = false;
};
// What the screen showed from a moment on ("seen COUNTER NS MARK"): a line per change.
struct Seen {
    std::uint16_t counter = 0;
    std::int64_t ns = 0;
    bool mark = false;
};

struct Timing {
    std::vector<Sent> sent;
    std::vector<std::int64_t> console_presses;  // "press BUTTON NS": when the stand-in read each press
    std::vector<std::int64_t> client_presses;   // "pretend: pressed NAME at NS ns": when the client pressed
    std::vector<Seen> seen;
    std::size_t unread = 0;                     // lines that were none of these
};

// Reads the stand-in's timing log, the reader's own lines and the client's log into `into`.
// Any of them can come in any order; lines that belong to none are counted, not fatal, for a
// client log has many other lines (those count only when they look like ours and are not).
void read_lines(std::string_view text, Timing& into);

struct Spread {
    std::size_t count = 0;
    double median_ms = 0, p99_ms = 0, most_ms = 0, least_ms = 0;
};
[[nodiscard]] Spread spread(std::vector<double> values_ms);

struct Score {
    std::size_t sent = 0;          // frames sent from the first one seen to the last one seen
    std::size_t reached = 0;       // of them, frames the screen showed
    std::size_t never = 0;         // and frames it never showed
    Spread latency;                // sent to on the screen
    std::size_t gaps = 0;          // times between one picture and the next on the screen
    std::size_t even = 0;          // of them, 13 to 21 ms
    std::size_t stood = 0;         // over 30 ms
    // A press to its picture, and its parts: client to console, the console's wait for its next
    // frame, that frame to the screen. Presses are paired in their order: a check presses seldom.
    Spread press_to_picture, press_to_console, console_wait, frame_to_screen;
    std::size_t presses_unanswered = 0;  // presses the screen never showed an answer to
    std::vector<std::string> problems;   // what makes the score untrustworthy, if anything
};
[[nodiscard]] Score score(const Timing& timing);

// The score as lines that start with "Lab:".
[[nodiscard]] std::string describe(const Score& score);

}  // namespace psvp::lab
