#include "render/video_device.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string_view>

#include "media/gpu_device.h"

namespace psvp::render {

namespace {

std::string version_text(std::uint32_t version) {
    return std::format("{}.{}.{}", (version >> 22) & 0x7f, (version >> 12) & 0x3ff, version & 0xfff);
}

bool has_extension(const std::vector<VkExtensionProperties>& extensions, std::string_view name) {
    return std::ranges::any_of(extensions, [&](const VkExtensionProperties& e) { return std::string_view(e.extensionName) == name; });
}

// Which decoding on the graphics card this device can do: the extensions and the queue it needs.
// Empty when it is set up; otherwise why not.
std::string plan_video(const DeviceRequest& request, std::vector<const char*>& extensions, media::VulkanShare& share,
                       std::optional<std::uint32_t>& decode_family) {
    if (!request.gpu_decoding) return "not asked for";
    if (const auto reason = media::GpuDevice::unavailable_reason()) return *reason;
    if (request.instance_api < kVideoApi || request.device_api < kVideoApi) {
        return std::format("the device or the Vulkan loader is below Vulkan {}", version_text(kVideoApi));
    }
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(request.physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(request.physical, nullptr, &count, available.data());
    for (const char* needed : {VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME}) {
        if (!has_extension(available, needed)) return std::format("the device has no {}", needed);
    }
    const bool h264 = has_extension(available, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME);
    const bool h265 = has_extension(available, VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME);
    if (!h264 && !h265) return "the device decodes neither H.264 nor HEVC";

    VkPhysicalDeviceVulkan11Features v11{};
    v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features v12{};
    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features v13{};
    v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &v11;
    v11.pNext = &v12;
    v12.pNext = &v13;
    vkGetPhysicalDeviceFeatures2(request.physical, &features);
    if (v12.timelineSemaphore != VK_TRUE || v13.synchronization2 != VK_TRUE) return "the device lacks timeline semaphores or synchronization2";

    // A queue that decodes what the console may send.
    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(request.physical, &family_count, nullptr);
    std::vector<VkQueueFamilyVideoPropertiesKHR> video(family_count);
    std::vector<VkQueueFamilyProperties2> families(family_count);
    for (std::uint32_t i = 0; i < family_count; ++i) {
        video[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        families[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        families[i].pNext = &video[i];
    }
    vkGetPhysicalDeviceQueueFamilyProperties2(request.physical, &family_count, families.data());
    VkVideoCodecOperationFlagsKHR wanted = 0;
    if (h264) wanted |= VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    if (h265) wanted |= VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
    for (std::uint32_t i = 0; i < family_count; ++i) {
        if ((families[i].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) == 0) continue;
        if ((video[i].videoCodecOperations & wanted) == 0) continue;
        decode_family = i;
        break;
    }
    if (!decode_family) return "the device has no queue that decodes H.264 or HEVC";

    extensions.push_back(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
    extensions.push_back(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
    if (h264) extensions.push_back(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME);
    if (h265) extensions.push_back(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME);
#ifdef VK_KHR_video_maintenance1  // Vulkan headers of 1.3.271 and later
    share.video_maintenance1 = has_extension(available, VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME);
    if (share.video_maintenance1) extensions.push_back(VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME);
#endif
    share.sampler_ycbcr_conversion = v11.samplerYcbcrConversion == VK_TRUE;
    share.timeline_semaphore = true;
    share.synchronization2 = true;
    share.host_query_reset = v12.hostQueryReset == VK_TRUE;

    const VkQueueFlags graphics_flags = families[request.family].queueFamilyProperties.queueFlags;
    share.families.push_back({request.family, 1, graphics_flags, video[request.family].videoCodecOperations});
    if (*decode_family != request.family) {
        share.families.push_back({*decode_family, 1, families[*decode_family].queueFamilyProperties.queueFlags, video[*decode_family].videoCodecOperations});
    }
    return {};
}

}  // namespace

std::expected<MadeDevice, std::string> make_device(const DeviceRequest& request) {
    MadeDevice made;
    media::VulkanShare& share = made.share;
    std::string& why_not = made.why_not;
    std::optional<std::uint32_t> decode_family;
    std::vector<const char*> extensions = request.extensions;
    why_not = plan_video(request, extensions, share, decode_family);
    const bool video = why_not.empty();
    if (!video) {
        decode_family.reset();
        share = media::VulkanShare{};
    }

    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queues[2]{};
    queues[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queues[0].queueFamilyIndex = request.family;
    queues[0].queueCount = 1;
    queues[0].pQueuePriorities = &priority;
    std::uint32_t queue_count = 1;
    if (video && *decode_family != request.family) {
        queues[1] = queues[0];
        queues[1].queueFamilyIndex = *decode_family;
        queue_count = 2;
    }

    // The features decoding needs, only where it is set up.
#ifdef VK_KHR_video_maintenance1
    VkPhysicalDeviceVideoMaintenance1FeaturesKHR maintenance1{};
    maintenance1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR;
    maintenance1.videoMaintenance1 = VK_TRUE;
    void* const after_v13 = share.video_maintenance1 ? &maintenance1 : request.features;
    maintenance1.pNext = request.features;
#else
    void* const after_v13 = request.features;
#endif
    VkPhysicalDeviceVulkan13Features v13{};
    v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    v13.synchronization2 = VK_TRUE;
    v13.pNext = after_v13;
    VkPhysicalDeviceVulkan12Features v12{};
    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    v12.timelineSemaphore = VK_TRUE;
    v12.hostQueryReset = share.host_query_reset ? VK_TRUE : VK_FALSE;
    v12.pNext = &v13;
    VkPhysicalDeviceVulkan11Features v11{};
    v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    v11.samplerYcbcrConversion = share.sampler_ycbcr_conversion ? VK_TRUE : VK_FALSE;
    v11.pNext = &v12;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &v11;

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = video ? &features : request.features;
    info.queueCreateInfoCount = queue_count;
    info.pQueueCreateInfos = queues;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    VkDevice& device_ = made.device;
    if (const VkResult result = vkCreateDevice(request.physical, &info, nullptr, &device_); result != VK_SUCCESS) {
        if (!video) return std::unexpected(std::format("vkCreateDevice failed with code {}", static_cast<int>(result)));
        // Without the video queue, as before: drawing matters more than decoding there.
        why_not = "the device could not be made with its video queue";
        decode_family.reset();
        share = media::VulkanShare{};
        info.pNext = request.features;
        info.queueCreateInfoCount = 1;
        extensions = request.extensions;
        info.ppEnabledExtensionNames = extensions.data();
        info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        if (const VkResult again = vkCreateDevice(request.physical, &info, nullptr, &device_); again != VK_SUCCESS) {
            return std::unexpected(std::format("vkCreateDevice failed with code {}", static_cast<int>(again)));
        }
    }
    vkGetDeviceQueue(device_, request.family, 0, &made.queue);
    if (why_not.empty()) {
        share.get_proc_addr = vkGetInstanceProcAddr;
        share.instance = request.instance;
        share.physical = request.physical;
        share.device = device_;
        share.instance_extensions = request.instance_extensions;
        share.device_extensions.assign(extensions.begin(), extensions.end());
        share.device_name = request.device_name;
    } else {
        share = media::VulkanShare{};
    }
    return made;
}

}  // namespace psvp::render
