// The 'chart' command's window: the test picture (media/test_chart.h) drawn as a stream is.
#pragma once

#include "app/session_run.h"

namespace psvp::app {

// Opens the window with the picture until it is closed (Esc, or the window's close button).
// The exit codes of render::run_window.
[[nodiscard]] int run_chart(const ChartOptions& options);

}  // namespace psvp::app
