#include "app/controllers.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#if defined(__linux__)
#include <unistd.h>
#endif

#include "app/controller_list.h"
#include "psvp/version.h"

namespace psvp::app {

namespace {

// A DualSense on a cable is also a sound device, whose last two channels move its actuators.
bool dualsense_sound_device_present() {
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    bool found = false;
    for (int i = 0; devices != nullptr && i < count && !found; ++i) {
        const char* name = SDL_GetAudioDeviceName(devices[i]);
        found = name != nullptr && SDL_strcasestr(name, "DualSense") != nullptr;
    }
    SDL_free(devices);
    return found;
}

// The controllers that are connected, each held open for as long as it is: what a controller
// says of its battery and its link it says only while it is open, and a wireless one says its
// battery level a moment after it was opened, and again whenever it changes.
class Controllers {
public:
    ~Controllers() {
        for (const Open& one : open_) SDL_CloseGamepad(one.pad);
    }

    // Opens the ones that came and lets go of the ones that went.
    void follow() {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        std::vector<Open> now;
        for (int i = 0; ids != nullptr && i < count; ++i) {
            const auto held = std::find_if(open_.begin(), open_.end(), [&](const Open& one) { return one.id == ids[i]; });
            if (held != open_.end()) {
                now.push_back(*held);
                open_.erase(held);
            } else if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) {
                now.push_back(Open{ids[i], pad});
            }
        }
        SDL_free(ids);
        for (const Open& gone : open_) SDL_CloseGamepad(gone.pad);
        open_ = std::move(now);
    }

    // True when every controller has said how its battery is (or that it has none).
    [[nodiscard]] bool batteries_known() const {
        return std::all_of(open_.begin(), open_.end(), [](const Open& one) {
            int percent = -1;
            return SDL_GetGamepadPowerInfo(one.pad, &percent) != SDL_POWERSTATE_UNKNOWN || percent >= 0;
        });
    }

    [[nodiscard]] std::vector<ControllerFacts> read(bool sound_device) const {
        std::vector<ControllerFacts> list;
        for (const Open& one : open_) {
            ControllerFacts facts;
            const char* name = SDL_GetGamepadName(one.pad);
            facts.name = name != nullptr ? name : "";
            const SDL_GamepadType type = SDL_GetGamepadType(one.pad);
            facts.kind = controller_kind(SDL_GetGamepadVendor(one.pad), SDL_GetGamepadProduct(one.pad), type == SDL_GAMEPAD_TYPE_PS5, type == SDL_GAMEPAD_TYPE_PS4,
                                         type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE,
                                         type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR ||
                                             type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT);
            const SDL_JoystickConnectionState link = SDL_GetGamepadConnectionState(one.pad);
            facts.link = link == SDL_JOYSTICK_CONNECTION_WIRED ? ControllerFacts::Link::cable
                         : link == SDL_JOYSTICK_CONNECTION_WIRELESS ? ControllerFacts::Link::wireless : ControllerFacts::Link::unknown;
            int percent = -1;
            const SDL_PowerState power = SDL_GetGamepadPowerInfo(one.pad, &percent);
            facts.battery = percent;
            facts.power = power == SDL_POWERSTATE_ON_BATTERY ? ControllerFacts::Power::on_battery : power == SDL_POWERSTATE_CHARGING ? ControllerFacts::Power::charging
                          : power == SDL_POWERSTATE_CHARGED ? ControllerFacts::Power::full : power == SDL_POWERSTATE_NO_BATTERY ? ControllerFacts::Power::no_battery
                          : ControllerFacts::Power::unknown;
            facts.touchpad = SDL_GetNumGamepadTouchpads(one.pad) > 0;
            facts.motion = SDL_GamepadHasSensor(one.pad, SDL_SENSOR_GYRO);
            facts.rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(one.pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
            const bool dualsense = facts.kind == ControllerFacts::Kind::dualsense || facts.kind == ControllerFacts::Kind::dualsense_edge;
            if (dualsense) facts.haptics = sound_device && facts.link != ControllerFacts::Link::wireless ? ControllerFacts::Haptics::sound_device : ControllerFacts::Haptics::rumble;
            list.push_back(std::move(facts));
        }
        return list;
    }

private:
    struct Open {
        SDL_JoystickID id = 0;
        SDL_Gamepad* pad = nullptr;
    };
    std::vector<Open> open_;
};

std::string say(const std::vector<ControllerFacts>& list, bool json) {
    if (json) return controllers_json(list) + "\n";
    if (list.empty()) return "No controller is connected.\n";
    std::string text;
    for (const ControllerFacts& one : list) text += controller_line(one) + "\n";
    return text;
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

}  // namespace

int run_controllers(const ControllersOptions& options) {
    SDL_SetAppMetadata(PSVP_NAME, PSVP_VERSION, PSVP_APP_ID);
    // No window of this program ever has the keyboard: controllers are read all the same.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (options.pretend) {
        // Only the pretended one: nothing of this machine is opened.
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_LINUX_CLASSIC, "0");
        SDL_SetHint("SDL_JOYSTICK_DEVICE", "/dev/null");
    }
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "controllers: controllers cannot be read: %s\n", SDL_GetError());
        return 1;
    }
    // Sound devices only say whether a DualSense's own one is there; without sound the list is the same, less that.
    const bool sound = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (options.pretend && !attach_pretended(*options.pretend)) {
        std::fprintf(stderr, "controllers: --pretend takes a vendor and a product number, as 054c:0df2\n");
        SDL_Quit();
        return 64;
    }
    {
        Controllers controllers;
        const auto look = [&] {
            controllers.follow();
            return say(controllers.read(sound && dualsense_sound_device_present()), options.json);
        };
        // A controller that was just opened has not said its battery level yet: a moment for that,
        // so that the first thing said is whole.
        const auto settle = [&] {
            const Uint64 until = SDL_GetTicks() + 700;
            do {
                SDL_PumpEvents();
                SDL_UpdateGamepads();
                controllers.follow();
                if (controllers.batteries_known()) break;
                SDL_Delay(20);
            } while (SDL_GetTicks() < until);
        };
        settle();
        std::string said = look();
        std::fputs(said.c_str(), stdout);
        std::fflush(stdout);
        if (options.watch) {
#if defined(__linux__)
            const pid_t parent = ::getppid();
#endif
            Uint64 looked = SDL_GetTicks();
            for (bool running = true; running;) {
                SDL_Event event;
                bool changed = false, came = false;
                if (SDL_WaitEventTimeout(&event, 1000)) {
                    do {
                        if (event.type == SDL_EVENT_QUIT) running = false;
                        came = came || event.type == SDL_EVENT_GAMEPAD_ADDED;
                        changed = changed || event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED ||
                                  event.type == SDL_EVENT_JOYSTICK_BATTERY_UPDATED || event.type == SDL_EVENT_AUDIO_DEVICE_ADDED ||
                                  event.type == SDL_EVENT_AUDIO_DEVICE_REMOVED;
                    } while (SDL_PollEvent(&event));
                }
#if defined(__linux__)
                // Whoever started this has gone: nobody reads what is said any more.
                if (::getppid() != parent) running = false;
#endif
                // What changed without a word is seen at the latest half a minute later.
                if (!running || (!changed && SDL_GetTicks() - looked < 30000)) continue;
                looked = SDL_GetTicks();
                if (came) settle();
                std::string now = look();
                if (now == said) continue;
                said = std::move(now);
                std::fputs(said.c_str(), stdout);
                std::fflush(stdout);
            }
        }
    }
    SDL_Quit();
    return 0;
}

}  // namespace psvp::app
