// Stand-ins for a touchpad, for whoever has none in hand: the mouse and a touch screen on the
// stream's window, and a controller's d-pad. Each says where its fingers are on the touchpad (0 to
// 1 across and down) and whether the touchpad is pressed in; the player tells the console. No SDL,
// no clock of its own: times come in as milliseconds.
//
// The mouse: the left button held and moved is a finger (from where it was pressed, once it moved a
// hundredth of the window: a click, or a double click for the full screen, touches nothing); the
// right or middle button held is a press of the touchpad. A touch screen: each finger is a finger (two at most), one put down near
// an edge (the outer twentieth) or the second of two taps within half a second also presses the
// touchpad while it stays down. (Both as the reference does; its rules were read in its source.)
//
// The d-pad, on a controller with no touchpad: the share button (View, Back, Select) held turns the
// d-pad into swipes, each a finger put down in the middle and drawn three tenths of the way across
// in the d-pad's direction, and the south button into a press of the touchpad. Share pressed and
// let go with neither is share, sent when it is let go.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace psvp::app {

struct TouchSpot {
    float x = 0.0F, y = 0.0F;
    friend bool operator==(const TouchSpot&, const TouchSpot&) = default;
};

// What a stand-in says: up to two fingers, and the touchpad pressed in or not.
struct StandInTouch {
    std::array<std::optional<TouchSpot>, 2> fingers;
    bool pressed = false;
    friend bool operator==(const StandInTouch&, const StandInTouch&) = default;
};

class PointerTouch {
public:
    enum class MouseButton { left, other };
    void mouse_button(MouseButton button, bool down, float x, float y);
    void mouse_moved(float x, float y);  // seen only while the left button is held
    // A touch screen's finger, by the number the system gave it.
    void finger(std::uint64_t id, bool down, float x, float y, std::uint64_t now_ms);
    void finger_moved(std::uint64_t id, float x, float y);
    [[nodiscard]] const StandInTouch& touch() const { return touch_; }

    static constexpr float kEdge = 0.05F;
    static constexpr float kMouseStill = 0.01F;
    static constexpr std::uint64_t kDoubleTapMs = 500;

private:
    void say_pressed();
    StandInTouch touch_;
    bool mouse_left_ = false, mouse_other_ = false;
    TouchSpot mouse_from_{};  // where the left button went down
    std::array<std::optional<std::uint64_t>, 2> finger_ids_;  // which screen finger is in which place
    std::array<bool, 2> finger_presses_{};                    // that finger presses the touchpad
    std::optional<std::uint64_t> last_tap_ms_;
};

class DpadTouch {
public:
    enum class Key { share, up, down, left, right, south };
    // A press or a let-go of one of the keys above. True when it was taken for the touchpad (the
    // console must not see it as itself).
    [[nodiscard]] bool key(Key key, bool down, std::uint64_t now_ms);
    // Moves a swipe on; call it as time passes (every picture is often enough).
    void tick(std::uint64_t now_ms);
    [[nodiscard]] const StandInTouch& touch() const { return touch_; }
    // Share, when it was pressed and let go with nothing between: the player sends a press of it.
    [[nodiscard]] bool take_share_tap() { return std::exchange(share_tap_, false); }
    [[nodiscard]] bool share_held() const { return share_held_; }

    static constexpr float kSwipe = 0.3F;
    static constexpr std::uint64_t kSwipeMs = 150;

private:
    StandInTouch touch_;
    bool share_held_ = false, used_ = false, share_tap_ = false;
    std::optional<std::uint64_t> swipe_from_ms_;
    TouchSpot swipe_to_{};
};

}  // namespace psvp::app
