// What this computer offers the stream program: its graphics devices, whether pictures can be
// decoded on the card (and why not), the libraries' versions, and the last session's report.
// For the app's diagnostics page and for bug reports.
#pragma once

namespace psvp::app {

// Prints it (as one line of JSON with `json`). Opens no window; it makes a Vulkan device for a
// moment to ask whether the card decodes. Exit 0, or 1 when Vulkan cannot be started at all.
[[nodiscard]] int run_diagnose(bool json);

}  // namespace psvp::app
