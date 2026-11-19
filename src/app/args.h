// Command-line options.
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace psvp::app {

struct Options {
    bool show_version = false;
    bool show_help = false;
    bool verbose = false;
    // Present this many frames, then exit by itself. For automated checks.
    std::optional<std::uint64_t> frames;
};

// The arguments after the program name. The error is a sentence for the user.
[[nodiscard]] std::expected<Options, std::string> parse_args(std::span<const std::string_view> args);

[[nodiscard]] std::string help_text();

}  // namespace psvp::app
