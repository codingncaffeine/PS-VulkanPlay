// One session with a console, from the request to the goodbye, as the 'session'
// and 'play' commands run it. It prints what happens to the terminal; whoever
// runs it gets the stream's frames and supplies the controller through hooks.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "proto/console.h"
#include "proto/input.h"
#include "proto/stream_message.h"
#include "proto/stream_setup.h"

namespace psvp::app {

struct SessionPlan {
    std::string console;                  // its name or hardware address, as registered
    std::optional<std::string> host;      // instead of the address it was last seen at
    std::optional<std::string> passcode;
    bool rest = false;                    // send the console to rest mode at the end
    proto::transport::VideoProfile video;
    bool invented_id = false;             // name the stream with an invented session ID, not the console's
    std::chrono::milliseconds hold{3000}; // how long the stream is held, unless `stop` ends it sooner

    // What the controller does at set times: for the commands' own button presses.
    struct Action {
        std::chrono::milliseconds at{0};
        proto::transport::Button button = proto::transport::Button::cross;
        bool down = false;
    };
    std::vector<Action> actions;

    // The hooks, each optional, all called on the thread that runs the session.
    // Every datagram from the console's stream port, with the time since the stream connection began.
    std::function<void(proto::ConsoleKind, std::span<const std::uint8_t>, std::chrono::microseconds)> on_packet;
    // The frames that are ready, as often as there are any; the stream information once it arrived.
    std::function<void(proto::transport::MediaOutput&, const std::optional<proto::StreamInfo>&)> on_media;
    std::function<void(const std::vector<proto::transport::ConsoleData>&, std::chrono::microseconds)> on_console_data;
    // The controller as it is now; true when it changed since the last call. Asked every few milliseconds.
    std::function<bool(proto::transport::Buttons&, proto::transport::InputState&)> controller;
    // True ends the stream.
    std::function<bool()> stop;
    // After the stream's own report lines: more of them.
    std::function<void()> on_report;
    // At the very end: a reason to call the session failed although it ran.
    std::function<std::optional<std::string>()> verdict;
};

// Returns the process exit code: 0 done, 1 it did not work (the reason is printed).
[[nodiscard]] int run_session(const SessionPlan& plan);

// What 'play' needs besides the plan.
struct PlayOptions {
    std::optional<std::uint64_t> frames;  // close the window after this many frames (for automated checks)
};
// The part of 'play' that has a window: the program that has one sets it before it runs a command.
using PlayRunner = int (*)(SessionPlan plan, const PlayOptions& options);
void set_play_runner(PlayRunner runner);

}  // namespace psvp::app
