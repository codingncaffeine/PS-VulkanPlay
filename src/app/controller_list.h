// What is known of the controllers that are connected, and how it is said: as a line for a
// person and as JSON for the app. Reading the controllers themselves is not in here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app/json.h"

namespace psvp::app {

struct ControllerFacts {
    enum class Kind { dualsense, dualsense_edge, dualshock4, xbox, nintendo, other };
    enum class Link { unknown, cable, wireless };
    enum class Power { unknown, on_battery, charging, full, no_battery };
    // What a DualSense's haptics can be played through here: its own sound device (on a cable),
    // or its two motors, as rumble made from the haptics.
    enum class Haptics { none, rumble, sound_device };

    std::string name;
    Kind kind = Kind::other;
    Link link = Link::unknown;
    Power power = Power::unknown;
    int battery = -1;  // percent, or -1 when the controller does not say
    bool touchpad = false;
    bool motion = false;
    bool rumble = false;
    Haptics haptics = Haptics::none;
};

// The kind of a controller from its USB vendor and product numbers and what the input library
// took it for (`sony_ps5`, `sony_ps4`, `xbox`, `nintendo`: any of them may be false).
[[nodiscard]] ControllerFacts::Kind controller_kind(std::uint16_t vendor, std::uint16_t product, bool sony_ps5, bool sony_ps4, bool xbox, bool nintendo) noexcept;

// "DualSense Edge Wireless Controller: on a cable, 55%, charging; haptics through its own sound device".
[[nodiscard]] std::string controller_line(const ControllerFacts& controller);
// {"controllers":[{"name":…,"kind":"dualsense-edge","link":"cable","battery":55,"power":"charging",…}]} on one line.
[[nodiscard]] std::string controllers_json(const std::vector<ControllerFacts>& controllers);

}  // namespace psvp::app
