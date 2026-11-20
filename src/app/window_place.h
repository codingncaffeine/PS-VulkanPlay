// The size the window was left at, kept between runs.
#pragma once

#include <filesystem>
#include <optional>

namespace psvp::app {

struct WindowPlace {
    static constexpr int kLeastWidth = 320, kLeastHeight = 180, kMost = 16384;
    // The window's size while it is neither maximized nor on the whole screen.
    int width = 1280;
    int height = 720;
    bool maximized = false;
    bool fullscreen = false;
    friend bool operator==(const WindowPlace&, const WindowPlace&) = default;
};

// The file it is kept in, in the state folder; nothing when there is no such folder.
[[nodiscard]] std::optional<std::filesystem::path> window_place_file();
// What the file says; the place a first run starts with when there is no file, or it says nothing usable.
[[nodiscard]] WindowPlace load_window_place(const std::filesystem::path& file);
// False when the file could not be written. The file is replaced whole or not at all.
bool save_window_place(const std::filesystem::path& file, const WindowPlace& place);

}  // namespace psvp::app
