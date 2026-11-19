// Where the application keeps its files, following the XDG Base Directory
// specification: a variable counts only when it is set, not empty and absolute.
#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace psvp::app {

// Looks one environment variable up. Tests pass their own; the app passes process_env().
using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

struct Dirs {
    std::filesystem::path config;  // settings, registered consoles
    std::filesystem::path data;    // things the user would not want to lose
    std::filesystem::path state;   // logs, session reports
    std::filesystem::path cache;   // anything that can be rebuilt
};

[[nodiscard]] EnvLookup process_env();

// Fails only when no absolute home folder can be found at all; it never returns a relative path.
[[nodiscard]] std::expected<Dirs, std::string> resolve_dirs(const EnvLookup& env);

}  // namespace psvp::app
