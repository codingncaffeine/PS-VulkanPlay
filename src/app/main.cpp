#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/args.h"
#include "app/commands.h"
#include "app/controllers.h"
#include "app/log.h"
#include "app/paths.h"
#include "app/player.h"
#include "app/window_place.h"
#include "media/test_clip.h"
#include "media/video_decoder.h"
#include "psvp/version.h"
#include "render/vk_window.h"

#if defined(__linux__)
#include <unistd.h>
#endif

namespace {

constexpr int kExitUsage = 64;
constexpr int kExitInternal = 70;
constexpr int kExitNoDecoder = 6;  // beside the window's own codes (render/vk_window.h)

std::string c_library() {
#if defined(__GLIBC__)
    return std::format("glibc {}.{}", __GLIBC__, __GLIBC_MINOR__);
#elif defined(__linux__)
    return "musl";
#else
    return "another C library";
#endif
}

std::string cxx_library() {
#if defined(_LIBCPP_VERSION)
    return std::format("libc++ {}", _LIBCPP_VERSION);
#elif defined(__GLIBCXX__)
    return std::format("libstdc++ {}", __GLIBCXX__);
#else
    return "another C++ library";
#endif
}

std::string compiler() {
#if defined(__clang__)
    return std::format("clang {}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    return std::format("gcc {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    return "another compiler";
#endif
}

// Started with nothing to do, by a person: what they want is the app, which is its own program
// (the windows around the stream: consoles, settings, setting up). It is looked for where a
// build and a package put it, and takes this process over; it is told where this program is, to
// run it for everything a console is asked. Returns when there is no app here.
void hand_over_to_the_app() {
#if defined(__linux__)
    std::error_code failed;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", failed);
    if (failed) return;
    const std::filesystem::path beside = self.parent_path();
    for (const std::filesystem::path& app : {beside / "app" / "PSVulkanPlay", beside.parent_path() / "lib" / PSVP_BINARY / "app" / "PSVulkanPlay"}) {
        if (::access(app.c_str(), X_OK) != 0) continue;
        ::setenv("PSVP_NATIVE", self.c_str(), 1);
        std::string path = app.string();
        char* const arguments[] = {path.data(), nullptr};
        ::execv(path.c_str(), arguments);
        std::fprintf(stderr, "%s: the app at %s could not be started; opening the stream window alone\n", PSVP_BINARY, path.c_str());
    }
#endif
}

int run(int argc, char** argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    // What is printed shows up line by line also when it goes into a file or another program,
    // in step with the log lines beside it.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // A command does its work in the terminal and opens neither the log nor a window.
    psvp::app::set_play_runner(&psvp::app::run_player);
    psvp::app::set_controllers_runner(&psvp::app::run_controllers);
    if (psvp::app::is_command(args)) return psvp::app::run_command(args);
    if (args.empty()) hand_over_to_the_app();

    const auto options = psvp::app::parse_args(args);
    if (!options) {
        std::fprintf(stderr, "%s: %s\n", PSVP_BINARY, options.error().c_str());
        return kExitUsage;
    }
    // These two answer before anything is opened or written.
    if (options->show_version) {
        std::printf("%s %s\n", PSVP_BINARY, PSVP_VERSION);
        return 0;
    }
    if (options->show_help) {
        std::fputs((psvp::app::help_text() + "\n" + psvp::app::commands_help()).c_str(), stdout);
        return 0;
    }

    if (options->verbose) psvp::log::set_level(psvp::log::Level::debug);
    const auto dirs = psvp::app::resolve_dirs(psvp::app::process_env());
    if (dirs) {
        const auto file = dirs->state / "logs" / (std::string(PSVP_BINARY) + ".log");
        if (!psvp::log::open_file(file)) psvp::log::warn("log: cannot write {}", file.string());
    } else {
        psvp::log::warn("log: {}", dirs.error());
    }
    psvp::log::info("{} {} starting ({} build, {}, {}, {})", PSVP_BINARY, PSVP_VERSION, PSVP_BUILD_TYPE,
                    compiler(), cxx_library(), c_library());

    psvp::render::WindowOptions window;
    window.frame_limit = options->frames;
    window.dialogs = !options->frames.has_value();
    if (const auto file = psvp::app::window_place_file()) {
        window.place = psvp::app::load_window_place(*file);
        window.on_place = [file = *file](const psvp::app::WindowPlace& place) {
            if (!psvp::app::save_window_place(file, place)) psvp::log::warn("window: its size could not be kept in {}", file.string());
        };
    }
    if (options->test_clip) {
        // The generated clip through the decoder, a new frame sixty times a second.
        struct Clip {
            psvp::media::ClipWriter writer{416, 240};
            std::optional<psvp::media::VideoDecoder> decoder;
            std::uint32_t counter = 0;
            std::chrono::steady_clock::time_point next;
        };
        auto clip = std::make_shared<Clip>();
        auto decoder = psvp::media::VideoDecoder::open(psvp::media::VideoCodec::h264, clip->writer.parameter_sets());
        if (!decoder) {
            psvp::log::error("test clip: {}", decoder.error());
            psvp::log::close_file();
            return kExitNoDecoder;
        }
        clip->decoder.emplace(std::move(*decoder));
        psvp::log::info("test clip: {}", clip->decoder->description());
        if (options->test_clip_frame) {
            // One frame, handed over once: it stays on the screen.
            window.next_picture = [clip, frame = *options->test_clip_frame, shown = false]() mutable -> std::optional<psvp::media::Picture> {
                if (std::exchange(shown, true)) return std::nullopt;
                return clip->decoder->decode(clip->writer.frame(frame, true));
            };
        } else {
            window.next_picture = [clip]() -> std::optional<psvp::media::Picture> {
                const auto now = std::chrono::steady_clock::now();
                if (now < clip->next) return std::nullopt;
                clip->next = now + std::chrono::microseconds(16667);
                return clip->decoder->decode(clip->writer.frame(clip->counter++, false));
            };
            window.ask_every = std::chrono::milliseconds(2);
        }
    }
    const int code = psvp::render::run_window(window);
    psvp::log::info("{} exiting with code {}", PSVP_BINARY, code);
    psvp::log::close_file();
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: stopped by an internal error: %s\n", PSVP_BINARY, error.what());
        return kExitInternal;
    }
}
