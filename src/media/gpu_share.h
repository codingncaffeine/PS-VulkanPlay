// The stream window's side of decoding on the graphics card: lending its own Vulkan device to
// FFmpeg, and drawing the decoded images straight from that device's memory.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "media/gpu_device.h"

namespace psvp::media {

struct VulkanQueueFamily {
    std::uint32_t index = 0;
    std::uint32_t count = 1;
    VkQueueFlags flags = 0;
    VkVideoCodecOperationFlagsKHR video = 0;  // for a video queue: what it decodes
};

// A device made by the window. The window keeps it alive until every GpuDevice made from it,
// every decoder opened on it and every picture decoded on it is gone.
struct VulkanShare {
    PFN_vkGetInstanceProcAddr get_proc_addr = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    // The features enabled when the device was made (only these four matter to FFmpeg's decoders).
    bool sampler_ycbcr_conversion = false;
    bool timeline_semaphore = false;
    bool synchronization2 = false;
    bool host_query_reset = false;
    bool video_maintenance1 = false;
    // In the order they are to be used: the window's own queue first, then the video queue.
    std::vector<VulkanQueueFamily> families;
    std::string device_name;
};

[[nodiscard]] std::expected<GpuDevice, std::string> share_device(const VulkanShare& share);

// What the window needs to draw a picture decoded on its device. The picture's frame stays
// locked from `begin_use` to `finish_use`: lock just before the submission, unlock right after.
struct GpuImageUse {
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAccessFlags2 access = 0;
    VkSemaphore semaphore = VK_NULL_HANDLE;  // a timeline: wait for `wait_value`, signal `wait_value + 1`
    std::uint64_t wait_value = 0;
    std::uint32_t family = VK_QUEUE_FAMILY_IGNORED;
    // Of the planes: 8 or 16 bits a sample (samples of 10 bits sit in the high bits of 16).
    bool wide = false;
    // The image's own size: a decoder's images may be larger than the picture (1088 rows for 1080).
    std::uint32_t image_width = 0, image_height = 0;
};
// Nothing when the picture is not one decoded on a graphics card in a single image of two planes.
[[nodiscard]] std::expected<GpuImageUse, std::string> begin_use(const Picture& picture);
// `submitted`: the work that waits for the frame went to the queue, signalling the next value,
// and leaves the image in `layout` after `access`.
void finish_use(const Picture& picture, bool submitted, VkImageLayout layout, VkAccessFlags2 access);

}  // namespace psvp::media
