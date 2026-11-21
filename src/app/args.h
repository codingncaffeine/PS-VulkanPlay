// Command-line options.
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "media/scaler.h"

namespace psvp::app {

struct Options {
    bool show_version = false;
    bool show_help = false;
    bool verbose = false;
    // Present this many frames, then exit by itself. For automated checks.
    std::optional<std::uint64_t> frames;
    // Show the generated clip instead of a console's picture: a check of the decoder and of the drawing.
    bool test_clip = false;
    // One frame of that clip, standing still: for a check that reads the colors off the screen.
    std::optional<std::uint32_t> test_clip_frame;
    // How that picture is scaled and whether it is debanded (as play's --scaler and --deband).
    std::optional<media::Scaler> scaler;
    std::optional<bool> deband;
};

// The arguments after the program name. The error is a sentence for the user.
[[nodiscard]] std::expected<Options, std::string> parse_args(std::span<const std::string_view> args);

[[nodiscard]] std::string help_text();

}  // namespace psvp::app
