// What a controller's buttons and left stick mean to an app that is moved through with it:
// a direction, "take this", "go back", the page before and the page after. Reading the
// controller itself is not in here; time is given, never asked for.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace psvp::app {

enum class Press { up, down, left, right, accept, back, previous_page, next_page };

// "up", "down", "left", "right", "accept", "back", "previous", "next".
[[nodiscard]] const char* press_text(Press press) noexcept;
// {"press":"down","controller":0} on one line: the controller is its place in the list last said,
// or -1 for one that is not in that list yet.
[[nodiscard]] std::string press_json(Press press, int controller);

// One controller. A button says its press once, when it goes down. A direction that is held
// (on the direction buttons or the left stick) is said again after a moment, then steadily,
// the way a held key is.
class PressReader {
public:
    static constexpr std::uint64_t kFirstRepeatMs = 400;
    static constexpr std::uint64_t kRepeatMs = 110;
    // Of the stick's full throw of 32767: a direction begins past the first and ends under the second.
    static constexpr int kStickOn = 19660;
    static constexpr int kStickOff = 13107;

    // A button went down or came up. A direction button and the stick are one thing to whoever
    // reads the presses: the buttons win while one of them is held.
    void button(Press which, bool down, std::uint64_t now_ms, std::vector<Press>& out);
    // The left stick: right and down are positive, as the input library says them.
    void stick(int x, int y, std::uint64_t now_ms, std::vector<Press>& out);
    // Time has passed: says what a held direction is due.
    void tick(std::uint64_t now_ms, std::vector<Press>& out);
    // When tick() has something to say next, if a direction is held.
    [[nodiscard]] std::optional<std::uint64_t> next_repeat() const noexcept { return held_ ? std::optional(repeat_at_) : std::nullopt; }

private:
    void direction_may_have_changed(std::uint64_t now_ms, std::vector<Press>& out);

    // The direction buttons that are down, the one pressed last at the back.
    std::vector<Press> buttons_;
    std::optional<Press> stick_;
    std::optional<Press> held_;
    std::uint64_t repeat_at_ = 0;
    bool others_down_[4] = {false, false, false, false};  // accept, back, previous, next
};

}  // namespace psvp::app
