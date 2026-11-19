// The command-line commands: finding consoles, registering with one, waking
// one, and managing the list of registered consoles. They print to the terminal
// and open no window.
#pragma once

#include <span>
#include <string>
#include <string_view>

namespace psvp::app {

// True when the first argument names a command rather than an option.
[[nodiscard]] bool is_command(std::span<const std::string_view> args);

// Runs the command and returns the process exit code: 0 done, 1 it did not
// work (the reason is printed), 64 the command line was not understood.
[[nodiscard]] int run_command(std::span<const std::string_view> args);

[[nodiscard]] std::string commands_help();

}  // namespace psvp::app
