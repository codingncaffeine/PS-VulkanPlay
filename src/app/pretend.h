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
// Gamepads with the vendor and product numbers "054c:0df2", several split by commas
// ("054c:0df2,045e:0b13"); false when the text is not that. What the program asks of each is said
// in the log: "pretend: ..." for the first, "pretend 2: ..." for the second and so on.
[[nodiscard]] bool attach_pretended(std::string_view text);

// For checks: presses the buttons of the pretended controller as a file says, line by line as
// the lines are added to it. "tap NAME" (down, and up a moment later), "down NAME", "up NAME",
// "stick X Y" and "right-stick X Y" (each from -32768 to 32767), "touch X Y" (the first finger
// down on the touchpad, each from 0 to 1), "lift" (and up again), "gyro X Y Z" (a reading of the
// motion sensor, radians a second). A line for another pretended controller than the first starts
// with its number: "2: tap south".
class Taps {
public:
    explicit Taps(std::string file) : file_(std::move(file)) {}

    // The pretended controllers, in the order they were made (they must be open: the player opens
    // every controller it sees).
    static SDL_Joystick* pretended(std::size_t number) {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetJoysticks(&count);
        SDL_Joystick* found = nullptr;
        std::size_t seen = 0;
        for (int i = 0; ids != nullptr && i < count && found == nullptr; ++i) {
            if (SDL_IsJoystickVirtual(ids[i]) && ++seen == number) found = SDL_GetJoystickFromID(ids[i]);
        }
        SDL_free(ids);
        return found;
    }

    void follow(std::uint64_t now_ms) {
        bool lifted = false;
        for (auto due = lifts_.begin(); due != lifts_.end();) {
            if (due->at > now_ms) {
                ++due;
                continue;
            }
            if (SDL_Joystick* joystick = pretended(due->pad)) SDL_SetJoystickVirtualButton(joystick, due->button, false);
            due = lifts_.erase(due);
            lifted = true;
        }
        // A tap is whole before the next line is read, and its lift is seen by the window's loop on its
        // own: lines read sooner would put a button down again before it was seen to come up.
        if (lifted || !lifts_.empty()) return;
        std::ifstream in(file_, std::ios::binary);
        if (!in) return;
        in.seekg(read_);
        std::string line;
        // A line counts when it has its end: one that is still being written is read the next time.
        while (std::getline(in, line) && !in.eof()) {
            const std::streamoff line_start = read_;
            read_ = in.tellg();
            std::size_t pad = 1;
            if (line.size() > 3 && line[0] >= '1' && line[0] <= '9' && line[1] == ':' && line[2] == ' ') {
                pad = static_cast<std::size_t>(line[0] - '0');
                line.erase(0, 3);
            }
            // A controller that is not open yet: the line waits for it.
            SDL_Joystick* joystick = pretended(pad);
            if (joystick == nullptr) {
                read_ = line_start;
                break;
            }
            const auto space = line.find(' ');
            const std::string verb = line.substr(0, space), rest = space == std::string::npos ? "" : line.substr(space + 1);
            if (verb == "stick" || verb == "right-stick") {
                int x = 0, y = 0;
                if (std::sscanf(rest.c_str(), "%d %d", &x, &y) == 2) {
                    const bool right = verb == "right-stick";
                    SDL_SetJoystickVirtualAxis(joystick, right ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX, static_cast<Sint16>(std::clamp(x, -32768, 32767)));
                    SDL_SetJoystickVirtualAxis(joystick, right ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY, static_cast<Sint16>(std::clamp(y, -32768, 32767)));
                    continue;
                }
            } else if (verb == "touch" || verb == "lift") {
                float x = 0.5F, y = 0.5F;
                if (verb == "lift" || std::sscanf(rest.c_str(), "%f %f", &x, &y) == 2) {
                    SDL_SetJoystickVirtualTouchpad(joystick, 0, 0, verb == "touch", std::clamp(x, 0.0F, 1.0F), std::clamp(y, 0.0F, 1.0F), verb == "touch" ? 1.0F : 0.0F);
                    continue;
                }
            } else if (verb == "gyro") {
                float data[3] = {};
                if (std::sscanf(rest.c_str(), "%f %f %f", &data[0], &data[1], &data[2]) == 3) {
                    SDL_SendJoystickVirtualSensorData(joystick, SDL_SENSOR_GYRO, SDL_GetTicksNS(), data, 3);
                    continue;
                }
            } else if (const int button = button_named(rest); button >= 0 && (verb == "tap" || verb == "down" || verb == "up")) {
                SDL_SetJoystickVirtualButton(joystick, button, verb != "up");
                // When, on the steady clock: where a reader of the screen starts the time from a press to its picture.
                if (verb != "up") {
                    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    log::debug("pretend: pressed {} at {} ns", rest, ns);
                }
                if (verb == "tap") {
                    lifts_.push_back(Lift{now_ms + 40, button, pad});
                    break;  // the rest waits for its lift (above)
                }
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
            {"right-shoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"start", SDL_GAMEPAD_BUTTON_START}, {"back", SDL_GAMEPAD_BUTTON_BACK},
            {"left-paddle1", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1}, {"right-paddle1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1},
            {"left-paddle2", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2}, {"right-paddle2", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2},
        };
        for (const auto& one : kNames) {
            if (one.name == name) return one.button;
        }
        return -1;
    }

    struct Lift {
        std::uint64_t at = 0;
        int button = 0;
        std::size_t pad = 1;
    };
    std::string file_;
    std::streamoff read_ = 0;
    std::vector<Lift> lifts_;
};

}  // namespace psvp::app
