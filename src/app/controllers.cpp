#include "app/controllers.h"
#include "app/pretend.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#if defined(__linux__)
#include <unistd.h>
#endif

#include "app/controller_list.h"
#include "app/controller_presses.h"
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
                Open one;
                one.id = ids[i];
                one.pad = pad;
                // Where the maker prints A on the right-hand button, that one takes and the lower one goes back.
                one.accept_is_east = SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_EAST) == SDL_GAMEPAD_BUTTON_LABEL_A;
                now.push_back(std::move(one));
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

    // What is known of each controller, in the order they are held: the list that is said. A
    // press names its controller by its place in the list read last.
    [[nodiscard]] std::vector<ControllerFacts> read(bool sound_device) {
        std::vector<ControllerFacts> list;
        said_.clear();
        for (const Open& one : open_) {
            said_.push_back(one.id);
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

    // A press, and the controller it is of.
    struct Pressed {
        SDL_JoystickID controller = 0;
        Press press = Press::accept;
    };

    // What an event of the input library means to an app that is moved through with a controller.
    void take(const SDL_Event& event, std::uint64_t now_ms, std::vector<Pressed>& out) {
        std::vector<Press> presses;
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            const int at = place_of(event.gbutton.which);
            if (at < 0) return;
            Open& one = open_[static_cast<std::size_t>(at)];
            const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            switch (event.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP: one.presses.button(Press::up, down, now_ms, presses); break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN: one.presses.button(Press::down, down, now_ms, presses); break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT: one.presses.button(Press::left, down, now_ms, presses); break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: one.presses.button(Press::right, down, now_ms, presses); break;
                case SDL_GAMEPAD_BUTTON_SOUTH: one.presses.button(one.accept_is_east ? Press::back : Press::accept, down, now_ms, presses); break;
                case SDL_GAMEPAD_BUTTON_EAST: one.presses.button(one.accept_is_east ? Press::accept : Press::back, down, now_ms, presses); break;
                case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: one.presses.button(Press::previous_page, down, now_ms, presses); break;
                case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: one.presses.button(Press::next_page, down, now_ms, presses); break;
                default: break;
            }
            for (const Press press : presses) out.push_back(Pressed{one.id, press});
        } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
            const int at = place_of(event.gaxis.which);
            if (at < 0) return;
            Open& one = open_[static_cast<std::size_t>(at)];
            (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? one.stick_x : one.stick_y) = event.gaxis.value;
            one.presses.stick(one.stick_x, one.stick_y, now_ms, presses);
            for (const Press press : presses) out.push_back(Pressed{one.id, press});
        }
    }

    // Time has passed: what the held directions are due.
    void tick(std::uint64_t now_ms, std::vector<Pressed>& out) {
        std::vector<Press> presses;
        for (Open& one : open_) {
            presses.clear();
            one.presses.tick(now_ms, presses);
            for (const Press press : presses) out.push_back(Pressed{one.id, press});
        }
    }

    [[nodiscard]] std::optional<std::uint64_t> next_repeat() const {
        std::optional<std::uint64_t> next;
        for (const Open& one : open_) {
            if (const auto at = one.presses.next_repeat(); at && (!next || *at < *next)) next = at;
        }
        return next;
    }

    // A controller's place in the list that was read last, or -1 for one that came since.
    [[nodiscard]] int said_place_of(SDL_JoystickID id) const {
        const auto at = std::find(said_.begin(), said_.end(), id);
        return at == said_.end() ? -1 : static_cast<int>(at - said_.begin());
    }

    [[nodiscard]] std::string name_of(SDL_JoystickID id) const {
        const int at = place_of(id);
        const char* name = at >= 0 ? SDL_GetGamepadName(open_[static_cast<std::size_t>(at)].pad) : nullptr;
        return name != nullptr && *name != 0 ? name : "A controller";
    }

    // The first controller, for the checks that press the buttons of a pretended one.
    [[nodiscard]] SDL_Joystick* first_joystick() const { return open_.empty() ? nullptr : SDL_GetGamepadJoystick(open_.front().pad); }

private:
    struct Open {
        SDL_JoystickID id = 0;
        SDL_Gamepad* pad = nullptr;
        bool accept_is_east = false;
        int stick_x = 0, stick_y = 0;
        PressReader presses;
    };

    [[nodiscard]] int place_of(SDL_JoystickID id) const {
        const auto at = std::find_if(open_.begin(), open_.end(), [&](const Open& one) { return one.id == id; });
        return at == open_.end() ? -1 : static_cast<int>(at - open_.begin());
    }

    std::vector<Open> open_;
    std::vector<SDL_JoystickID> said_;
};

std::string say(const std::vector<ControllerFacts>& list, bool json) {
    if (json) return controllers_json(list) + "\n";
    if (list.empty()) return "No controller is connected.\n";
    std::string text;
    for (const ControllerFacts& one : list) text += controller_line(one) + "\n";
    return text;
}

}  // namespace

int run_controllers(const ControllersOptions& options) {
    SDL_SetAppMetadata(PSVP_NAME, PSVP_VERSION, PSVP_APP_ID);
    // No window of this program ever has the keyboard: controllers are read all the same.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (options.pretend) only_pretended_controllers();
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
            std::optional<Taps> taps;
            if (options.pretend_taps) taps.emplace(*options.pretend_taps);
            std::vector<Controllers::Pressed> pressed;
            Uint64 looked = SDL_GetTicks();
            // A controller that came is given a moment to say its battery level before the list is
            // said with it. The moment is not waited out here: what is pressed meanwhile is said at once.
            std::optional<Uint64> settling_until;
            bool changed = false;
            for (bool running = true; running;) {
                SDL_Event event;
                bool came = false;
                // No longer than until a held direction is due again, or the checks' file is looked at again.
                Sint32 wait = 1000;
                if (options.presses) {
                    if (const auto due = controllers.next_repeat()) {
                        const Uint64 now = SDL_GetTicks();
                        wait = *due <= now ? 1 : static_cast<Sint32>(std::min<Uint64>(*due - now, 1000));
                    }
                }
                if (taps) {
                    taps->follow(controllers.first_joystick(), SDL_GetTicks());
                    wait = std::min<Sint32>(wait, 15);
                }
                if (settling_until) wait = std::min<Sint32>(wait, 20);
                if (SDL_WaitEventTimeout(&event, wait)) {
                    do {
                        if (event.type == SDL_EVENT_QUIT) running = false;
                        if (options.presses) controllers.take(event, SDL_GetTicks(), pressed);
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
                if (!running) continue;
                if (came) settling_until = SDL_GetTicks() + 700;
                if (settling_until) {
                    controllers.follow();
                    if (controllers.batteries_known() || SDL_GetTicks() >= *settling_until) settling_until.reset();
                }
                // What changed without a word is seen at the latest half a minute later.
                if (!settling_until && (changed || SDL_GetTicks() - looked >= 30000)) {
                    looked = SDL_GetTicks();
                    changed = false;
                    std::string now = look();
                    if (now != said) {
                        said = std::move(now);
                        std::fputs(said.c_str(), stdout);
                        std::fflush(stdout);
                    }
                }
                if (options.presses) {
                    controllers.tick(SDL_GetTicks(), pressed);
                    for (const Controllers::Pressed& one : pressed) {
                        const std::string line = options.json ? press_json(one.press, controllers.said_place_of(one.controller)) + "\n"
                                                              : controllers.name_of(one.controller) + ": " + press_text(one.press) + "\n";
                        std::fputs(line.c_str(), stdout);
                    }
                    if (!pressed.empty()) std::fflush(stdout);
                    pressed.clear();
                }
            }
        }
    }
    SDL_Quit();
    return 0;
}

}  // namespace psvp::app
