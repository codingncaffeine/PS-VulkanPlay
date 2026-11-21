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
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = static_cast<Uint16>(vendor);
    desc.product_id = static_cast<Uint16>(product);
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "A controller for checks";
    return SDL_AttachVirtualJoystick(&desc) != 0;
}

}  // namespace psvp::app
