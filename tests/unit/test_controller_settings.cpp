#include <cmath>
#include <cstdlib>

#include "app/controller_settings.h"
#include "check.h"

using namespace psvp::app;

PSVP_TEST(controller_settings_words) {
    CHECK(parse_light_bar("console")->first == LightBar::console);
    CHECK(parse_light_bar("off")->first == LightBar::off);
    const auto orange = parse_light_bar("ff8000");
    CHECK(orange && orange->first == LightBar::fixed && orange->second == (std::array<std::uint8_t, 3>{255, 128, 0}));
    CHECK(!parse_light_bar("ff80") && !parse_light_bar("gg8000") && !parse_light_bar("blue"));
    for (const FnAction action : {FnAction::none, FnAction::menu, FnAction::stats, FnAction::fullscreen}) CHECK(parse_fn_action(fn_action_name(action)) == action);
    CHECK(!parse_fn_action("screenshot"));
    CHECK(describe(ControllerSettings{}).empty());
    ControllerSettings s;
    s.trigger_effects = false;
    s.light_bar = LightBar::fixed;
    s.light_color = {0, 64, 255};
    s.dead_zone = 12;
    s.fn_right = FnAction::menu;
    CHECK(describe(s) == "trigger effects off, light bar 0040ff, dead zone 12%, right Fn: menu");
}

using Axes = std::pair<std::int16_t, std::int16_t>;

// A round dead zone: at rest inside it, no jump where it ends, the direction kept, full throw stays full.
PSVP_TEST(controller_settings_dead_zone) {
    CHECK((apply_dead_zone(1234, -2000, 0) == Axes{1234, -2000}));
    CHECK((apply_dead_zone(3000, 2000, 15) == Axes{}));  // 11% of the throw
    // Just past the zone: near rest, not a jump.
    const auto [x1, y1] = apply_dead_zone(static_cast<std::int16_t>(0.16 * 32767), 0, 15);
    CHECK(x1 > 0 && x1 < 600 && y1 == 0);
    CHECK((apply_dead_zone(32767, 0, 15) == Axes{32767, 0}));
    CHECK(apply_dead_zone(-32768, 0, 15).first == -32767);
    // The direction is kept: a diagonal stays a diagonal.
    const auto [x2, y2] = apply_dead_zone(16000, -16000, 20);
    CHECK(x2 == -y2 && x2 > 0);
    // Halfway past the zone along an axis: half the throw.
    const auto [x3, y3] = apply_dead_zone(static_cast<std::int16_t>(0.6 * 32767), 0, 20);
    CHECK(std::abs(x3 - 16384) < 10 && y3 == 0);
    // More than the most there is counts as the most.
    CHECK(apply_dead_zone(static_cast<std::int16_t>(0.32 * 32767), 0, 90).first > 0);
}

// The rumble at the strength the user chose: none at 0, all of it at 100, in proportion between.
PSVP_TEST(controller_settings_rumble_strength) {
    CHECK(rumble_at(255, 100) == 65535 && rumble_at(0, 100) == 0);
    CHECK(rumble_at(255, 0) == 0 && rumble_at(128, 0) == 0);
    CHECK(rumble_at(255, 50) == 32768 && rumble_at(100, 40) == 10280);
    CHECK(rumble_at(255, 150) == 65535 && rumble_at(255, -5) == 0);  // held to 0..100
    ControllerSettings s;
    s.rumble = 40;
    CHECK(describe(s) == "rumble 40%");
    s.light_brightness = 25;
    CHECK(describe(s) == "light bar at 25%, rumble 40%");
}
