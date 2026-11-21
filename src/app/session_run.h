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
    // The controller is a DualSense: a PS5 is told so, and then sends what only that controller
    // can do (its haptics as a second sound, effects for its triggers).
    bool dualsense = false;
    // The most loss the console is told of in one congestion report, in percent of its packets: a
    // console answers loss by lowering the bitrate and the frame rate (proto/transport.h).
    unsigned reported_loss_ceiling = proto::transport::kReportedLossCeiling;
    unsigned ack_copies = 2;              // each acknowledgment of the console's messages sent again that often
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
    // The console's view of the connection, each time it sends one (about every 1.2 s).
    std::function<void(const proto::ConnectionQuality&)> on_quality;
    // The controller as it is now; true when it changed since the last call. Asked every few milliseconds.
    std::function<bool(proto::transport::Buttons&, proto::transport::InputState&)> controller;
    // True when the console is to be asked for a frame that needs no earlier one. Asked every few milliseconds.
    std::function<bool()> wants_keyframe;
    // True ends the stream.
    std::function<bool()> stop;
    // Asked at the end: true sends the console to rest mode as `rest` does (chosen in the menu over the stream).
    std::function<bool()> rest_asked;
    // After the stream's own report lines: more of them.
    std::function<void()> on_report;
    // At the very end: a reason to call the session failed although it ran.
    std::function<std::optional<std::string>()> verdict;
    // A stream that was lost (the console went silent while it ran) ends with kSessionLost, not 1.
    bool reconnectable = false;
};

// Returns the process exit code: 0 done, 1 it did not work (the reason is printed), kSessionNotYet
// when the console is there but takes no session at this moment: asking again later can work.
inline constexpr int kSessionNotYet = 75;
// With `reconnectable`: the stream ran and was lost (the network or the console went silent); a new
// session can work.
inline constexpr int kSessionLost = 76;
[[nodiscard]] int run_session(const SessionPlan& plan);

// What 'play' needs besides the plan.
struct PlayOptions {
    std::optional<std::uint64_t> frames;  // close the window after this many frames (for automated checks)
    // What the console is told the controller is: a DualSense when one is connected as the
    // session starts, unless the command line says which.
    enum class Controller { by_what_is_connected, dualsense, dualshock4 };
    Controller controller = Controller::by_what_is_connected;
    // Show pictures evenly, a frame's time apart, by holding one back for as long as frames were
    // lately seen to arrive unevenly (media/frame_pacer.h). Off: every picture the moment it is there.
    bool pacing = true;
    // Delay kept on purpose, in milliseconds, so that frames which come that much late are still shown in place.
    int cushion_ms = 0;
    // After the connection was lost, how long new sessions are tried for; 0: none is.
    int reconnect_seconds = 60;
    // Where pictures are decoded: on the graphics card where it can (after a check of its first
    // pictures against software), or in software only.
    enum class Decoder { automatic, software };
    Decoder decoder = Decoder::automatic;
    // For checks: a pretended controller ("054c:0df2") in place of this machine's, and a file whose lines press it.
    std::optional<std::string> pretend, pretend_taps;
    // How strong a DualSense's haptics are: the volume of the two channels of its sound device
    // that move its actuators, in percent.
    int haptics_volume = 100;
    // What the two back buttons of a controller that has them (a DualSense Edge) are to the
    // console: one of its buttons each, or nothing.
    std::optional<proto::transport::Button> back_left, back_right;
    // The statistics over the picture from the start (F3 shows and hides them at any time).
    bool stats = false;
    // How bright an HDR picture is taken to be at its brightest, in cd/m² (the console's streams do
    // not say): what is brighter than SDR white is fitted between it and that (media/tone_map.h).
    int hdr_peak_nits = 1000;
    // With --wake: while the connection is made again, a console that answers as resting is woken,
    // waiting at most the time given; `on_waking` is called once it is known to rest. Returns 0 when
    // it is on (or did not answer at all).
    std::function<int(std::chrono::seconds, const std::function<void()>& on_waking)> wake_resting;
};
// The part of 'play' that has a window: the program that has one sets it before it runs a command.
using PlayRunner = int (*)(SessionPlan plan, const PlayOptions& options);
void set_play_runner(PlayRunner runner);

// The 'controllers' command reads the controllers through the input library, which only the
// program with the window is built with: it hands the command in here (app/controllers.h).
struct ControllersOptions;
using ControllersRunner = int (*)(const ControllersOptions& options);
void set_controllers_runner(ControllersRunner runner);
// What this computer offers (the program's own part: it makes a Vulkan device): `diagnose [--json]`.
using DiagnoseRunner = int (*)(bool json);
void set_diagnose_runner(DiagnoseRunner runner);

}  // namespace psvp::app
