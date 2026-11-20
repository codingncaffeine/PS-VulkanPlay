#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <unistd.h>

#include "app/paths.h"
#include "app/window_place.h"
#include "check.h"

namespace {

psvp::app::EnvLookup env_of(std::map<std::string, std::string, std::less<>> values) {
    return [values = std::move(values)](std::string_view name) -> std::optional<std::string> {
        const auto found = values.find(name);
        if (found == values.end()) return std::nullopt;
        return found->second;
    };
}

}  // namespace

PSVP_TEST(paths_follow_the_xdg_variables) {
    const auto dirs = psvp::app::resolve_dirs(env_of({{"HOME", "/home/someone"},
                                                      {"XDG_CONFIG_HOME", "/a/config"},
                                                      {"XDG_DATA_HOME", "/a/data"},
                                                      {"XDG_STATE_HOME", "/a/state"},
                                                      {"XDG_CACHE_HOME", "/a/cache"}}));
    CHECK(dirs.has_value());
    if (!dirs) return;
    CHECK(dirs->config == "/a/config/ps-vulkanplay");
    CHECK(dirs->data == "/a/data/ps-vulkanplay");
    CHECK(dirs->state == "/a/state/ps-vulkanplay");
    CHECK(dirs->cache == "/a/cache/ps-vulkanplay");
}

PSVP_TEST(paths_fall_back_to_the_home_folder) {
    const auto dirs = psvp::app::resolve_dirs(env_of({{"HOME", "/home/someone"}}));
    CHECK(dirs.has_value());
    if (!dirs) return;
    CHECK(dirs->config == "/home/someone/.config/ps-vulkanplay");
    CHECK(dirs->data == "/home/someone/.local/share/ps-vulkanplay");
    CHECK(dirs->state == "/home/someone/.local/state/ps-vulkanplay");
    CHECK(dirs->cache == "/home/someone/.cache/ps-vulkanplay");
}

// The specification: a relative or empty value is invalid and is ignored.
PSVP_TEST(paths_ignore_relative_and_empty_variables) {
    const auto dirs = psvp::app::resolve_dirs(
        env_of({{"HOME", "/home/someone"}, {"XDG_CONFIG_HOME", "relative/config"}, {"XDG_STATE_HOME", ""}}));
    CHECK(dirs.has_value());
    if (!dirs) return;
    CHECK(dirs->config == "/home/someone/.config/ps-vulkanplay");
    CHECK(dirs->state == "/home/someone/.local/state/ps-vulkanplay");
}

// With no usable HOME the account's own home folder is used, or the call fails:
// in neither case does a relative path come back.
PSVP_TEST(paths_are_never_relative) {
    for (const char* home : {"", "relative/home", "."}) {
        const auto dirs = psvp::app::resolve_dirs(env_of({{"HOME", home}}));
        if (!dirs) {
            CHECK(!dirs.error().empty());
            continue;
        }
        CHECK(dirs->config.is_absolute());
        CHECK(dirs->data.is_absolute());
        CHECK(dirs->state.is_absolute());
        CHECK(dirs->cache.is_absolute());
        CHECK(dirs->config.filename() == "ps-vulkanplay");
    }
}

PSVP_TEST(paths_use_set_variables_even_without_a_home) {
    const auto dirs = psvp::app::resolve_dirs(env_of({{"HOME", ""},
                                                      {"XDG_CONFIG_HOME", "/b/config"},
                                                      {"XDG_DATA_HOME", "/b/data"},
                                                      {"XDG_STATE_HOME", "/b/state"},
                                                      {"XDG_CACHE_HOME", "/b/cache"}}));
    CHECK(dirs.has_value());
    if (!dirs) return;
    CHECK(dirs->state == "/b/state/ps-vulkanplay");
}

// The window's size between runs (app/window_place.h).
PSVP_TEST(window_place_is_kept_and_read_back) {
    namespace fs = std::filesystem;
    using psvp::app::WindowPlace;
    const fs::path dir = fs::temp_directory_path() / ("psvp-test-place-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    const fs::path file = dir / "state" / "window.json";
    // No file yet: the size of a first run.
    CHECK(psvp::app::load_window_place(file) == WindowPlace{});
    CHECK(WindowPlace{}.width == 1280 && WindowPlace{}.height == 720 && !WindowPlace{}.maximized && !WindowPlace{}.fullscreen);

    WindowPlace left;
    left.width = 1999;
    left.height = 1331;
    left.maximized = true;
    CHECK(psvp::app::save_window_place(file, left));  // the folder is made on the way
    CHECK(psvp::app::load_window_place(file) == left);
    left.maximized = false;
    left.fullscreen = true;
    CHECK(psvp::app::save_window_place(file, left));
    CHECK(psvp::app::load_window_place(file) == left);
    CHECK(!fs::exists(dir / "state" / "window.json.new"));

    // A file that says nothing usable counts for nothing, whole.
    const auto write = [&](const char* text) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    };
    for (const char* text : {"", "not json", "[1280, 720]", R"({"width": 1280})", R"({"width": 10, "height": 720})", R"({"width": 1280.5, "height": 720})",
                             R"({"width": 1280, "height": 99999})", R"({"width": "1280", "height": 720})", R"({"width": -1280, "height": 720})"}) {
        write(text);
        CHECK(psvp::app::load_window_place(file) == WindowPlace{});
    }
    // The flags are optional, and only a true counts.
    write(R"({"width": 800, "height": 450, "maximized": "yes"})");
    WindowPlace small;
    small.width = 800;
    small.height = 450;
    CHECK(psvp::app::load_window_place(file) == small);
    fs::remove_all(dir);
}
