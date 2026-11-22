#include "app/stand_in_touch.h"
#include "check.h"

using namespace psvp::app;

namespace {
bool at(const std::optional<TouchSpot>& spot, float x, float y) {
    return spot && spot->x > x - 0.001F && spot->x < x + 0.001F && spot->y > y - 0.001F && spot->y < y + 0.001F;
}
}  // namespace

// The mouse: the left button is a finger that follows it; the others press the touchpad.
PSVP_TEST(stand_in_touch_mouse) {
    PointerTouch p;
    p.mouse_moved(0.2F, 0.2F);
    CHECK(!p.touch().fingers[0]);  // moving without a button is nothing
    p.mouse_button(PointerTouch::MouseButton::left, true, 0.25F, 0.5F);
    CHECK(!p.touch().fingers[0] && !p.touch().pressed);  // a click touches nothing
    p.mouse_moved(0.255F, 0.505F);
    CHECK(!p.touch().fingers[0]);                        // nor a twitch
    p.mouse_moved(0.3F, 0.5F);
    CHECK(at(p.touch().fingers[0], 0.3F, 0.5F));
    p.mouse_moved(0.75F, 1.5F);
    CHECK(at(p.touch().fingers[0], 0.75F, 1.0F));  // held inside the touchpad
    p.mouse_button(PointerTouch::MouseButton::left, false, 0.75F, 1.0F);
    CHECK(!p.touch().fingers[0]);
    p.mouse_button(PointerTouch::MouseButton::other, true, 0.5F, 0.5F);
    CHECK(p.touch().pressed && !p.touch().fingers[0]);
    p.mouse_button(PointerTouch::MouseButton::other, false, 0.5F, 0.5F);
    CHECK(!p.touch().pressed);
}

// A touch screen: two fingers at most; an edge or a second quick tap presses while it stays down.
PSVP_TEST(stand_in_touch_screen) {
    PointerTouch p;
    p.finger(7, true, 0.5F, 0.5F, 1000);
    CHECK(at(p.touch().fingers[0], 0.5F, 0.5F) && !p.touch().pressed);
    p.finger(8, true, 0.3F, 0.4F, 1010);
    p.finger(9, true, 0.6F, 0.6F, 1020);  // a third: nowhere
    CHECK(at(p.touch().fingers[1], 0.3F, 0.4F));
    p.finger_moved(8, 0.35F, 0.45F);
    CHECK(at(p.touch().fingers[1], 0.35F, 0.45F) && at(p.touch().fingers[0], 0.5F, 0.5F));
    p.finger(7, false, 0, 0, 1100);
    p.finger(8, false, 0, 0, 1100);
    CHECK(!p.touch().fingers[0] && !p.touch().fingers[1] && !p.touch().pressed);
    // Near an edge.
    p.finger(10, true, 0.97F, 0.5F, 5000);
    CHECK(p.touch().pressed);
    p.finger(10, false, 0, 0, 5050);
    CHECK(!p.touch().pressed);
    // Two taps: the second presses; a third tap right after is a first again.
    p.finger(11, true, 0.5F, 0.5F, 9000);
    p.finger(11, false, 0, 0, 9050);
    CHECK(!p.touch().pressed);
    p.finger(12, true, 0.5F, 0.5F, 9300);
    CHECK(p.touch().pressed);
    p.finger(12, false, 0, 0, 9350);
    p.finger(13, true, 0.5F, 0.5F, 9400);
    CHECK(!p.touch().pressed);
    p.finger(13, false, 0, 0, 9450);
    // Too slow: no press.
    p.finger(14, true, 0.5F, 0.5F, 9450 + 600);
    CHECK(!p.touch().pressed);
}

// The d-pad behind share: swipes and the south button's press; share alone is share, on its let-go.
PSVP_TEST(stand_in_touch_dpad) {
    DpadTouch d;
    CHECK(!d.key(DpadTouch::Key::up, true, 0));  // without share the d-pad is the d-pad
    CHECK(!d.key(DpadTouch::Key::up, false, 10));
    CHECK(d.key(DpadTouch::Key::share, true, 100));
    CHECK(!d.take_share_tap());
    CHECK(d.key(DpadTouch::Key::right, true, 200));
    CHECK(at(d.touch().fingers[0], 0.5F, 0.5F));
    d.tick(275);
    CHECK(at(d.touch().fingers[0], 0.65F, 0.5F));
    d.tick(350);
    CHECK(at(d.touch().fingers[0], 0.8F, 0.5F));  // where it ends, said once
    d.tick(360);
    CHECK(!d.touch().fingers[0]);                  // then lifted
    CHECK(d.key(DpadTouch::Key::right, false, 400));
    CHECK(d.key(DpadTouch::Key::south, true, 500) && d.touch().pressed);
    CHECK(d.key(DpadTouch::Key::south, false, 600) && !d.touch().pressed);
    CHECK(d.key(DpadTouch::Key::share, false, 700));
    CHECK(!d.take_share_tap());  // it was used: no share
    // Share alone.
    CHECK(d.key(DpadTouch::Key::share, true, 1000) && d.share_held());
    CHECK(d.key(DpadTouch::Key::share, false, 1100));
    CHECK(d.take_share_tap() && !d.take_share_tap());
    // Up swipes up.
    (void)d.key(DpadTouch::Key::share, true, 2000);
    (void)d.key(DpadTouch::Key::up, true, 2000);
    d.tick(2150);
    CHECK(at(d.touch().fingers[0], 0.5F, 0.2F));
}
