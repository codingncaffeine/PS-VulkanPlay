#include "render/vk_window.h"

#include <algorithm>
#include <atomic>
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
#include "psvp/version.h"
#include "render/overlay_pass.h"
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
    bool create_device();
    bool create_frame_objects();
    bool create_swapchain();
    void destroy_swapchain();
    void destroy();
    // Returns false on a failed call; `presented` says whether a frame reached the swapchain.
    // `fresh` is a picture to show from this frame on.
    bool draw_frame(bool& presented, const media::Picture* fresh);
    void note_size();
    app::WindowPlace place_;

    SDL_Window* window_ = nullptr;
    bool sdl_started_ = false;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    std::string device_name_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;

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

    VideoPass video_;
    bool picture_refused_ = false;  // said once
    OverlayPass overlay_;
    const WindowOptions* options_ = nullptr;
    std::vector<std::string> overlay_lines_;  // as laid out last
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
    app.apiVersion = kMinimumApi;

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = extension_count;
    info.ppEnabledExtensionNames = extensions;
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
            device_name_ = std::format("{} ({})", name, kind);
        }
    }
    if (physical_ == VK_NULL_HANDLE) {
        log::error("render: no usable Vulkan device ({} found, none can draw to this window)", devices.size());
        return false;
    }
    return true;
}

bool Window::create_device() {
    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = queue_family_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    const char* const extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    if (!check(vkCreateDevice(physical_, &info, nullptr, &device_), "vkCreateDevice")) return false;
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return true;
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
    VkSwapchainKHR created = VK_NULL_HANDLE;
    const bool ok = check(vkCreateSwapchainKHR(device_, &info, nullptr, &created), "vkCreateSwapchainKHR");
    if (previous != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, previous, nullptr);
    swapchain_ = created;
    if (!ok) return false;
    format_ = chosen.format;
    extent_ = extent;

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
    if (options_ != nullptr && options_->overlay_lines) {
        std::vector<std::string> lines = options_->overlay_lines();
        if (lines != overlay_lines_ || extent_.width != overlay_extent_.width || extent_.height != overlay_extent_.height) {
            overlay_.set_quads(overlay::stats_panel(lines, static_cast<float>(extent_.width), static_cast<float>(extent_.height)));
            overlay_lines_ = std::move(lines);
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

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_available_;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done_[index];
    if (!check(vkQueueSubmit(queue_, 1, &submit, frame_done_), "vkQueueSubmit")) return false;

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_done_[index];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &index;
    const VkResult shown = vkQueuePresentKHR(queue_, &present);
    if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR) {
        swapchain_stale_ = true;
    } else if (shown != VK_SUCCESS) {
        return check(shown, "vkQueuePresentKHR");
    }
    presented = shown != VK_ERROR_OUT_OF_DATE_KHR;
    return true;
}

void Window::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        destroy_swapchain();
        video_.destroy();
        overlay_.destroy();
        if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        if (frame_done_ != VK_NULL_HANDLE) vkDestroyFence(device_, frame_done_, nullptr);
        if (image_available_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, image_available_, nullptr);
        if (command_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, command_pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
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
    if (!create_device() || !create_frame_objects() || !video_.create(physical_, device_) || !overlay_.create(physical_, device_)) {
        return fail(kExitVulkanFailed, PSVP_NAME " cannot start",
                    std::format("The graphics device {} could not be set up. The log has the details.", device_name_));
    }
    return 0;
}

int Window::run(const WindowOptions& options) {
    options_ = &options;
    if (const int code = open(options); code != 0) return code;
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
        if (on_demand && !fresh && !stale && !swapchain_stale_) {
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
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
    place_.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    place_.fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
    if (options.on_place) options.on_place(place_);
    return 0;
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
