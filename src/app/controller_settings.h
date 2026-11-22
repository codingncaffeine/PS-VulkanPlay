// What the user chose for the controller while playing (PLAN D40, CONTROLLER-PLAN §3): which of
// the console's wishes reach it (trigger effects, light bar, player lights), which of its parts
// are told to the console (motion, touchpad), the sticks' dead zone, and what an Edge's Fn
// buttons do. The words the program's options take, and the dead zone's arithmetic; no SDL here.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace psvp::app {

enum class LightBar : std::uint8_t { console, off, fixed };
enum class FnAction : std::uint8_t { none, menu, stats, fullscreen };

struct ControllerSettings {
    bool trigger_effects = true;                    // the console's adaptive trigger effects reach a DualSense
    LightBar light_bar = LightBar::console;         // the light bar follows the console, is off, or one color
    std::array<std::uint8_t, 3> light_color{};      // for LightBar::fixed
    bool player_lights = true;                      // the player lights under the touchpad follow the console
    bool motion = true;                             // the motion sensors are read and told to the console
    bool touchpad = true;                           // touches on the touchpad are told to the console (its click is a button)
    int dead_zone = 0;                              // percent of a stick's throw that counts as rest, 0 to kMostDeadZone
    int rumble = 100;                               // percent of the rumble's strength: the console's, and that made from haptics
    bool pointer_touch = true;                      // the mouse and a touch screen on the window stand in for the touchpad
    bool dpad_touch = true;                         // on a controller with no touchpad: share held + the d-pad stand in for it
    FnAction fn_left = FnAction::none, fn_right = FnAction::none;  // a DualSense Edge's Fn buttons

    static constexpr int kMostDeadZone = 30;
    friend bool operator==(const ControllerSettings&, const ControllerSettings&) = default;
};

// "console", "off" or a color as six hexadecimal digits ("ff8000"); nothing for anything else.
[[nodiscard]] std::optional<std::pair<LightBar, std::array<std::uint8_t, 3>>> parse_light_bar(std::string_view text);
// "none", "menu", "stats", "fullscreen".
[[nodiscard]] std::optional<FnAction> parse_fn_action(std::string_view text);
[[nodiscard]] std::string_view fn_action_name(FnAction action);

// What differs from the defaults, as words for the log and the report ("" when nothing does).
[[nodiscard]] std::string describe(const ControllerSettings& settings);

// A stick's two axes with a round dead zone: inside it the stick is at rest, outside it the rest
// of its throw is stretched over the whole range (no jump where the dead zone ends; the direction
// is kept). Axes from -32768 to 32767.
[[nodiscard]] std::pair<std::int16_t, std::int16_t> apply_dead_zone(std::int16_t x, std::int16_t y, int percent);

// A motor's level (0 to 255) at the strength the user chose, as SDL's rumble takes it (0 to 65535).
[[nodiscard]] std::uint16_t rumble_at(std::uint8_t level, int percent) noexcept;

}  // namespace psvp::app
