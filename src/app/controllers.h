// The 'controllers' command: which controllers are connected, read through the same input
// library the stream uses (so what is listed here is what a session will find).
#pragma once

#include <optional>
#include <string>

namespace psvp::app {

struct ControllersOptions {
    bool json = false;
    // Go on, and say it again each time something changes: a controller comes or goes, or its
    // battery level changes. Ends when this program is told to end, or its parent has gone.
    bool watch = false;
    // For checks: a controller that exists only inside this program, by its USB vendor and
    // product numbers ("054c:0df2"). The machine's own controllers are then not looked at.
    std::optional<std::string> pretend;
    // With watch: also say what is pressed on a controller, as what it means to an app that is
    // moved through with it (a direction, accept, back, the page before or after).
    bool presses = false;
    // For checks, with pretend: a file whose lines press the pretended controller's buttons.
    std::optional<std::string> pretend_taps;
};

[[nodiscard]] int run_controllers(const ControllersOptions& options);

}  // namespace psvp::app
