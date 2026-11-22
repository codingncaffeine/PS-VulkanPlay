#include "app/controller_settings.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace psvp::app {

std::optional<std::pair<LightBar, std::array<std::uint8_t, 3>>> parse_light_bar(std::string_view text) {
    if (text == "console") return std::pair{LightBar::console, std::array<std::uint8_t, 3>{}};
    if (text == "off") return std::pair{LightBar::off, std::array<std::uint8_t, 3>{}};
    if (text.size() != 6) return std::nullopt;
    std::array<std::uint8_t, 3> color{};
    for (std::size_t i = 0; i < 3; ++i) {
        unsigned value = 0;
        const char* first = text.data() + 2 * i;
        const auto [end, error] = std::from_chars(first, first + 2, value, 16);
        if (error != std::errc{} || end != first + 2) return std::nullopt;
        color[i] = static_cast<std::uint8_t>(value);
    }
    return std::pair{LightBar::fixed, color};
}

std::optional<FnAction> parse_fn_action(std::string_view text) {
    for (const FnAction action : {FnAction::none, FnAction::menu, FnAction::stats, FnAction::fullscreen}) {
        if (fn_action_name(action) == text) return action;
    }
    return std::nullopt;
}

std::string_view fn_action_name(FnAction action) {
    switch (action) {
    case FnAction::none: return "none";
    case FnAction::menu: return "menu";
    case FnAction::stats: return "stats";
    case FnAction::fullscreen: return "fullscreen";
    }
    return "none";
}

std::string describe(const ControllerSettings& s) {
    std::string text;
    const auto add = [&](const std::string& part) {
        if (!text.empty()) text += ", ";
        text += part;
    };
    if (!s.trigger_effects) add("trigger effects off");
    if (s.light_bar == LightBar::off) add("light bar off");
    if (s.light_bar == LightBar::fixed) add(std::format("light bar {:02x}{:02x}{:02x}", s.light_color[0], s.light_color[1], s.light_color[2]));
    if (!s.player_lights) add("player lights off");
    if (!s.motion) add("motion off");
    if (!s.touchpad) add("touchpad off");
    if (s.dead_zone != 0) add(std::format("dead zone {}%", s.dead_zone));
    if (s.light_brightness != 100) add(std::format("light bar at {}%", s.light_brightness));
    if (s.rumble != 100) add(std::format("rumble {}%", s.rumble));
    if (!s.pointer_touch) add("touchpad from the mouse off");
    if (!s.dpad_touch) add("touchpad from the d-pad off");
    if (s.fn_left != FnAction::none) add(std::format("left Fn: {}", fn_action_name(s.fn_left)));
    if (s.fn_right != FnAction::none) add(std::format("right Fn: {}", fn_action_name(s.fn_right)));
    return text;
}

std::pair<std::int16_t, std::int16_t> apply_dead_zone(std::int16_t x, std::int16_t y, int percent) {
    percent = std::clamp(percent, 0, ControllerSettings::kMostDeadZone);
    if (percent == 0) return {x, y};
    const double fx = std::max(-1.0, static_cast<double>(x) / 32767.0), fy = std::max(-1.0, static_cast<double>(y) / 32767.0);
    const double length = std::hypot(fx, fy), zone = percent / 100.0;
    if (length <= zone) return {0, 0};
    // The throw past the zone over the whole range, in the stick's direction; no farther than full.
    const double scale = std::min(1.0, (length - zone) / (1.0 - zone)) / length;
    const auto axis = [&](double f) { return static_cast<std::int16_t>(std::clamp(std::lround(f * scale * 32767.0), -32768L, 32767L)); };
    return {axis(fx), axis(fy)};
}

std::uint16_t rumble_at(std::uint8_t level, int percent) noexcept {
    const int held = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    return static_cast<std::uint16_t>((level * 257 * held + 50) / 100);
}

}  // namespace psvp::app
