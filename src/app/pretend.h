// For checks: a controller that is not there, made with the input library's virtual joystick, and
// a file whose lines press its buttons. Nothing of the machine's own controllers is opened meanwhile.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include "app/log.h"

namespace psvp::app {

// Before the input library starts: none of this machine's controllers is opened.
void only_pretended_controllers();
// A gamepad with the vendor and product numbers "054c:0df2"; false when the text is not that.
[[nodiscard]] bool attach_pretended(std::string_view text);

// For checks: presses the buttons of the pretended controller as a file says, line by line as
// the lines are added to it. "tap NAME" (down, and up a moment later), "down NAME", "up NAME",
// "stick X Y" (the left stick, each from -32768 to 32767).
class Taps {
public:
    explicit Taps(std::string file) : file_(std::move(file)) {}

    void follow(SDL_Joystick* joystick, std::uint64_t now_ms) {
        if (joystick == nullptr) return;
        for (auto due = lifts_.begin(); due != lifts_.end();) {
            if (due->at > now_ms) {
                ++due;
                continue;
            }
            SDL_SetJoystickVirtualButton(joystick, due->button, false);
            due = lifts_.erase(due);
        }
        std::ifstream in(file_, std::ios::binary);
        if (!in) return;
        in.seekg(read_);
        std::string line;
        // A line counts when it has its end: one that is still being written is read the next time.
        while (std::getline(in, line) && !in.eof()) {
            read_ = in.tellg();
            const auto space = line.find(' ');
            const std::string verb = line.substr(0, space), rest = space == std::string::npos ? "" : line.substr(space + 1);
            if (verb == "stick") {
                int x = 0, y = 0;
                if (std::sscanf(rest.c_str(), "%d %d", &x, &y) == 2) {
                    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, static_cast<Sint16>(std::clamp(x, -32768, 32767)));
                    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, static_cast<Sint16>(std::clamp(y, -32768, 32767)));
                    continue;
                }
            } else if (const int button = button_named(rest); button >= 0 && (verb == "tap" || verb == "down" || verb == "up")) {
                SDL_SetJoystickVirtualButton(joystick, button, verb != "up");
                // When, on the steady clock: where a reader of the screen starts the time from a press to its picture.
                if (verb != "up") {
                    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    log::debug("pretend: pressed {} at {} ns", rest, ns);
                }
                if (verb == "tap") lifts_.push_back(Lift{now_ms + 40, button});
                continue;
            }
            std::fprintf(stderr, "--pretend-taps: not understood: %s\n", line.c_str());
        }
    }

private:
    static int button_named(std::string_view name) {
        static constexpr struct { std::string_view name; SDL_GamepadButton button; } kNames[] = {
            {"up", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
            {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT}, {"south", SDL_GAMEPAD_BUTTON_SOUTH}, {"east", SDL_GAMEPAD_BUTTON_EAST},
            {"west", SDL_GAMEPAD_BUTTON_WEST}, {"north", SDL_GAMEPAD_BUTTON_NORTH}, {"left-shoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
            {"right-shoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"start", SDL_GAMEPAD_BUTTON_START},
        };
        for (const auto& one : kNames) {
            if (one.name == name) return one.button;
        }
        return -1;
    }

    struct Lift {
        std::uint64_t at = 0;
        int button = 0;
    };
    std::string file_;
    std::streamoff read_ = 0;
    std::vector<Lift> lifts_;
};

}  // namespace psvp::app
