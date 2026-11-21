#include "app/pretend.h"

#include <charconv>

namespace psvp::app {

void only_pretended_controllers() {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_LINUX_CLASSIC, "0");
    SDL_SetHint("SDL_JOYSTICK_DEVICE", "/dev/null");
}

bool attach_pretended(std::string_view text) {
    const auto colon = text.find(':');
    unsigned vendor = 0, product = 0;
    if (colon == std::string_view::npos || std::from_chars(text.data(), text.data() + colon, vendor, 16).ec != std::errc{} ||
        std::from_chars(text.data() + colon + 1, text.data() + text.size(), product, 16).ec != std::errc{} || vendor > 0xffff || product > 0xffff) {
        return false;
    }
    // A touchpad of two fingers and motion sensors, as a DualSense has; what the program asks of the
    // controller is said in the log, for a check to read ("pretend: ...").
    static const SDL_VirtualJoystickTouchpadDesc touchpad{2, {0, 0, 0}};
    static const SDL_VirtualJoystickSensorDesc sensors[2] = {{SDL_SENSOR_GYRO, 250.0F}, {SDL_SENSOR_ACCEL, 250.0F}};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.ntouchpads = 1;
    desc.touchpads = &touchpad;
    desc.nsensors = 2;
    desc.sensors = sensors;
    desc.SetPlayerIndex = [](void*, int player) { log::debug("pretend: player lights set to player index {}", player); };
    desc.Rumble = [](void*, Uint16 low, Uint16 high) {
        log::debug("pretend: rumble {} and {}", low, high);
        return true;
    };
    desc.SetLED = [](void*, Uint8 red, Uint8 green, Uint8 blue) {
        log::debug("pretend: light bar set to {}, {}, {}", red, green, blue);
        return true;
    };
    desc.SendEffect = [](void*, const void*, int size) {
        log::debug("pretend: an effect report of {} bytes", size);
        return true;
    };
    desc.SetSensorsEnabled = [](void*, bool enabled) {
        log::debug("pretend: motion sensors {}", enabled ? "on" : "off");
        return true;
    };
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = static_cast<Uint16>(vendor);
    desc.product_id = static_cast<Uint16>(product);
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "A controller for checks";
    return SDL_AttachVirtualJoystick(&desc) != 0;
}

}  // namespace psvp::app
