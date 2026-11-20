#include "app/controller_presses.h"

#include <algorithm>
#include <cstdlib>
#include <format>

namespace psvp::app {

namespace {

bool is_direction(Press press) noexcept { return press == Press::up || press == Press::down || press == Press::left || press == Press::right; }

// How far the stick is pushed toward a direction.
int toward(Press direction, int x, int y) noexcept {
    switch (direction) {
        case Press::up: return -y;
        case Press::down: return y;
        case Press::left: return -x;
        case Press::right: return x;
        default: return 0;
    }
}

}  // namespace

const char* press_text(Press press) noexcept {
    switch (press) {
        case Press::up: return "up";
        case Press::down: return "down";
        case Press::left: return "left";
        case Press::right: return "right";
        case Press::accept: return "accept";
        case Press::back: return "back";
        case Press::previous_page: return "previous";
        case Press::next_page: break;
    }
    return "next";
}

std::string press_json(Press press, int controller) { return std::format(R"({{"press":"{}","controller":{}}})", press_text(press), controller); }

void PressReader::button(Press which, bool down, std::uint64_t now_ms, std::vector<Press>& out) {
    if (!is_direction(which)) {
        bool& held = others_down_[static_cast<int>(which) - static_cast<int>(Press::accept)];
        if (down && !held) out.push_back(which);
        held = down;
        return;
    }
    const auto at = std::find(buttons_.begin(), buttons_.end(), which);
    if (down && at == buttons_.end()) buttons_.push_back(which);
    else if (!down && at != buttons_.end()) buttons_.erase(at);
    else return;
    direction_may_have_changed(now_ms, out);
}

void PressReader::stick(int x, int y, std::uint64_t now_ms, std::vector<Press>& out) {
    const Press along = std::abs(x) >= std::abs(y) ? (x < 0 ? Press::left : Press::right) : (y < 0 ? Press::up : Press::down);
    const int far = std::max(std::abs(x), std::abs(y));
    if (stick_) {
        // A direction that began goes on until the stick is nearly back, or is pushed clearly
        // another way: a stick held between two directions does not flutter from one to the other.
        const int still = toward(*stick_, x, y);
        if (along != *stick_ && far >= kStickOn && far > 2 * std::max(still, 0)) stick_ = along;
        else if (still < kStickOff) stick_ = far >= kStickOn ? std::optional(along) : std::nullopt;
    } else if (far >= kStickOn) {
        stick_ = along;
    }
    direction_may_have_changed(now_ms, out);
}

void PressReader::direction_may_have_changed(std::uint64_t now_ms, std::vector<Press>& out) {
    const std::optional<Press> now = !buttons_.empty() ? std::optional(buttons_.back()) : stick_;
    if (now == held_) return;
    held_ = now;
    if (!held_) return;
    out.push_back(*held_);
    repeat_at_ = now_ms + kFirstRepeatMs;
}

void PressReader::tick(std::uint64_t now_ms, std::vector<Press>& out) {
    if (!held_ || now_ms < repeat_at_) return;
    // One press however late this is: a program that was held up does not make up for it in a burst.
    out.push_back(*held_);
    repeat_at_ = std::max(repeat_at_ + kRepeatMs, now_ms + kRepeatMs / 2);
}

}  // namespace psvp::app
