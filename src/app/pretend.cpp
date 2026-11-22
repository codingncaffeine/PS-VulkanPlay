#include "app/pretend.h"

#include <charconv>
#include <cstdint>
#include <format>
#include <string>

namespace psvp::app {

void only_pretended_controllers() {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_LINUX_CLASSIC, "0");
    SDL_SetHint("SDL_JOYSTICK_DEVICE", "/dev/null");
}

namespace {

// What the program asks of pretended controller `number`, said in the log for a check to read.
std::string who(void* userdata) {
    const auto number = reinterpret_cast<std::uintptr_t>(userdata);
    return number == 1 ? std::string("pretend") : std::format("pretend {}", number);
}

bool attach_one(std::string_view text, std::uintptr_t number) {
    const auto colon = text.find(':');
    unsigned vendor = 0, product = 0;
    if (colon == std::string_view::npos || std::from_chars(text.data(), text.data() + colon, vendor, 16).ec != std::errc{} ||
        std::from_chars(text.data() + colon + 1, text.data() + text.size(), product, 16).ec != std::errc{} || vendor > 0xffff || product > 0xffff) {
        return false;
    }
    // A touchpad of two fingers and motion sensors, as a DualSense has; other makers' controllers
    // have no touchpad (the d-pad stands in for one there).
    static const SDL_VirtualJoystickTouchpadDesc touchpad{2, {0, 0, 0}};
    static const SDL_VirtualJoystickSensorDesc sensors[2] = {{SDL_SENSOR_GYRO, 250.0F}, {SDL_SENSOR_ACCEL, 250.0F}};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.userdata = reinterpret_cast<void*>(number);
    desc.ntouchpads = vendor == 0x054c ? 1 : 0;
    desc.touchpads = &touchpad;
    desc.nsensors = 2;
    desc.sensors = sensors;
    desc.SetPlayerIndex = [](void* self, int player) { log::debug("{}: player lights set to player index {}", who(self), player); };
    desc.Rumble = [](void* self, Uint16 low, Uint16 high) {
        log::debug("{}: rumble {} and {}", who(self), low, high);
        return true;
    };
    desc.SetLED = [](void* self, Uint8 red, Uint8 green, Uint8 blue) {
        log::debug("{}: light bar set to {}, {}, {}", who(self), red, green, blue);
        return true;
    };
    desc.SendEffect = [](void* self, const void*, int size) {
        log::debug("{}: an effect report of {} bytes", who(self), size);
        return true;
    };
    desc.SetSensorsEnabled = [](void* self, bool enabled) {
        log::debug("{}: motion sensors {}", who(self), enabled ? "on" : "off");
        return true;
    };
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = static_cast<Uint16>(vendor);
    desc.product_id = static_cast<Uint16>(product);
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    const std::string name = number == 1 ? std::string("A controller for checks") : std::format("Controller {} for checks", number);
    desc.name = name.c_str();  // SDL keeps its own copy
    return SDL_AttachVirtualJoystick(&desc) != 0;
}

}  // namespace

bool attach_pretended(std::string_view text) {
    std::uintptr_t number = 0;
    while (true) {
        const auto comma = text.find(',');
        if (!attach_one(text.substr(0, comma), ++number)) return false;
        if (comma == std::string_view::npos) return true;
        text.remove_prefix(comma + 1);
    }
}

}  // namespace psvp::app
