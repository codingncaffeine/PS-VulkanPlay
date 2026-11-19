// The application window and its Vulkan swapchain. For now it shows the
// background color and nothing else; the video and UI passes are built on it.
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace psvp::render {

// The window background, as the sRGB bytes that reach the screen.
inline constexpr std::array<std::uint8_t, 3> kBackground{0x14, 0x17, 0x1c};

struct WindowOptions {
    // Present this many frames, then close. Not set: run until the window is closed.
    std::optional<std::uint64_t> frame_limit;
    // Show a dialog when the window cannot be opened. Off for automated runs.
    bool dialogs = true;
};

// Opens the window and runs it until it is closed. Returns the process exit code:
// 0 closed normally, 3 no usable window system or Vulkan loader, 4 no usable
// Vulkan device, 5 a Vulkan call failed while running.
[[nodiscard]] int run_window(const WindowOptions& options);

}  // namespace psvp::render
