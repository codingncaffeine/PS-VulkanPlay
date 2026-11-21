#include "app/args.h"

#include <charconv>
#include <format>

#include "psvp/version.h"

namespace psvp::app {

std::expected<Options, std::string> parse_args(std::span<const std::string_view> args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--version") {
            options.show_version = true;
        } else if (arg == "--help" || arg == "-h") {
            options.show_help = true;
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--test-clip") {
            options.test_clip = true;
        } else if (arg == "--test-clip-frame") {
            if (i + 1 >= args.size()) return std::unexpected(std::string("--test-clip-frame needs a number"));
            const std::string_view text = args[++i];
            std::uint32_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value > 65535) {
                return std::unexpected(std::format("--test-clip-frame needs a number from 0 to 65535, not '{}'", text));
            }
            options.test_clip = true;
            options.test_clip_frame = value;
        } else if (arg == "--scaler") {
            if (i + 1 >= args.size()) return std::unexpected(std::string("--scaler needs a name"));
            options.scaler = media::scaler_from_name(args[++i]);
            if (!options.scaler) return std::unexpected(std::format("--scaler takes bilinear, catmull-rom, lanczos, ewa-lanczos or ewa-lanczos-sharp, not '{}'", args[i]));
        } else if (arg == "--deband") {
            if (i + 1 >= args.size() || (args[i + 1] != "on" && args[i + 1] != "off")) return std::unexpected(std::string("--deband takes on or off"));
            options.deband = args[++i] == "on";
        } else if (arg == "--frames") {
            if (i + 1 >= args.size()) return std::unexpected(std::string("--frames needs a number"));
            const std::string_view text = args[++i];
            std::uint64_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
                return std::unexpected(std::format("--frames needs a whole number above zero, not '{}'", text));
            }
            options.frames = value;
        } else {
            return std::unexpected(std::format("unknown option '{}' (see --help)", arg));
        }
    }
    return options;
}

std::string help_text() {
    return std::format(
        "{} {}\n"
        "\n"
        "Usage: {} [options]\n"
        "       {} --play CONSOLE         a session with the app's settings, as its Play button starts one\n"
        "       {} ps-vulkanplay://play/CONSOLE   the same, from a link\n"
        "       {} COMMAND ...            one of the commands below\n"
        "\n"
        "  --version      print the version and exit\n"
        "  --help, -h     print this help and exit\n"
        "  --verbose      log more detail\n"
        "  --frames N     present N frames, then exit (for automated checks)\n"
        "  --test-clip    show a generated clip instead of a console's picture: moving blocks,\n"
        "                 color patches (a check that pictures are decoded and drawn right)\n"
        "  --test-clip-frame N   show only frame N of that clip, standing still\n"
        "  --scaler NAME, --deband on|off   how that clip is scaled and debanded (as for play)\n",
        PSVP_NAME, PSVP_VERSION, PSVP_BINARY, PSVP_BINARY, PSVP_BINARY, PSVP_BINARY);
}

}  // namespace psvp::app
