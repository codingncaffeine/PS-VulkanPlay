// The application window and its Vulkan swapchain. It shows the pictures it is
// handed, as large as they fit, and the background color while there are none.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "app/stream_menu.h"
#include "app/window_place.h"
#include "media/gpu_device.h"
#include "media/video_decoder.h"

union SDL_Event;

namespace psvp::render {

// The window background, as the sRGB bytes that reach the screen.
inline constexpr std::array<std::uint8_t, 3> kBackground{0x14, 0x17, 0x1c};

struct WindowOptions {
    // Present this many frames, then close. Not set: run until the window is closed.
    std::optional<std::uint64_t> frame_limit;
    // Show a dialog when the window cannot be opened. Off for automated runs.
    bool dialogs = true;
    // The pictures to show. Asked on the window's thread before it draws: the newest picture
    // that was not handed over before, or nothing. It must not wait for anything.
    std::function<std::optional<media::Picture>()> next_picture;
    // Every event of the window system, as it comes, on the window's thread: for the controller and the keys.
    std::function<void(const SDL_Event&)> on_event;
    // Asked as often: true closes the window.
    std::function<bool()> finished;
    // The size the window opens at, and whether maximized or on the whole screen.
    app::WindowPlace place;
    // Told once, when the window closes: how it was left, to open the next one the same way.
    std::function<void(const app::WindowPlace&)> on_place;
    // With a picture source the window draws when there is a new picture or the window
    // changed, and otherwise waits this long at most before it asks again.
    std::chrono::milliseconds ask_every{100};
    // Lines drawn over the picture in a panel at the top left (the statistics), asked before every
    // frame on the window's thread; none: no panel. It must not wait for anything.
    std::function<std::vector<std::string>()> overlay_lines;
    // The menu over the stream while it is open, asked the same way; nothing: none is drawn.
    std::function<std::optional<app::MenuView>()> overlay_menu;
    // A notice at the top (the connection was lost, and is being made again); empty: none.
    std::function<std::string()> overlay_notice;
    // How bright an HDR picture is taken to be at its brightest, in cd/m² (VideoPass::set_hdr_peak).
    double hdr_peak_nits = 1000.0;
    // Lend the window's Vulkan device to FFmpeg for decoding on the graphics card, where the device
    // can (a video decode queue for H.264 or HEVC, Vulkan 1.3).
    bool gpu_decoding = true;
    // Told once, on the window's thread, when its device is set up: the device to decode on, or
    // nothing and why not. Not told when the window cannot be opened.
    std::function<void(std::optional<media::GpuDevice>, const std::string& why_not)> on_gpu;
    // Told before the device goes, after `on_gpu` gave one: every decoder opened on it and every
    // picture decoded on it must be gone when this returns.
    std::function<void()> before_device_gone;
};

// From any thread: there is a new picture, or the answer of `finished` changed. The window
// asks at once instead of at the end of its wait.
void wake_window();

// Opens the window and runs it until it is closed. Returns the process exit code:
// 0 closed normally, 3 no usable window system or Vulkan loader, 4 no usable
// Vulkan device, 5 a Vulkan call failed while running.
[[nodiscard]] int run_window(const WindowOptions& options);

}  // namespace psvp::render
