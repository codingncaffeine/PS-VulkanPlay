#include "render/vk_window.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include "app/log.h"
#include "media/gpu_share.h"
#include "media/present_clock.h"
#include "psvp/version.h"
#include "render/overlay_pass.h"
#include "render/video_device.h"
#include "render/video_pass.h"

namespace psvp::render {
namespace {

constexpr int kExitNoWindowSystem = 3;
constexpr int kExitNoDevice = 4;
constexpr int kExitVulkanFailed = 5;

constexpr std::uint32_t make_version(std::uint32_t major, std::uint32_t minor, std::uint32_t patch) {
    return (major << 22) | (minor << 12) | patch;
}
constexpr std::uint32_t kMinimumApi = make_version(1, 2, 0);


std::string version_text(std::uint32_t version) {
    return std::format("{}.{}.{}", (version >> 22) & 0x7f, (version >> 12) & 0x3ff, version & 0xfff);
}

std::string_view result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "another VkResult";
    }
}

std::string_view device_type_name(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "software";
        default: return "other";
    }
}

int device_rank(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
        default: return 0;
    }
}

float srgb_to_linear(float encoded) {
    return encoded <= 0.04045F ? encoded / 12.92F : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

bool is_srgb_format(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
}

class Window {
public:
    Window() = default;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window() { destroy(); }

    int run(const WindowOptions& options);

private:
    bool check(VkResult result, std::string_view what);
    int open(const WindowOptions& options);
    bool create_instance();
    bool pick_device();
    // Says in the log whether the device and this window's surface can tell when each picture reached
    // the screen (VK_EXT_present_timing), and asks for it where both can (PLAN D37).
    void offer_present_timing();
    // With present timing: after the swapchain was made, its queue of reports, time domain and
    // refresh; after every present, the reports that came. A call that fails turns it off for the
    // rest of the session (the swapchain is made again without it).
    void start_present_timing();
    void read_present_timing();
    void give_up_present_timing(std::string_view why);
    bool create_device();
    void share_with_decoder();
    bool create_frame_objects();
    bool create_swapchain();
    void destroy_swapchain();
    void destroy();
    // Returns false on a failed call; `presented` says whether a frame reached the swapchain.
    // `fresh` is a picture to show from this frame on.
    bool draw_frame(bool& presented, const media::Picture* fresh);
    // What lies over the picture is not what was drawn last: a frame is due without a new picture
    // (the menu opened, a notice came, while no picture arrives).
    [[nodiscard]] bool overlay_due() const;
    void note_size();
    app::WindowPlace place_;

    SDL_Window* window_ = nullptr;
    bool sdl_started_ = false;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    std::string device_name_;
    std::uint32_t device_api_ = 0;
    std::uint32_t instance_api_ = 0;
    std::vector<std::string> instance_extensions_;
    bool surface_capabilities2_ = false;  // VK_KHR_get_surface_capabilities2 is enabled
    bool software_device_ = false;        // the device draws on the processor (llvmpipe and the like)
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;

    // Decoding on the graphics card: the video queue the device was made with, and FFmpeg's view of it.
    media::VulkanShare share_;
    std::string gpu_why_not_;
    std::optional<media::GpuDevice> gpu_;
    void lock_queue() const {
        if (gpu_) gpu_->lock_queue(queue_family_, 0);
    }
    void unlock_queue() const {
        if (gpu_) gpu_->unlock_queue(queue_family_, 0);
    }

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkSemaphore image_available_ = VK_NULL_HANDLE;
    VkFence frame_done_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> render_done_;  // one per swapchain image
    bool swapchain_stale_ = true;
    const char* present_mode_name_ = "";

    // When pictures reached the screen, as the driver says (VK_EXT_present_timing).
    struct PresentTiming {
        bool wanted = false;    // the device and the surface offer it: asked for when the device is made
        bool enabled = false;   // the device was made with it
        bool on = false;        // the swapchain is made with it
        bool given_up = false;  // a call failed: off for the rest of the session
        std::uint32_t stages = 0;           // what the surface reports (VkPresentStageFlagsEXT)
        std::uint64_t domain = 0;           // the swapchain's time domain asked for
        std::uint64_t properties_seen = 0;  // the swapchain's timing properties' counter when read last
        std::uint64_t next_id = 1;
        PFN_vkVoidFunction set_queue_size = nullptr, timing_properties = nullptr, domain_properties = nullptr, past_timing = nullptr;
    } timing_;
    media::PresentClock present_clock_;

    VideoPass video_;
    bool picture_refused_ = false;  // said once
    OverlayPass overlay_;
    const WindowOptions* options_ = nullptr;
    std::vector<std::string> overlay_lines_;  // as laid out last
    std::optional<app::MenuView> overlay_menu_;
    std::string overlay_notice_;
    VkExtent2D overlay_extent_{};             // the size they were laid out for
};

// The event that wakes the window's wait; 0 until a window registered it.
std::atomic<std::uint32_t> wake_event{0};

bool Window::check(VkResult result, std::string_view what) {
    if (result == VK_SUCCESS) return true;
    log::error("render: {} failed: {}", what, result_name(result));
    return false;
}

bool Window::create_instance() {
    std::uint32_t loader_version = make_version(1, 0, 0);
    vkEnumerateInstanceVersion(&loader_version);

    std::uint32_t extension_count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (extensions == nullptr) {
        log::error("render: the window system offers no Vulkan surface: {}", SDL_GetError());
        return false;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = PSVP_NAME;
    app.applicationVersion = make_version(0, 0, 1);
    app.pEngineName = PSVP_NAME;
    // The newest version this program uses: 1.3 for decoding on the graphics card. A device of 1.2
    // still draws (it is only not lent to the decoder).
    instance_api_ = loader_version >= kVideoApi ? kVideoApi : kMinimumApi;
    app.apiVersion = instance_api_;
    std::vector<const char*> enabled(extensions, extensions + extension_count);
    // Asking a surface what it can do beyond the basics (whether it says when pictures reached the
    // screen) takes the second form of the surface query, where the loader offers it.
    std::uint32_t offered_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &offered_count, nullptr);
    std::vector<VkExtensionProperties> offered(offered_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &offered_count, offered.data());
    const std::string_view capabilities2 = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
    surface_capabilities2_ = std::ranges::any_of(offered, [&](const VkExtensionProperties& e) { return e.extensionName == capabilities2; });
    if (surface_capabilities2_ && std::ranges::none_of(enabled, [&](const char* name) { return name == capabilities2; })) {
        enabled.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    }
    for (const char* name : enabled) instance_extensions_.emplace_back(name);

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    info.ppEnabledExtensionNames = enabled.data();
    if (!check(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance")) return false;
    log::info("render: Vulkan loader {}", version_text(loader_version));
    return true;
}

bool Window::pick_device() {
    std::uint32_t count = 0;
    if (!check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices")) return false;
    std::vector<VkPhysicalDevice> devices(count);
    if (count > 0) {
        const VkResult result = vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) return check(result, "vkEnumeratePhysicalDevices");
        devices.resize(count);
    }

    int best_rank = -1;
    for (const VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        const std::string name = properties.deviceName;
        const std::string_view kind = device_type_name(properties.deviceType);
        if (properties.apiVersion < kMinimumApi) {
            log::info("render: skipping {} ({}): Vulkan {} is below {}", name, kind,
                      version_text(properties.apiVersion), version_text(kMinimumApi));
            continue;
        }

        std::uint32_t extension_count = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, nullptr);
        std::vector<VkExtensionProperties> extensions(extension_count);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, extensions.data());
        const bool has_swapchain = std::ranges::any_of(extensions, [](const VkExtensionProperties& e) {
            return std::string_view(e.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        });
        if (!has_swapchain) {
            log::info("render: skipping {} ({}): it cannot present", name, kind);
            continue;
        }

        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        std::optional<std::uint32_t> family;
        for (std::uint32_t i = 0; i < family_count; ++i) {
            VkBool32 can_present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &can_present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && can_present == VK_TRUE) {
                family = i;
                break;
            }
        }
        if (!family) {
            log::info("render: skipping {} ({}): it cannot draw to this window", name, kind);
            continue;
        }

        log::info("render: found {} ({}), Vulkan {}", name, kind, version_text(properties.apiVersion));
        const int rank = device_rank(properties.deviceType);
        if (rank > best_rank) {
            best_rank = rank;
            physical_ = candidate;
            queue_family_ = *family;
            device_api_ = properties.apiVersion;
            device_name_ = std::format("{} ({})", name, kind);
            software_device_ = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        }
    }
    if (physical_ == VK_NULL_HANDLE) {
        log::error("render: no usable Vulkan device ({} found, none can draw to this window)", devices.size());
        return false;
    }
    return true;
}

void Window::offer_present_timing() {
#ifdef VK_EXT_present_timing
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, extensions.data());
    const auto has = [&](std::string_view name) {
        return std::ranges::any_of(extensions, [&](const VkExtensionProperties& e) { return e.extensionName == name; });
    };
    if (!has(VK_EXT_PRESENT_TIMING_EXTENSION_NAME)) {
        log::info("render: present timing: the device does not offer it");
        return;
    }
    const auto query = surface_capabilities2_ ? reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
                                                    vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"))
                                              : nullptr;
    if (query == nullptr) {
        log::info("render: present timing: the device offers it, but the surface cannot be asked (no {})", VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        return;
    }
    VkPresentTimingSurfaceCapabilitiesEXT timing{};
    timing.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT;
    VkSurfaceCapabilities2KHR capabilities{};
    capabilities.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    capabilities.pNext = &timing;
    VkPhysicalDeviceSurfaceInfo2KHR surface{};
    surface.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surface.surface = surface_;
    if (const VkResult result = query(physical_, &surface, &capabilities); result != VK_SUCCESS) {
        log::info("render: present timing: the device offers it; asking the surface failed with code {}", static_cast<int>(result));
        return;
    }
    if (timing.presentTimingSupported != VK_TRUE) {
        log::info("render: present timing: the device offers it, this window's surface does not");
        return;
    }
    std::string stages;
    const auto stage = [&](VkPresentStageFlagsEXT bit, std::string_view name) {
        if ((timing.presentStageQueries & bit) == 0) return;
        if (!stages.empty()) stages += ", ";
        stages += name;
    };
    stage(VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT, "queue done");
    stage(VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT, "taken by the presentation engine");
    stage(VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT, "first pixel out");
    stage(VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT, "first pixel visible");
    log::info("render: present timing: offered by the device and this window's surface; at an absolute time: {}, at a relative time: {}; "
              "it reports: {}",
              timing.presentAtAbsoluteTimeSupported == VK_TRUE ? "yes" : "no", timing.presentAtRelativeTimeSupported == VK_TRUE ? "yes" : "no",
              stages.empty() ? "no stage" : stages);
    // Asked for where the screen's own stage is reported, with what it needs: present ids (the second
    // form) and calibrated timestamps, each offered, and their features there.
    const VkPresentStageFlagsEXT screen = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT | VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;
    const bool calibrated = has(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) || has(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    VkPhysicalDevicePresentId2FeaturesKHR id2{};
    id2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;
    VkPhysicalDevicePresentTimingFeaturesEXT features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT;
    features.pNext = &id2;
    VkPhysicalDeviceFeatures2 all{};
    all.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    all.pNext = &features;
    vkGetPhysicalDeviceFeatures2(physical_, &all);
    std::string missing;
    if ((timing.presentStageQueries & screen) == 0) missing = "no stage on the screen is reported";
    else if (!has(VK_KHR_PRESENT_ID_2_EXTENSION_NAME)) missing = std::format("no {}", VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
    else if (!calibrated) missing = "no calibrated timestamps";
    else if (features.presentTiming != VK_TRUE || id2.presentId2 != VK_TRUE) missing = "its features are not there";
    if (!missing.empty()) {
        log::info("render: present timing: not used ({})", missing);
        return;
    }
    timing_.wanted = true;
    timing_.stages = timing.presentStageQueries & (VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT | screen);
#else
    log::info("render: present timing: this build's Vulkan headers do not know it");
#endif
}

void Window::give_up_present_timing(std::string_view why) {
    if (timing_.given_up) return;
    log::warn("render: present timing: turned off for this session: {}", why);
    timing_.given_up = true;
    timing_.on = false;
    swapchain_stale_ = true;
}

void Window::start_present_timing() {
#ifdef VK_EXT_present_timing
    if (!timing_.on) return;
    const auto set_size = reinterpret_cast<PFN_vkSetSwapchainPresentTimingQueueSizeEXT>(timing_.set_queue_size);
    const auto domains = reinterpret_cast<PFN_vkGetSwapchainTimeDomainPropertiesEXT>(timing_.domain_properties);
    // Room for a second's reports at the fastest displays; they are read after every present.
    if (const VkResult result = set_size(device_, swapchain_, 256); result != VK_SUCCESS) {
        give_up_present_timing(std::format("its queue could not be made ({})", result_name(result)));
        return;
    }
    VkSwapchainTimeDomainPropertiesEXT domain{};
    domain.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT;
    std::uint64_t counter = 0;
    if (const VkResult result = domains(device_, swapchain_, &domain, &counter); result != VK_SUCCESS || domain.timeDomainCount == 0) {
        give_up_present_timing(std::format("the swapchain names no time domain ({})", result_name(result)));
        return;
    }
    std::vector<VkTimeDomainKHR> kinds(domain.timeDomainCount);
    std::vector<std::uint64_t> ids(domain.timeDomainCount);
    domain.pTimeDomains = kinds.data();
    domain.pTimeDomainIds = ids.data();
    if (const VkResult result = domains(device_, swapchain_, &domain, &counter); result != VK_SUCCESS && result != VK_INCOMPLETE) {
        give_up_present_timing(std::format("its time domains could not be read ({})", result_name(result)));
        return;
    }
    // The steady clock where the swapchain reports in it (then the delay from the hand-off is measured
    // too), else its first domain.
    const auto steady = std::ranges::find(kinds, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR);
    const std::size_t chosen = steady != kinds.end() ? static_cast<std::size_t>(steady - kinds.begin()) : 0;
    timing_.domain = ids[chosen];
    timing_.properties_seen = 0;
    present_clock_.set_steady_domain(steady != kinds.end());
    log::info("render: present timing: on, {} time domains, reported {}", domain.timeDomainCount,
              steady != kinds.end() ? "on this program's clock" : std::format("in domain kind {}", static_cast<int>(kinds[chosen])));
#endif
}

void Window::read_present_timing() {
#ifdef VK_EXT_present_timing
    if (!timing_.on) return;
    const auto past = reinterpret_cast<PFN_vkGetPastPresentationTimingEXT>(timing_.past_timing);
    constexpr std::uint32_t kAtOnce = 16;
    VkPresentStageTimeEXT stages[kAtOnce][4]{};
    VkPastPresentationTimingEXT timings[kAtOnce]{};
    for (std::uint32_t i = 0; i < kAtOnce; ++i) {
        timings[i].sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
        timings[i].presentStageCount = 4;
        timings[i].pPresentStages = stages[i];
    }
    VkPastPresentationTimingPropertiesEXT properties{};
    properties.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
    properties.presentationTimingCount = kAtOnce;
    properties.pPresentationTimings = timings;
    VkPastPresentationTimingInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT;
    info.swapchain = swapchain_;
    const VkResult result = past(device_, &info, &properties);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        give_up_present_timing(std::format("its reports could not be read ({})", result_name(result)));
        return;
    }
    // The refresh, when the swapchain's timing properties are new (or not read yet).
    if (properties.timingPropertiesCounter != timing_.properties_seen || timing_.properties_seen == 0) {
        VkSwapchainTimingPropertiesEXT swapchain_timing{};
        swapchain_timing.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT;
        std::uint64_t counter = 0;
        const auto read = reinterpret_cast<PFN_vkGetSwapchainTimingPropertiesEXT>(timing_.timing_properties);
        if (read(device_, swapchain_, &swapchain_timing, &counter) == VK_SUCCESS) {
            timing_.properties_seen = std::max<std::uint64_t>(counter, 1);
            present_clock_.set_refresh(swapchain_timing.refreshDuration);
            log::debug("render: present timing: refresh {} ns, interval {} ns", swapchain_timing.refreshDuration, swapchain_timing.refreshInterval);
        }
    }
    for (std::uint32_t i = 0; i < std::min(properties.presentationTimingCount, kAtOnce); ++i) {
        const VkPastPresentationTimingEXT& timing = timings[i];
        if (timing.reportComplete != VK_TRUE) continue;
        std::uint64_t first_pixel = 0;
        for (std::uint32_t s = 0; s < std::min(timing.presentStageCount, 4U); ++s) {
            const VkPresentStageTimeEXT& stage = timing.pPresentStages[s];
            if (stage.stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT || (first_pixel == 0 && stage.stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT)) {
                first_pixel = stage.time;
            }
        }
        present_clock_.reported(timing.presentId, first_pixel);
    }
#endif
}

bool Window::create_device() {
    const bool wanted = options_ != nullptr && options_->gpu_decoding;
    DeviceRequest request{instance_, instance_api_, instance_extensions_, physical_, device_api_, queue_family_, device_name_, wanted,
                          {VK_KHR_SWAPCHAIN_EXTENSION_NAME}};
    std::expected<MadeDevice, std::string> made = std::unexpected(std::string());
#ifdef VK_EXT_present_timing
    // Present timing and what it needs, where it was offered; without it when the device cannot be made so.
    if (timing_.wanted) {
        std::uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> offered(count);
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, offered.data());
        const bool khr_calibrated = std::ranges::any_of(offered, [](const VkExtensionProperties& e) {
            return std::string_view(e.extensionName) == VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
        });
        DeviceRequest timed = request;
        timed.extensions.push_back(VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
        timed.extensions.push_back(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
        timed.extensions.push_back(khr_calibrated ? VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME : VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        VkPhysicalDevicePresentId2FeaturesKHR id2{};
        id2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;
        id2.presentId2 = VK_TRUE;
        VkPhysicalDevicePresentTimingFeaturesEXT features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT;
        features.presentTiming = VK_TRUE;
        features.pNext = &id2;
        timed.features = &features;
        made = make_device(timed);
        if (made) {
            timing_.set_queue_size = vkGetDeviceProcAddr(made->device, "vkSetSwapchainPresentTimingQueueSizeEXT");
            timing_.timing_properties = vkGetDeviceProcAddr(made->device, "vkGetSwapchainTimingPropertiesEXT");
            timing_.domain_properties = vkGetDeviceProcAddr(made->device, "vkGetSwapchainTimeDomainPropertiesEXT");
            timing_.past_timing = vkGetDeviceProcAddr(made->device, "vkGetPastPresentationTimingEXT");
            timing_.enabled = timing_.set_queue_size != nullptr && timing_.timing_properties != nullptr && timing_.domain_properties != nullptr &&
                              timing_.past_timing != nullptr;
            if (!timing_.enabled) log::info("render: present timing: not used (the device made with it lacks its functions)");
        } else {
            log::info("render: present timing: not used (the device could not be made with it: {})", made.error());
        }
    }
#endif
    if (!made) made = make_device(request);
    if (!made) {
        log::error("render: {}", made.error());
        return false;
    }
    device_ = made->device;
    queue_ = made->queue;
    share_ = std::move(made->share);
    gpu_why_not_ = made->why_not;
    return true;
}

void Window::share_with_decoder() {
    if (gpu_why_not_.empty()) {
        if (auto shared = media::share_device(share_)) {
            gpu_.emplace(std::move(*shared));
        } else {
            gpu_why_not_ = shared.error();
        }
    }
    if (gpu_) log::info("render: pictures are decoded on {}", gpu_->description());
    else log::info("render: pictures are decoded in software: {}", gpu_why_not_);
    if (options_ != nullptr && options_->on_gpu) options_->on_gpu(gpu_, gpu_why_not_);
}

bool Window::create_frame_objects() {
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queue_family_;
    if (!check(vkCreateCommandPool(device_, &pool, nullptr, &command_pool_), "vkCreateCommandPool")) return false;

    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = command_pool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (!check(vkAllocateCommandBuffers(device_, &allocate, &command_buffer_), "vkAllocateCommandBuffers")) {
        return false;
    }

    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (!check(vkCreateSemaphore(device_, &semaphore, nullptr, &image_available_), "vkCreateSemaphore")) return false;

    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return check(vkCreateFence(device_, &fence, nullptr, &frame_done_), "vkCreateFence");
}

void Window::destroy_swapchain() {
    if (device_ == VK_NULL_HANDLE) return;
    video_.destroy_pipeline();
    overlay_.destroy_pipeline();
    for (const VkSemaphore semaphore : render_done_) vkDestroySemaphore(device_, semaphore, nullptr);
    render_done_.clear();
    for (const VkFramebuffer framebuffer : framebuffers_) vkDestroyFramebuffer(device_, framebuffer, nullptr);
    framebuffers_.clear();
    for (const VkImageView view : views_) vkDestroyImageView(device_, view, nullptr);
    views_.clear();
    if (render_pass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, render_pass_, nullptr);
    render_pass_ = VK_NULL_HANDLE;
}

bool Window::create_swapchain() {
    vkDeviceWaitIdle(device_);
    destroy_swapchain();

    VkSurfaceCapabilitiesKHR caps{};
    if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), "surface capabilities")) {
        return false;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xffffffffU) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        extent.width = std::clamp(static_cast<std::uint32_t>(std::max(width, 0)), caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<std::uint32_t>(std::max(height, 0)), caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        // Minimized: there is nothing to draw to until the window has a size again.
        extent_ = VkExtent2D{};
        return true;
    }

    std::uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &format_count, formats.data());
    if (formats.empty()) {
        log::error("render: the window offers no surface format");
        return false;
    }
    // A plain 8-bit format first: the color we write is the color on screen.
    VkSurfaceFormatKHR chosen = formats.front();
    int chosen_rank = 0;
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        int rank = 0;
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM || candidate.format == VK_FORMAT_R8G8B8A8_UNORM) rank = 2;
        else if (is_srgb_format(candidate.format)) rank = 1;
        if (rank > chosen_rank) {
            chosen_rank = rank;
            chosen = candidate;
        }
    }

    std::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) image_count = std::min(image_count, caps.maxImageCount);

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0) {
        alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    }

    const VkSwapchainKHR previous = swapchain_;
    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = alpha;
    // Shown at the screen's refresh, never torn. Where the newest waiting picture can replace
    // an older one that was not shown yet, it does: a late picture adds no delay.
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always available
    present_mode_name_ = "one after another";
    std::uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, modes.data());
    if (std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) {
        info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        present_mode_name_ = "the newest replaces the one waiting";
    }
    info.clipped = VK_TRUE;
    info.oldSwapchain = previous;
#ifdef VK_EXT_present_timing
    timing_.on = timing_.enabled && !timing_.given_up;
    if (timing_.on) info.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
#endif
    VkSwapchainKHR created = VK_NULL_HANDLE;
    VkResult made = vkCreateSwapchainKHR(device_, &info, nullptr, &created);
#ifdef VK_EXT_present_timing
    if (made != VK_SUCCESS && timing_.on) {
        // Made again without it: the picture matters more than knowing when it was shown.
        log::warn("render: present timing: turned off for this session: the swapchain could not be made with it ({})", result_name(made));
        timing_.on = false;
        timing_.given_up = true;
        info.flags &= ~static_cast<VkSwapchainCreateFlagsKHR>(VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR);
        // The one before was retired by the failed call, made or not: it can be named no more.
        info.oldSwapchain = VK_NULL_HANDLE;
        made = vkCreateSwapchainKHR(device_, &info, nullptr, &created);
    }
#endif
    const bool ok = check(made, "vkCreateSwapchainKHR");
    if (previous != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, previous, nullptr);
    swapchain_ = created;
    if (!ok) return false;
    format_ = chosen.format;
    extent_ = extent;
    start_present_timing();

    VkAttachmentDescription color{};
    color.format = format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference reference{};
    reference.attachment = 0;
    reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount = 1;
    pass.pAttachments = &color;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 1;
    pass.pDependencies = &dependency;
    if (!check(vkCreateRenderPass(device_, &pass, nullptr, &render_pass_), "vkCreateRenderPass")) return false;
    if (!video_.create_pipeline(render_pass_, is_srgb_format(format_))) return false;
    if (!overlay_.create_pipeline(render_pass_, is_srgb_format(format_))) return false;

    std::uint32_t actual_count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, nullptr);
    std::vector<VkImage> images(actual_count);
    if (!check(vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, images.data()), "swapchain images")) {
        return false;
    }
    for (const VkImage image : images) {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format_;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        VkImageView created_view = VK_NULL_HANDLE;
        if (!check(vkCreateImageView(device_, &view, nullptr, &created_view), "vkCreateImageView")) return false;
        views_.push_back(created_view);

        VkFramebufferCreateInfo framebuffer{};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = render_pass_;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &views_.back();
        framebuffer.width = extent_.width;
        framebuffer.height = extent_.height;
        framebuffer.layers = 1;
        VkFramebuffer created_framebuffer = VK_NULL_HANDLE;
        if (!check(vkCreateFramebuffer(device_, &framebuffer, nullptr, &created_framebuffer), "vkCreateFramebuffer")) {
            return false;
        }
        framebuffers_.push_back(created_framebuffer);

        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore created_semaphore = VK_NULL_HANDLE;
        if (!check(vkCreateSemaphore(device_, &semaphore, nullptr, &created_semaphore), "vkCreateSemaphore")) {
            return false;
        }
        render_done_.push_back(created_semaphore);
    }
    swapchain_stale_ = false;
    log::debug("render: swapchain {}x{}, {} images, format {}", extent_.width, extent_.height, images.size(),
               static_cast<int>(format_));
    return true;
}

bool Window::draw_frame(bool& presented, const media::Picture* fresh) {
    presented = false;
    if (!check(vkWaitForFences(device_, 1, &frame_done_, VK_TRUE, UINT64_MAX), "vkWaitForFences")) return false;
    // The frame before is done: the picture's textures are free to be written or made anew.
    if (fresh != nullptr && !video_.set_picture(*fresh) && !std::exchange(picture_refused_, true)) {
        log::error("render: a picture of {}x{} with {} bits cannot be shown", fresh->width, fresh->height, fresh->bits);
    }
    // What lies over the picture, laid out again only when it or the window's size changed.
    if (options_ != nullptr && (options_->overlay_lines || options_->overlay_menu || options_->overlay_notice)) {
        std::vector<std::string> lines = options_->overlay_lines ? options_->overlay_lines() : std::vector<std::string>{};
        std::optional<app::MenuView> menu = options_->overlay_menu ? options_->overlay_menu() : std::nullopt;
        std::string notice = options_->overlay_notice ? options_->overlay_notice() : std::string();
        if (lines != overlay_lines_ || menu != overlay_menu_ || notice != overlay_notice_ || extent_.width != overlay_extent_.width ||
            extent_.height != overlay_extent_.height) {
            const auto width = static_cast<float>(extent_.width), height = static_cast<float>(extent_.height);
            std::vector<overlay::Quad> quads = overlay::stats_panel(lines, width, height);
            std::vector<overlay::Quad> said = overlay::notice_panel(notice, width, height);
            quads.insert(quads.end(), said.begin(), said.end());
            if (menu) {
                std::vector<overlay::Quad> panel = overlay::menu_panel(menu->title, menu->items, menu->selected, menu->hint, width, height);
                quads.insert(quads.end(), panel.begin(), panel.end());
            }
            overlay_.set_quads(std::move(quads));
            overlay_lines_ = std::move(lines);
            overlay_menu_ = std::move(menu);
            overlay_notice_ = std::move(notice);
            overlay_extent_ = extent_;
        }
    }

    std::uint32_t index = 0;
    const VkResult acquired =
        vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_, VK_NULL_HANDLE, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_stale_ = true;
        return true;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return check(acquired, "vkAcquireNextImageKHR");

    if (!check(vkResetFences(device_, 1, &frame_done_), "vkResetFences")) return false;
    if (!check(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer")) return false;

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer")) return false;

    // A picture on the graphics card stays locked from here until it was submitted, or not.
    bool submitted = false;
    struct Unlock {
        VideoPass& video;
        bool& submitted;
        ~Unlock() { video.after_submit(submitted); }
    } unlock{video_, submitted};
    video_.record_upload(command_buffer_);
    overlay_.record_upload(command_buffer_);

    // An sRGB swapchain encodes what we write, so it gets the linear value of the same color.
    // Beside a picture the window is black, as a screen is beside a film.
    VkClearValue clear{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float encoded = video_.has_picture() ? 0.0F : static_cast<float>(kBackground[channel]) / 255.0F;
        clear.color.float32[channel] = is_srgb_format(format_) ? srgb_to_linear(encoded) : encoded;
    }
    clear.color.float32[3] = 1.0F;

    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = render_pass_;
    pass.framebuffer = framebuffers_[index];
    pass.renderArea.extent = extent_;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(command_buffer_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    video_.record_draw(command_buffer_, extent_);
    overlay_.record_draw(command_buffer_, extent_);
    vkCmdEndRenderPass(command_buffer_);
    if (!check(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer")) return false;

    // With a picture decoded on the graphics card, the frame also waits for the decoder's work on
    // it (a timeline semaphore) and tells the decoder when it has read it.
    const std::optional<VideoPass::GpuWait> gpu_wait = video_.gpu_wait();
    const VkSemaphore wait_semaphores[2] = {image_available_, gpu_wait ? gpu_wait->semaphore : VK_NULL_HANDLE};
    const VkPipelineStageFlags wait_stages[2] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    const VkSemaphore signal_semaphores[2] = {render_done_[index], gpu_wait ? gpu_wait->semaphore : VK_NULL_HANDLE};
    const std::uint64_t wait_values[2] = {0, gpu_wait ? gpu_wait->wait : 0};
    const std::uint64_t signal_values[2] = {0, gpu_wait ? gpu_wait->signal : 0};
    VkTimelineSemaphoreSubmitInfo timeline{};
    timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline.waitSemaphoreValueCount = 2;
    timeline.pWaitSemaphoreValues = wait_values;
    timeline.signalSemaphoreValueCount = 2;
    timeline.pSignalSemaphoreValues = signal_values;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = gpu_wait ? &timeline : nullptr;
    submit.waitSemaphoreCount = gpu_wait ? 2 : 1;
    submit.pWaitSemaphores = wait_semaphores;
    submit.pWaitDstStageMask = wait_stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    submit.signalSemaphoreCount = gpu_wait ? 2 : 1;
    submit.pSignalSemaphores = signal_semaphores;
    lock_queue();
    const VkResult queued = vkQueueSubmit(queue_, 1, &submit, frame_done_);
    unlock_queue();
    submitted = queued == VK_SUCCESS;
    if (!check(queued, "vkQueueSubmit")) return false;

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_done_[index];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &index;
#ifdef VK_EXT_present_timing
    // With present timing: an id for the present, and the stages to report (when the queue was done
    // with it, when its first pixel went out); no time to present at: as soon as it can be.
    const std::uint64_t present_id = timing_.next_id;
    VkPresentId2KHR id{};
    id.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR;
    id.swapchainCount = 1;
    id.pPresentIds = &present_id;
    VkPresentTimingInfoEXT when{};
    when.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT;
    when.timeDomainId = timing_.domain;
    when.presentStageQueries = timing_.stages;
    VkPresentTimingsInfoEXT timings{};
    timings.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT;
    timings.pNext = &id;
    timings.swapchainCount = 1;
    timings.pTimingInfos = &when;
    const bool timed = timing_.on;
    if (timed) {
        present.pNext = &timings;
        ++timing_.next_id;
        present_clock_.handed(present_id, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                         std::chrono::steady_clock::now().time_since_epoch())
                                                                         .count()));
    }
#else
    const bool timed = false;
#endif
    lock_queue();
    const VkResult shown = vkQueuePresentKHR(queue_, &present);
    unlock_queue();
    if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR) {
        swapchain_stale_ = true;
    } else if (shown != VK_SUCCESS && timed) {
        // Refused with the timing asked for: the session goes on without it (the swapchain is made again).
        give_up_present_timing(std::format("a present was refused ({})", result_name(shown)));
        return true;
    } else if (shown != VK_SUCCESS) {
        return check(shown, "vkQueuePresentKHR");
    }
    presented = shown != VK_ERROR_OUT_OF_DATE_KHR;
    read_present_timing();
    return true;
}

void Window::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        // Whoever decodes on the device lets go of it first: the decoders, and the pictures they made.
        if (gpu_ && options_ != nullptr && options_->before_device_gone) options_->before_device_gone();
        lock_queue();
        vkDeviceWaitIdle(device_);
        unlock_queue();
        destroy_swapchain();
        video_.destroy();
        overlay_.destroy();
        // FFmpeg's objects on the device go when the last hold on its device context does.
        bool device_free = true;
        if (gpu_) {
            device_free = gpu_->users() == 1;
            if (!device_free) log::error("render: the decoder still holds the graphics device; it is left to the system");
            gpu_.reset();
        }
        if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        if (frame_done_ != VK_NULL_HANDLE) vkDestroyFence(device_, frame_done_, nullptr);
        if (image_available_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, image_available_, nullptr);
        if (command_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, command_pool_, nullptr);
        if (device_free) vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (sdl_started_) {
        // Only the window's own part: sound and controllers may still be in someone's use.
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        if (SDL_WasInit(0) == 0) SDL_Quit();
        sdl_started_ = false;
    }
}

namespace {

// The icon cut from the project's emblem, as 32-bit bitmaps with transparency.
constexpr unsigned char kIcon256[] = {
#include "psvp/window_icon_256.inc"
};
constexpr unsigned char kIcon48[] = {
#include "psvp/window_icon_48.inc"
};

SDL_Surface* load_icon(const unsigned char* data, std::size_t size) {
    SDL_IOStream* stream = SDL_IOFromConstMem(data, size);
    return stream != nullptr ? SDL_LoadBMP_IO(stream, true) : nullptr;
}

// The icon a title bar, a task switcher and a taskbar show for the window. A
// taskbar on Wayland takes its icon from the desktop entry named after the
// application ID instead (set before SDL starts), where one is installed.
void set_window_icon(SDL_Window* window) {
    SDL_Surface* large = load_icon(kIcon256, sizeof(kIcon256));
    SDL_Surface* small = load_icon(kIcon48, sizeof(kIcon48));
    if (large == nullptr || small == nullptr) {
        log::warn("window: the icon could not be read: {}", SDL_GetError());
    } else {
        // The window system picks the size that fits where the icon is shown.
        SDL_AddSurfaceAlternateImage(large, small);
        if (SDL_SetWindowIcon(window, large)) log::info("window: icon set ({} and {} pixels)", large->w, small->w);
        else log::info("window: this window system takes no window icon: {}", SDL_GetError());
    }
    SDL_DestroySurface(small);
    SDL_DestroySurface(large);
}

}  // namespace

int Window::open(const WindowOptions& options) {
    const auto fail = [&](int code, const char* title, const std::string& text) {
        log::error("window: {}", text);
        if (options.dialogs) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text.c_str(), window_);
        return code;
    };

    SDL_SetAppMetadata(PSVP_NAME, PSVP_VERSION, PSVP_APP_ID);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log::error("window: no window system could be started: {}", SDL_GetError());
        return kExitNoWindowSystem;
    }
    sdl_started_ = true;
    const int sdl = SDL_GetVersion();
    log::info("window: SDL {}.{}.{}, video driver {}", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl),
              SDL_VERSIONNUM_MICRO(sdl), SDL_GetCurrentVideoDriver());

    // As it was left the last time: its size, and maximized or on the whole screen.
    place_ = options.place;
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (place_.maximized) flags |= SDL_WINDOW_MAXIMIZED;
    if (place_.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    window_ = SDL_CreateWindow(PSVP_NAME, place_.width, place_.height, flags);
    if (window_ == nullptr) {
        return fail(kExitNoWindowSystem, PSVP_NAME " cannot start",
                    std::format("The window could not be opened. Vulkan may not be installed on this system.\n\n{}",
                                SDL_GetError()));
    }

    set_window_icon(window_);

    if (!create_instance()) {
        return fail(kExitNoWindowSystem, PSVP_NAME " cannot start",
                    "Vulkan could not be started. Check that a Vulkan driver for your graphics card is installed.");
    }
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        return fail(kExitNoWindowSystem, PSVP_NAME " cannot start",
                    std::format("The window cannot be drawn to with Vulkan.\n\n{}", SDL_GetError()));
    }
    if (!pick_device()) {
        return fail(kExitNoDevice, PSVP_NAME " cannot start",
                    std::format("No graphics device on this system can run {}. It needs Vulkan {} or later; "
                                "an updated graphics driver usually provides it.",
                                PSVP_NAME, version_text(kMinimumApi)));
    }
    offer_present_timing();
    if (!create_device() || !create_frame_objects() || !video_.create(physical_, device_, gpu_why_not_.empty()) || !overlay_.create(physical_, device_)) {
        return fail(kExitVulkanFailed, PSVP_NAME " cannot start",
                    std::format("The graphics device {} could not be set up. The log has the details.", device_name_));
    }
    share_with_decoder();
    return 0;
}

int Window::run(const WindowOptions& options) {
    options_ = &options;
    if (const int code = open(options); code != 0) return code;
    if (options.hdr_peak_nits) video_.set_hdr_peak(*options.hdr_peak_nits);
    else video_.set_hdr_peak_measured();
    video_.set_scaler(options.scaler.value_or(software_device_ ? media::Scaler::bilinear : media::kDefaultScaler));
    video_.set_deband(options.deband.value_or(!software_device_));
    if (software_device_ && (!options.scaler || !options.deband)) {
        log::info("render: a software device: the picture is scaled by the tent and not debanded unless asked for (the sharper scalers cost each picture "
                  "more than the processor has to spare)");
    }
    if (wake_event.load() == 0) wake_event.store(SDL_RegisterEvents(1));

    std::uint64_t frames = 0;
    std::uint64_t pictures = 0;
    // With a picture source a frame is drawn when there is something new to show; without
    // one, every time round, as fast as the screen takes frames.
    const bool on_demand = static_cast<bool>(options.next_picture);
    bool stale = true;
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) swapchain_stale_ = true;
            if (event.type == SDL_EVENT_WINDOW_EXPOSED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) stale = true;
            if (event.type == SDL_EVENT_WINDOW_RESIZED) note_size();
            // F11, as everywhere: the whole screen, and back.
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F11 && !event.key.repeat) {
                SDL_SetWindowFullscreen(window_, (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) == 0);
            }
            // A double click on the picture, as in a video player: the same.
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 2) {
                SDL_SetWindowFullscreen(window_, (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) == 0);
            }
            // The title bar's maximize button: a maximized window stays inside the desktop's work area,
            // under its panels, which is not what the stream is maximized for: the whole screen
            // instead (borderless, no mode change). Restored first, so that leaving the full screen
            // comes back to the window's own size.
            if (event.type == SDL_EVENT_WINDOW_MAXIMIZED && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) == 0) {
                SDL_RestoreWindow(window_);
                SDL_SetWindowFullscreen(window_, true);
            }
            if (options.on_event) options.on_event(event);
        }
        if (options.finished && options.finished()) running = false;
        if (!running) break;

        if (swapchain_stale_ && !create_swapchain()) return kExitVulkanFailed;
        if (extent_.width == 0 || extent_.height == 0) {
            SDL_WaitEventTimeout(nullptr, 50);  // minimized: wait for a size without spinning
            swapchain_stale_ = true;
            continue;
        }

        std::optional<media::Picture> fresh;
        if (options.next_picture) fresh = options.next_picture();
        if (on_demand && !fresh && !stale && !swapchain_stale_ && !overlay_due()) {
            SDL_WaitEventTimeout(nullptr, static_cast<Sint32>(options.ask_every.count()));
            continue;
        }

        bool presented = false;
        if (!draw_frame(presented, fresh ? &*fresh : nullptr)) return kExitVulkanFailed;
        if (!presented) {
            stale = true;
            continue;
        }
        stale = false;
        if (++frames == 1) {
            log::info("render: first frame presented, {}x{} on {} ({})", extent_.width, extent_.height, device_name_, present_mode_name_);
        }
        if (fresh && video_.has_picture() && ++pictures == 1) log::info("render: first picture shown, {}", video_.describe());
        if (options.frame_limit && frames >= *options.frame_limit) running = false;
    }
    log::info("render: stopped after {} frames, {} pictures", frames, pictures);
    if (const std::string screen = present_clock_.text(); !screen.empty()) {
        log::info("render: {}", screen);
        if (options.on_screen_times) options.on_screen_times(screen);
    }
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
    place_.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    place_.fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
    if (options.on_place) options.on_place(place_);
    return 0;
}

bool Window::overlay_due() const {
    if (options_ == nullptr) return false;
    if (options_->overlay_notice && options_->overlay_notice() != overlay_notice_) return true;
    if (options_->overlay_menu && options_->overlay_menu() != overlay_menu_) return true;
    return options_->overlay_lines && options_->overlay_lines() != overlay_lines_;
}

// The size to come back to: the window's own, not the screen's while it is maximized or fills it.
void Window::note_size() {
    if ((SDL_GetWindowFlags(window_) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED)) != 0) return;
    int width = 0, height = 0;
    if (!SDL_GetWindowSize(window_, &width, &height)) return;
    if (width < app::WindowPlace::kLeastWidth || height < app::WindowPlace::kLeastHeight || width > app::WindowPlace::kMost ||
        height > app::WindowPlace::kMost) {
        return;
    }
    place_.width = width;
    place_.height = height;
}

}  // namespace

void wake_window() {
    const std::uint32_t type = wake_event.load();
    if (type == 0 || SDL_WasInit(SDL_INIT_EVENTS) == 0) return;
    SDL_Event event{};
    event.type = type;
    SDL_PushEvent(&event);
}

int run_window(const WindowOptions& options) {
    Window window;
    return window.run(options);
}

}  // namespace psvp::render
