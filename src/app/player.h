// 'play': a session's picture in the window, its sound through the speakers,
// and a controller sent to the console.
#pragma once

#include "app/session_run.h"

namespace psvp::app {

// Runs the session of the plan with a window. Returns the process exit code.
[[nodiscard]] int run_player(SessionPlan plan, const PlayOptions& options);

}  // namespace psvp::app
