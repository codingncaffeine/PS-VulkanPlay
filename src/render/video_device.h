// The Vulkan device the program draws with, made with a video decode queue where the graphics
// card has one, so that FFmpeg can decode on it and the pictures are drawn from its memory.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "media/gpu_share.h"

namespace psvp::render {

// What decoding on the graphics card needs of the instance and the device (FFmpeg's Vulkan video).
inline constexpr std::uint32_t kVideoApi = (1U << 22) | (3U << 12);

struct DeviceRequest {
    VkInstance instance = VK_NULL_HANDLE;
    std::uint32_t instance_api = 0;
    std::vector<std::string> instance_extensions;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::uint32_t device_api = 0;
    std::uint32_t family = 0;  // the queue the program draws with
    std::string device_name;
    bool gpu_decoding = true;  // set up the video queue where the device has one
    std::vector<const char*> extensions;  // the program's own (the swapchain for a window)
};

struct MadeDevice {
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;  // the first of `family`
    // For FFmpeg, when `why_not` is empty: the device with its video queue.
    media::VulkanShare share;
    std::string why_not;
};

// A device without the video queue when the card has none (or making it with one failed): drawing
// matters more than decoding on the card. Fails only when no device at all could be made.
[[nodiscard]] std::expected<MadeDevice, std::string> make_device(const DeviceRequest& request);

}  // namespace psvp::render
