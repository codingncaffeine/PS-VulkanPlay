#include "app/stand_in_touch.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace psvp::app {

namespace {

TouchSpot held(float x, float y) { return {std::clamp(x, 0.0F, 1.0F), std::clamp(y, 0.0F, 1.0F)}; }

bool near_an_edge(float x, float y) {
    return x <= PointerTouch::kEdge || x >= 1.0F - PointerTouch::kEdge || y <= PointerTouch::kEdge || y >= 1.0F - PointerTouch::kEdge;
}

}  // namespace

void PointerTouch::say_pressed() { touch_.pressed = mouse_other_ || finger_presses_[0] || finger_presses_[1]; }

void PointerTouch::mouse_button(MouseButton button, bool down, float x, float y) {
    if (button == MouseButton::other) {
        mouse_other_ = down;
    } else {
        mouse_left_ = down;
        mouse_from_ = held(x, y);
        // The mouse's finger is the first place, put down once it moves; let go, it is lifted.
        if (!down) touch_.fingers[0].reset();
    }
    say_pressed();
}

void PointerTouch::mouse_moved(float x, float y) {
    if (!mouse_left_) return;
    const TouchSpot now = held(x, y);
    if (!touch_.fingers[0] || finger_ids_[0]) {
        if (std::abs(now.x - mouse_from_.x) < kMouseStill && std::abs(now.y - mouse_from_.y) < kMouseStill) return;
        // A touch screen's finger there gives way.
        finger_ids_[0].reset();
        finger_presses_[0] = false;
        say_pressed();
    }
    touch_.fingers[0] = now;
}

void PointerTouch::finger(std::uint64_t id, bool down, float x, float y, std::uint64_t now_ms) {
    if (!down) {
        for (std::size_t i = 0; i < 2; ++i) {
            if (finger_ids_[i] == id) {
                finger_ids_[i].reset();
                touch_.fingers[i].reset();
                finger_presses_[i] = false;
            }
        }
        say_pressed();
        return;
    }
    const bool first = !finger_ids_[0] && !finger_ids_[1];
    const auto place = std::ranges::find_if(finger_ids_, [](const auto& one) { return !one.has_value(); });
    if (place == finger_ids_.end()) return;  // a third finger: the touchpad has two
    const auto i = static_cast<std::size_t>(place - finger_ids_.begin());
    *place = id;
    touch_.fingers[i] = held(x, y);
    bool presses = near_an_edge(x, y);
    // A tap right behind a tap, with nothing else on the screen, presses the touchpad.
    if (!presses && first) {
        presses = last_tap_ms_ && now_ms - *last_tap_ms_ < kDoubleTapMs;
        last_tap_ms_ = presses ? std::nullopt : std::optional<std::uint64_t>(now_ms);
    }
    finger_presses_[i] = presses;
    say_pressed();
}

void PointerTouch::finger_moved(std::uint64_t id, float x, float y) {
    for (std::size_t i = 0; i < 2; ++i) {
        if (finger_ids_[i] == id) touch_.fingers[i] = held(x, y);
    }
}

bool DpadTouch::key(Key key, bool down, std::uint64_t now_ms) {
    if (key == Key::share) {
        if (down) {
            share_held_ = true;
            used_ = false;
        } else if (share_held_) {
            share_held_ = false;
            share_tap_ = !used_;
            touch_.pressed = false;
        }
        return true;
    }
    if (!share_held_) return false;
    used_ = true;
    if (key == Key::south) {
        touch_.pressed = down;
        return true;
    }
    if (!down) return true;
    // A swipe from the middle, in the d-pad's direction.
    const float dx = key == Key::left ? -kSwipe : key == Key::right ? kSwipe : 0.0F;
    const float dy = key == Key::up ? -kSwipe : key == Key::down ? kSwipe : 0.0F;
    swipe_to_ = {0.5F + dx, 0.5F + dy};
    swipe_from_ms_ = now_ms;
    touch_.fingers[0] = TouchSpot{0.5F, 0.5F};
    return true;
}

void DpadTouch::tick(std::uint64_t now_ms) {
    if (!swipe_from_ms_) return;
    const std::uint64_t gone = now_ms - *swipe_from_ms_;
    if (gone >= kSwipeMs) {
        // At the end, once (the console sees where the swipe ended); lifted the next time.
        if (touch_.fingers[0] != swipe_to_) {
            touch_.fingers[0] = swipe_to_;
        } else {
            touch_.fingers[0].reset();
            swipe_from_ms_.reset();
        }
        return;
    }
    const float part = static_cast<float>(gone) / static_cast<float>(kSwipeMs);
    touch_.fingers[0] = TouchSpot{0.5F + (swipe_to_.x - 0.5F) * part, 0.5F + (swipe_to_.y - 0.5F) * part};
}

}  // namespace psvp::app
