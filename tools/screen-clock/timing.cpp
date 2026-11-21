#include "timing.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <map>

namespace psvp::lab {

namespace {

// Frames sent in the first second after the first picture was seen are not scored: the window,
// the decoder's start-up check and the first keyframe all happen then, once per session.
constexpr std::int64_t kSettleNs = 1'000'000'000;

std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && line[at] == ' ') ++at;
        const std::size_t end = std::min(line.find(' ', at), line.size());
        if (end > at) out.push_back(line.substr(at, end - at));
        at = end;
    }
    return out;
}

template <class Number>
bool number(std::string_view text, Number& into) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), into);
    return error == std::errc{} && end == text.data() + text.size();
}

double ms(std::int64_t ns) { return static_cast<double>(ns) / 1e6; }

}  // namespace

void read_lines(std::string_view text, Timing& into) {
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto w = words(line);
        if (w.empty()) continue;
        if (w[0] == "frame" || w[0] == "seen") {
            unsigned index = 0, mark = 0;
            std::int64_t ns = 0;
            if (w.size() != 4 || !number(w[1], index) || index > 0xffff || !number(w[2], ns) || !number(w[3], mark) || mark > 1) {
                ++into.unread;
                continue;
            }
            if (w[0] == "frame") into.sent.push_back(Sent{static_cast<std::uint16_t>(index), ns, mark == 1});
            else into.seen.push_back(Seen{static_cast<std::uint16_t>(index), ns, mark == 1});
        } else if (w[0] == "press") {
            std::int64_t ns = 0;
            if (w.size() != 3 || !number(w[2], ns)) {
                ++into.unread;
                continue;
            }
            into.console_presses.push_back(ns);
        } else if (const std::size_t from = line.find("pretend: pressed "); from != std::string_view::npos) {
            // "... pretend: pressed NAME at NS ns"
            const auto rest = words(line.substr(from + 17));
            std::int64_t ns = 0;
            if (rest.size() != 4 || rest[1] != "at" || !number(rest[2], ns) || rest[3] != "ns") {
                ++into.unread;
                continue;
            }
            into.client_presses.push_back(ns);
        }
    }
    const auto by_time = [](const auto& a, const auto& b) { return a.ns < b.ns; };
    std::ranges::stable_sort(into.sent, by_time);
    std::ranges::stable_sort(into.seen, by_time);
    std::ranges::sort(into.console_presses);
    std::ranges::sort(into.client_presses);
}

Spread spread(std::vector<double> values) {
    Spread out;
    out.count = values.size();
    if (values.empty()) return out;
    std::ranges::sort(values);
    // Nearest rank: the value that this share of all values is at or under.
    const auto rank = [&](double share) {
        const auto place = static_cast<std::size_t>(std::ceil(share * static_cast<double>(values.size())));
        return values[std::clamp<std::size_t>(place, 1, values.size()) - 1];
    };
    out.median_ms = rank(0.5);
    out.p99_ms = rank(0.99);
    out.most_ms = values.back();
    out.least_ms = values.front();
    return out;
}

Score score(const Timing& timing) {
    Score out;
    if (timing.seen.empty()) {
        out.problems.emplace_back("the screen never showed a frame of the clip");
        return out;
    }
    if (timing.sent.empty()) {
        out.problems.emplace_back("the stand-in's log has no frames");
        return out;
    }

    // Each frame the screen showed, the first time it showed it, by its place among the frames sent:
    // the latest frame of that number sent before the screen showed it.
    std::map<std::uint16_t, std::vector<std::size_t>> by_index;
    for (std::size_t i = 0; i < timing.sent.size(); ++i) by_index[timing.sent[i].index].push_back(i);
    std::map<std::size_t, std::int64_t> first_seen;  // place among the frames sent -> when on the screen
    std::vector<std::int64_t> changes;                // when the screen went to another frame
    std::optional<std::uint16_t> showing;
    for (const Seen& seen : timing.seen) {
        if (showing == seen.counter) continue;
        showing = seen.counter;
        changes.push_back(seen.ns);
        const auto places = by_index.find(seen.counter);
        if (places == by_index.end()) {
            out.problems.push_back(std::format("the screen showed frame {}, which was never sent", seen.counter));
            continue;
        }
        std::optional<std::size_t> place;
        for (const std::size_t p : places->second) {
            if (timing.sent[p].ns <= seen.ns) place = p;
        }
        if (!place) {
            out.problems.push_back(std::format("the screen showed frame {} before it was sent", seen.counter));
            continue;
        }
        first_seen.try_emplace(*place, seen.ns);
    }
    if (first_seen.empty()) return out;

    const std::int64_t settled = timing.seen.front().ns + kSettleNs;
    const std::size_t first = first_seen.begin()->first, last = first_seen.rbegin()->first;
    std::vector<double> latency;
    for (std::size_t p = first; p <= last; ++p) {
        if (timing.sent[p].ns < settled) continue;
        ++out.sent;
        const auto shown = first_seen.find(p);
        if (shown == first_seen.end()) {
            ++out.never;
            continue;
        }
        ++out.reached;
        latency.push_back(ms(shown->second - timing.sent[p].ns));
    }
    out.latency = spread(std::move(latency));
    for (std::size_t i = 1; i < changes.size(); ++i) {
        if (changes[i - 1] < settled) continue;
        const double gap = ms(changes[i] - changes[i - 1]);
        ++out.gaps;
        if (gap >= 13.0 && gap <= 21.0) ++out.even;
        if (gap > 30.0) ++out.stood;
    }

    // Presses: the mark turns over in the frames sent after the stand-in read a press, and on the
    // screen when the first of those frames is shown.
    std::vector<std::int64_t> sent_turns, seen_turns;
    bool sent_mark = false, seen_mark = false;
    for (const Sent& sent : timing.sent) {
        if (sent.mark != sent_mark) sent_turns.push_back(sent.ns);
        sent_mark = sent.mark;
    }
    for (const Seen& seen : timing.seen) {
        if (seen.mark != seen_mark) seen_turns.push_back(seen.ns);
        seen_mark = seen.mark;
    }
    const std::size_t presses = timing.console_presses.size();
    if (!timing.client_presses.empty() && timing.client_presses.size() != presses) {
        out.problems.push_back(std::format("the client pressed {} times, the stand-in read {} presses", timing.client_presses.size(), presses));
    }
    if (sent_turns.size() != presses) out.problems.push_back(std::format("{} presses read, the mark sent {} times", presses, sent_turns.size()));
    if (seen_turns.size() < sent_turns.size()) out.presses_unanswered = sent_turns.size() - seen_turns.size();
    if (seen_turns.size() > sent_turns.size()) out.problems.push_back("the screen showed the mark turn more often than it was sent");
    std::vector<double> to_picture, to_console, wait, to_screen;
    const bool client = timing.client_presses.size() == presses;
    for (std::size_t n = 0; n < presses && n < sent_turns.size(); ++n) {
        const std::int64_t read = timing.console_presses[n];
        wait.push_back(ms(sent_turns[n] - read));
        if (client) to_console.push_back(ms(read - timing.client_presses[n]));
        if (n < seen_turns.size()) {
            to_screen.push_back(ms(seen_turns[n] - sent_turns[n]));
            if (client) to_picture.push_back(ms(seen_turns[n] - timing.client_presses[n]));
        }
    }
    for (const auto* values : {&to_picture, &to_console, &wait, &to_screen}) {
        if (std::ranges::any_of(*values, [](double v) { return v < 0.0; })) {
            out.problems.emplace_back("a press was answered before it was made: the presses do not pair up in order");
            break;
        }
    }
    out.press_to_picture = spread(std::move(to_picture));
    out.press_to_console = spread(std::move(to_console));
    out.console_wait = spread(std::move(wait));
    out.frame_to_screen = spread(std::move(to_screen));
    if (timing.unread != 0) out.problems.push_back(std::format("{} lines could not be read", timing.unread));
    return out;
}

std::string describe(const Score& score) {
    std::string text;
    const auto line = [&](const std::string& body) { text += "Lab: " + body + "\n"; };
    line(std::format("{} frames sent after the first second, {} reached the screen, {} never did (a newer one took their place first).",
                     score.sent, score.reached, score.never));
    if (score.latency.count != 0) {
        line(std::format("From sent to on the screen: median {:.1f} ms, 99th percentile {:.1f} ms, least {:.1f} ms, most {:.1f} ms.",
                         score.latency.median_ms, score.latency.p99_ms, score.latency.least_ms, score.latency.most_ms));
    }
    if (score.gaps != 0) {
        line(std::format("Between pictures on the screen: {:.1f}% 13-21 ms apart; {} times over 30 ms.",
                         100.0 * static_cast<double>(score.even) / static_cast<double>(score.gaps), score.stood));
    }
    if (score.console_wait.count != 0) {
        std::string body = std::format("{} presses", score.console_wait.count);
        if (score.press_to_picture.count != 0) {
            body += std::format(", press to picture: median {:.1f} ms, most {:.1f} ms", score.press_to_picture.median_ms, score.press_to_picture.most_ms);
        }
        body += " (";
        if (score.press_to_console.count != 0) body += std::format("to the console: median {:.1f} ms; ", score.press_to_console.median_ms);
        body += std::format("the console's wait for its next frame: median {:.1f} ms", score.console_wait.median_ms);
        if (score.frame_to_screen.count != 0) body += std::format("; that frame to the screen: median {:.1f} ms", score.frame_to_screen.median_ms);
        body += std::format("); {} never answered on the screen.", score.presses_unanswered);
        line(body);
    }
    for (const std::string& problem : score.problems) line("PROBLEM: " + problem + ".");
    return text;
}

}  // namespace psvp::lab
