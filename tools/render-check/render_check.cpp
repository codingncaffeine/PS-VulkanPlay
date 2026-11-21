// Draws pictures decoded on the graphics card the way the stream window draws them (its own
// VideoPass, from the card's memory, with the decoder's semaphores) into an image nobody sees,
// and compares every pixel with the same samples drawn the software way, read back from the card.
// A stream window on a virtual screen cannot do this: the drivers of graphics cards that decode
// do not present to a virtual X server.
//
//   psvp_render_check FOLDER|CLIP [--every N] [--checks N]
//   psvp_render_check --pattern [--peak NITS]...   (tone mapping: see run_pattern; any Vulkan device)
//   psvp_render_check --scaling [--time]           (scalers and debanding: see run_scaling; any Vulkan device)
//       FOLDER as 'session --dump' wrote it, or CLIP: a stream file laid out as tests/data/clip.h265
//       (its parameter sets, then a frame per unit); every Nth picture of the card is drawn twice
//       (10); --checks: how many of the card's first pictures the decoder compares (as when playing)
//
// Exit: 0 every picture the same both ways | 1 not | 2 usage | 3 this machine cannot decode on a
// graphics card (nothing checked)
#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#include "media/color.h"
#include "media/delta_e.h"
#include "media/gpu_device.h"
#include "media/gpu_share.h"
#include "media/scaler.h"
#include "media/video_decoder.h"
#include "render/video_device.h"
#include "render/video_pass.h"

namespace {

using namespace psvp;

template <typename... Args>
int fail(int code, std::format_string<Args...> format, Args&&... args) {
    std::fprintf(stderr, "render-check: %s\n", std::format(format, std::forward<Args>(args)...).c_str());
    return code;
}

bool good(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return true;
    std::fprintf(stderr, "render-check: %s failed with code %d\n", what, static_cast<int>(result));
    return false;
}

// An image to draw into and a buffer to read it from.
class Target {
public:
    // `wide`: 16 bits a channel (for the scaling check, which looks closer than a step of 255).
    Target(VkPhysicalDevice physical, VkDevice device, bool wide = false)
        : physical_(physical), device_(device), format_(wide ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R8G8B8A8_UNORM), pixel_bytes_(wide ? 8 : 4) {}
    ~Target() { destroy(); }
    Target(const Target&) = delete;
    Target& operator=(const Target&) = delete;

    bool create_pass() {
        VkAttachmentDescription color{};
        color.format = format_;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkSubpassDependency after{};
        after.srcSubpass = 0;
        after.dstSubpass = VK_SUBPASS_EXTERNAL;
        after.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        after.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo pass{};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        pass.attachmentCount = 1;
        pass.pAttachments = &color;
        pass.subpassCount = 1;
        pass.pSubpasses = &subpass;
        pass.dependencyCount = 1;
        pass.pDependencies = &after;
        return good(vkCreateRenderPass(device_, &pass, nullptr, &pass_), "vkCreateRenderPass");
    }
    [[nodiscard]] VkRenderPass pass() const { return pass_; }

    bool size(std::uint32_t width, std::uint32_t height) {
        if (width == width_ && height == height_) return true;
        destroy_images();
        VkImageCreateInfo image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = format_;
        image.extent = {width, height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!good(vkCreateImage(device_, &image, nullptr, &image_), "vkCreateImage")) return false;
        VkMemoryRequirements needs{};
        vkGetImageMemoryRequirements(device_, image_, &needs);
        if (!allocate(needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image_memory_) || !good(vkBindImageMemory(device_, image_, image_memory_, 0), "vkBindImageMemory")) {
            return false;
        }
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = image_;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format_;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!good(vkCreateImageView(device_, &view, nullptr, &view_), "vkCreateImageView")) return false;
        VkFramebufferCreateInfo framebuffer{};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = pass_;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &view_;
        framebuffer.width = width;
        framebuffer.height = height;
        framebuffer.layers = 1;
        if (!good(vkCreateFramebuffer(device_, &framebuffer, nullptr, &framebuffer_), "vkCreateFramebuffer")) return false;
        VkBufferCreateInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size = VkDeviceSize{width} * height * pixel_bytes_;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (!good(vkCreateBuffer(device_, &buffer, nullptr, &buffer_), "vkCreateBuffer")) return false;
        vkGetBufferMemoryRequirements(device_, buffer_, &needs);
        if (!allocate(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer_memory_) ||
            !good(vkBindBufferMemory(device_, buffer_, buffer_memory_, 0), "vkBindBufferMemory") ||
            !good(vkMapMemory(device_, buffer_memory_, 0, VK_WHOLE_SIZE, 0, &mapped_), "vkMapMemory")) {
            return false;
        }
        width_ = width;
        height_ = height;
        return true;
    }
    [[nodiscard]] VkFramebuffer framebuffer() const { return framebuffer_; }
    [[nodiscard]] VkImage image() const { return image_; }
    [[nodiscard]] VkBuffer buffer() const { return buffer_; }
    [[nodiscard]] const std::uint8_t* pixels() const { return static_cast<const std::uint8_t*>(mapped_); }
    [[nodiscard]] VkExtent2D extent() const { return {width_, height_}; }
    // A channel of a pixel in steps of 255 (a 16-bit target's values keep their fractions).
    [[nodiscard]] double value(std::uint32_t x, std::uint32_t y, unsigned channel) const {
        const std::size_t at = (std::size_t{y} * width_ + x) * pixel_bytes_;
        if (pixel_bytes_ == 4) return pixels()[at + channel];
        std::uint16_t sample = 0;
        std::memcpy(&sample, pixels() + at + 2 * channel, 2);
        return static_cast<double>(sample) / 257.0;
    }

private:
    bool allocate(const VkMemoryRequirements& needs, VkMemoryPropertyFlags wanted, VkDeviceMemory& memory) {
        VkPhysicalDeviceMemoryProperties types{};
        vkGetPhysicalDeviceMemoryProperties(physical_, &types);
        for (std::uint32_t i = 0; i < types.memoryTypeCount; ++i) {
            if ((needs.memoryTypeBits & (1U << i)) != 0 && (types.memoryTypes[i].propertyFlags & wanted) == wanted) {
                VkMemoryAllocateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                info.allocationSize = needs.size;
                info.memoryTypeIndex = i;
                return good(vkAllocateMemory(device_, &info, nullptr, &memory), "vkAllocateMemory");
            }
        }
        std::fprintf(stderr, "render-check: no memory of the kind wanted\n");
        return false;
    }
    void destroy_images() {
        if (mapped_ != nullptr) vkUnmapMemory(device_, buffer_memory_);
        mapped_ = nullptr;
        if (buffer_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, buffer_, nullptr);
        if (buffer_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, buffer_memory_, nullptr);
        if (framebuffer_ != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, framebuffer_, nullptr);
        if (view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, view_, nullptr);
        if (image_ != VK_NULL_HANDLE) vkDestroyImage(device_, image_, nullptr);
        if (image_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, image_memory_, nullptr);
        buffer_ = VK_NULL_HANDLE;
        buffer_memory_ = image_memory_ = VK_NULL_HANDLE;
        framebuffer_ = VK_NULL_HANDLE;
        view_ = VK_NULL_HANDLE;
        image_ = VK_NULL_HANDLE;
        width_ = height_ = 0;
    }
    void destroy() {
        destroy_images();
        if (pass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, pass_, nullptr);
        pass_ = VK_NULL_HANDLE;
    }

    VkPhysicalDevice physical_;
    VkDevice device_;
    VkFormat format_;
    std::size_t pixel_bytes_;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    std::uint32_t width_ = 0, height_ = 0;
};

// Draws one picture into the target as the window does, and waits until it is there: a target of
// `size` (none: the picture's), the times before the conversion, before the drawing and after it
// written to `timing` (3 timestamps) when given.
bool draw(VkDevice device, VkQueue queue, std::uint32_t family, const media::GpuDevice* gpu, VkCommandBuffer commands, VkFence fence,
          render::VideoPass& video, Target& target, const media::Picture& picture, VkExtent2D size = {}, VkQueryPool timing = VK_NULL_HANDLE) {
    if (!video.set_picture(picture)) return false;
    if (size.width == 0) size = {picture.width, picture.height};
    if (!target.size(size.width, size.height)) return false;
    if (!good(vkResetCommandBuffer(commands, 0), "vkResetCommandBuffer")) return false;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!good(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer")) return false;
    bool submitted = false;
    if (timing != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(commands, timing, 0, 3);
        vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timing, 0);
    }
    video.record_upload(commands);
    if (timing != VK_NULL_HANDLE) vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing, 1);
    VkClearValue clear{};
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = target.pass();
    pass.framebuffer = target.framebuffer();
    pass.renderArea.extent = target.extent();
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    video.record_draw(commands, target.extent());
    vkCmdEndRenderPass(commands);
    if (timing != VK_NULL_HANDLE) vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing, 2);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {target.extent().width, target.extent().height, 1};
    vkCmdCopyImageToBuffer(commands, target.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.buffer(), 1, &copy);
    if (!good(vkEndCommandBuffer(commands), "vkEndCommandBuffer")) {
        video.after_submit(false);
        return false;
    }
    // As the window submits: waiting for the decoder's work on the image, telling it when it was read.
    const auto wait = video.gpu_wait();
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo timeline{};
    timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    std::uint64_t wait_value = 0, signal_value = 0;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    if (wait) {
        wait_value = wait->wait;
        signal_value = wait->signal;
        timeline.waitSemaphoreValueCount = 1;
        timeline.pWaitSemaphoreValues = &wait_value;
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &signal_value;
        submit.pNext = &timeline;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &wait->semaphore;
        submit.pWaitDstStageMask = &stage;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &wait->semaphore;
    }
    if (gpu != nullptr) gpu->lock_queue(family, 0);
    const VkResult queued = vkQueueSubmit(queue, 1, &submit, fence);
    if (gpu != nullptr) gpu->unlock_queue(family, 0);
    submitted = queued == VK_SUCCESS;
    video.after_submit(submitted);
    if (!good(queued, "vkQueueSubmit")) return false;
    const bool done = good(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    vkResetFences(device, 1, &fence);
    return done;
}

std::vector<std::vector<std::uint64_t>> rows(std::string_view text) {
    std::vector<std::vector<std::uint64_t>> out;
    bool heading = true;
    for (const auto line : std::views::split(text, '\n')) {
        if (std::exchange(heading, false) || line.empty()) continue;
        std::vector<std::uint64_t> row;
        for (const auto field : std::views::split(line, '\t')) {
            std::uint64_t value = 0;
            std::from_chars(field.data(), field.data() + field.size(), value);
            row.push_back(value);
        }
        out.push_back(std::move(row));
    }
    return out;
}

// Whether the pixels are the picture's: where the color samples around a place are all the same (no
// edge for the filtering to blend), its pixel against the screen color of the picture's samples there.
// The largest difference in steps of 255 and as CIEDE2000, and how many places were looked at.
struct Off {
    unsigned steps = 0;
    double delta_e = 0.0;
    unsigned looked = 0;
};
Off off_from_picture(const media::Picture& picture, const std::uint8_t* pixels) {
    const media::ColorTransform transform = media::color_transform(picture.matrix, picture.full_range, picture.bits);
    unsigned worst = 0, looked = 0;
    double worst_delta_e = 0.0;
    const auto sample = [&](int plane, unsigned x, unsigned y) -> int {
        const std::uint8_t* row = picture.planes[plane] + std::size_t{y} * picture.strides[plane];
        if (picture.bits == 8) return row[x];
        std::uint16_t value = 0;
        std::memcpy(&value, row + 2 * x, 2);
        return value;
    };
    const unsigned color_width = (picture.width + 1) / 2, color_height = (picture.height + 1) / 2;
    // One step of an 8-bit sample; for a tone mapped picture one step of its own: the pixel blends a
    // quarter of the next row of color samples in, and the tone curve makes a few steps of ten bits
    // in a bright place several of 255.
    const int flat = picture.transfer != media::ColorTransfer::sdr ? 1 : 1 << (picture.bits - 8);
    for (unsigned cy = 1; cy + 1 < color_height; cy += 3) {
        for (unsigned cx = 1; cx + 1 < color_width; cx += 3) {
            bool even = true;
            for (int plane = 1; plane <= 2 && even; ++plane) {
                const int middle = sample(plane, cx, cy);
                for (int dy = -1; dy <= 1 && even; ++dy) {
                    for (int dx = -1; dx <= 1 && even; ++dx) {
                        even = std::abs(sample(plane, cx + static_cast<unsigned>(dx), cy + static_cast<unsigned>(dy)) - middle) <= flat;
                    }
                }
            }
            if (!even) continue;
            const unsigned x = 2 * cx, y = 2 * cy;
            const auto expected = media::screen_color(transform, picture.bits, static_cast<unsigned>(sample(0, x, y)), static_cast<unsigned>(sample(1, cx, cy)),
                                                      static_cast<unsigned>(sample(2, cx, cy)), media::ToneMap{picture.transfer});
            const std::uint8_t* pixel = pixels + (std::size_t{y} * picture.width + x) * 4;
            unsigned off = 0;
            for (int c = 0; c < 3; ++c) off = std::max(off, static_cast<unsigned>(std::abs(expected[static_cast<std::size_t>(c)] - pixel[c])));
            worst = std::max(worst, off);
            if (off > 1) {
                const auto u8 = [](int value) { return static_cast<std::uint8_t>(value); };
                worst_delta_e = std::max(worst_delta_e, media::delta_e_2000(media::lab_of_srgb(u8(expected[0]), u8(expected[1]), u8(expected[2])),
                                                                            media::lab_of_srgb(pixel[0], pixel[1], pixel[2])));
            }
            ++looked;
        }
    }
    return {worst, worst_delta_e, looked};
}

// Whether two drawings of a picture agree: within 1 of 255 in every channel for an SDR picture. A
// tone mapped HDR picture is judged by how the colors look (CIEDE2000 at most 1, the smallest
// difference an eye notices): near black and at the edge of BT.709 the steps of the curve and of
// the gamut conversion make the graphics card's and software's few-thousandths differences in
// their samples' filtering several steps of 255 in one channel, seen 2026-11-21 on a console's
// picture (15 steps of red in a bright green: CIEDE2000 0.25).
constexpr double kMostDeltaE = 1.0;

// Samples of a 10-bit, limited range BT.2020 picture for red, green and blue as the picture encodes them.
std::array<unsigned, 3> samples_of(double red, double green, double blue) {
    const double kr = 0.2627, kb = 0.0593, kg = 1.0 - kr - kb;
    const double y = kr * red + kg * green + kb * blue;
    const double cb = (blue - y) / (2.0 * (1.0 - kb)), cr = (red - y) / (2.0 * (1.0 - kr));
    const auto code = [](double value) { return static_cast<unsigned>(std::lround(value)); };
    return {code(64.0 + 876.0 * y), code(512.0 + 896.0 * cb), code(512.0 + 896.0 * cr)};
}

// --pattern: patches of even color (grays from below black to above white, and red, green and blue
// in seven steps each), drawn the software way into an image nobody sees, every patch's middle
// against media::screen_color: as an SDR, a PQ (at the peaks given) and an HLG picture. Needs no
// decoder, so any Vulkan device does (a software one too). The control: the HDR pictures must come
// out unlike what the same samples give without tone mapping, or the check could not see it missing.
int run_pattern(const std::vector<double>& peaks) {
    // The patches: 8 by 8 pixels, so a color sample (4 by 4 of them) never blends with a neighbor's.
    std::vector<std::array<unsigned, 3>> patches;
    for (unsigned y = 0; y <= 1023; y += 33) patches.push_back({y, 512, 512});
    patches.push_back({940, 512, 512});
    const double steps[] = {0.0, 0.25, 0.5, 0.62, 0.75, 0.9, 1.0};
    for (const double r : steps) {
        for (const double g : steps) {
            for (const double b : steps) patches.push_back(samples_of(r, g, b));
        }
    }
    constexpr unsigned kSide = 8, kAcross = 24;
    const unsigned down = static_cast<unsigned>((patches.size() + kAcross - 1) / kAcross);
    const unsigned width = kAcross * kSide, height = down * kSide;
    std::vector<std::uint16_t> plane_y(std::size_t{width} * height, 64), plane_cb(std::size_t{width / 2} * (height / 2), 512),
        plane_cr(std::size_t{width / 2} * (height / 2), 512);
    for (std::size_t i = 0; i < patches.size(); ++i) {
        const unsigned px = static_cast<unsigned>(i % kAcross) * kSide, py = static_cast<unsigned>(i / kAcross) * kSide;
        for (unsigned y = 0; y < kSide; ++y) {
            for (unsigned x = 0; x < kSide; ++x) plane_y[std::size_t{py + y} * width + px + x] = static_cast<std::uint16_t>(patches[i][0]);
        }
        for (unsigned y = 0; y < kSide / 2; ++y) {
            for (unsigned x = 0; x < kSide / 2; ++x) {
                const std::size_t at = std::size_t{py / 2 + y} * (width / 2) + px / 2 + x;
                plane_cb[at] = static_cast<std::uint16_t>(patches[i][1]);
                plane_cr[at] = static_cast<std::uint16_t>(patches[i][2]);
            }
        }
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "psvp_render_check";
    app.apiVersion = render::kVideoApi;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (!good(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance")) return 3;
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(instance, &count, physicals.data());
    std::optional<render::MadeDevice> made;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    std::string name;
    for (const VkPhysicalDevice candidate : physicals) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        const auto graphics = std::ranges::find_if(families, [](const VkQueueFamilyProperties& f) { return (f.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0; });
        if (graphics == families.end()) continue;
        const auto index = static_cast<std::uint32_t>(graphics - families.begin());
        auto attempt = render::make_device(render::DeviceRequest{instance, render::kVideoApi, {}, candidate, properties.apiVersion, index, properties.deviceName, false, {}});
        if (!attempt) continue;
        made = std::move(*attempt);
        physical = candidate;
        family = index;
        name = properties.deviceName;
        break;
    }
    if (!made) {
        vkDestroyInstance(instance, nullptr);
        return fail(3, "no Vulkan device here can draw");
    }
    const VkDevice device = made->device;
    int code = 0;
    {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = family;
        VkCommandPool pool = VK_NULL_HANDLE;
        vkCreateCommandPool(device, &pool_info, nullptr, &pool);
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device, &allocate, &commands);
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(device, &fence_info, nullptr, &fence);
        {
            render::VideoPass pass;
            Target target(physical, device);
            bool broken = false;  // nothing more can be drawn
            if (!pass.create(physical, device, false) || !target.create_pass() || !pass.create_pipeline(target.pass(), false)) {
                code = fail(1, "the pass could not be set up");
                broken = true;
            }
            struct Run {
                media::ColorTransfer transfer;
                double peak;
                const char* name;
            };
            std::vector<Run> runs{{media::ColorTransfer::sdr, media::kAssumedHdrPeakNits, "SDR"}, {media::ColorTransfer::hlg, media::kHlgPeakNits, "HLG"}};
            for (const double peak : peaks) runs.push_back({media::ColorTransfer::pq, peak, "PQ"});
            const media::ColorTransform transform = media::color_transform(media::ColorMatrix::bt2020, false, 10);
            std::printf("render-check: drawing %zu patches of a 10-bit BT.2020 picture on %s\n", patches.size(), name.c_str());
            for (const Run& run : runs) {
                if (broken) break;
                media::Picture picture;
                picture.width = width;
                picture.height = height;
                picture.bits = 10;
                picture.matrix = media::ColorMatrix::bt2020;
                picture.transfer = run.transfer;
                picture.planes[0] = reinterpret_cast<const std::uint8_t*>(plane_y.data());
                picture.planes[1] = reinterpret_cast<const std::uint8_t*>(plane_cb.data());
                picture.planes[2] = reinterpret_cast<const std::uint8_t*>(plane_cr.data());
                picture.strides[0] = std::size_t{width} * 2;
                picture.strides[1] = picture.strides[2] = std::size_t{width / 2} * 2;
                pass.set_hdr_peak(run.peak);
                if (!draw(device, made->queue, family, nullptr, commands, fence, pass, target, picture)) {
                    code = fail(1, "the {} picture could not be drawn", run.name);
                    broken = true;
                    break;
                }
                if (pass.describe().find("shown as 8") != std::string::npos) {
                    code = fail(1, "this device cannot smooth 16-bit textures: the samples were cut to 8 bits ({})", pass.describe());
                    break;
                }
                unsigned worst = 0, unlike_untoned = 0;
                std::string worst_where;
                for (std::size_t i = 0; i < patches.size(); ++i) {
                    const unsigned x = static_cast<unsigned>(i % kAcross) * kSide + kSide / 2, y = static_cast<unsigned>(i / kAcross) * kSide + kSide / 2;
                    const std::uint8_t* pixel = target.pixels() + (std::size_t{y} * width + x) * 4;
                    const auto want = media::screen_color(transform, 10, patches[i][0], patches[i][1], patches[i][2], media::ToneMap{run.transfer, run.peak});
                    const auto untoned = media::screen_color(transform, 10, patches[i][0], patches[i][1], patches[i][2]);
                    unsigned off = 0, apart = 0;
                    for (std::size_t c = 0; c < 3; ++c) {
                        off = std::max(off, static_cast<unsigned>(std::abs(want[c] - pixel[c])));
                        apart = std::max(apart, static_cast<unsigned>(std::abs(untoned[c] - pixel[c])));
                    }
                    if (apart > 8) ++unlike_untoned;
                    if (off > worst) {
                        worst = off;
                        worst_where = std::format(" (samples {} {} {}: drawn {} {} {}, wanted {} {} {})", patches[i][0], patches[i][1], patches[i][2], pixel[0], pixel[1], pixel[2],
                                                  want[0], want[1], want[2]);
                    }
                }
                const bool toned = run.transfer != media::ColorTransfer::sdr;
                std::printf("render-check: %s%s: %zu patches within %u of 255 of media::screen_color%s; %u unlike the same samples without tone mapping\n", run.name,
                            run.transfer == media::ColorTransfer::pq ? std::format(" at {:.0f} cd/m²", run.peak).c_str() : "", patches.size(), worst,
                            worst > 1 ? worst_where.c_str() : "", unlike_untoned);
                if (worst > 1) code = 1;
                // SDR is drawn as it always was; an HDR picture drawn untouched would agree with "untoned" everywhere.
                if (toned ? unlike_untoned < patches.size() / 4 : unlike_untoned != 0) {
                    std::printf("render-check: %s: the control failed (%u patches unlike the untouched colors)\n", run.name, unlike_untoned);
                    code = 1;
                }
            }
            vkDeviceWaitIdle(device);
            pass.destroy();
        }
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
    }
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return code;
}

// A gray 8-bit, limited range picture of the given samples (the color planes even: its red, green
// and blue are all (y - 16) / 219, so a reference can follow every pixel through both passes).
struct GrayPicture {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> y, cb, cr;
    [[nodiscard]] media::Picture picture() const {
        media::Picture p;
        p.width = width;
        p.height = height;
        p.bits = 8;
        p.matrix = media::ColorMatrix::bt709;
        p.planes[0] = y.data();
        p.planes[1] = cb.data();
        p.planes[2] = cr.data();
        p.strides[0] = width;
        p.strides[1] = p.strides[2] = (width + 1) / 2;
        return p;
    }
    // Red, green and blue as the conversion makes them (before any debanding), as shares.
    [[nodiscard]] std::vector<float> shares() const {
        std::vector<float> out(y.size());
        for (std::size_t i = 0; i < y.size(); ++i) out[i] = static_cast<float>(std::clamp((static_cast<double>(y[i]) - 16.0) / 219.0, 0.0, 1.0));
        return out;
    }
};

GrayPicture gray_picture(std::uint32_t width, std::uint32_t height, const std::function<int(std::uint32_t, std::uint32_t)>& sample) {
    GrayPicture g;
    g.width = width;
    g.height = height;
    g.y.resize(std::size_t{width} * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) g.y[std::size_t{y} * width + x] = static_cast<std::uint8_t>(std::clamp(sample(x, y), 0, 255));
    }
    const std::size_t color = std::size_t{(width + 1) / 2} * ((height + 1) / 2);
    g.cb.assign(color, 128);
    g.cr.assign(color, 128);
    return g;
}


// --scaling: the scalers and the debanding against media/scaler.h's references.
//   1. A picture of fine detail (checkerboards of 1, 2 and 4 samples, an edge at a slant, noise, a
//      ramp) drawn by every scaler larger (1.5 and 2.36 times), as large, and smaller (0.6 and 0.32
//      times) into a target of 16 bits a channel: every pixel within 0.05 of a step of 255 of the
//      reference of that scaler (debanding moves values by fractions of a step: an 8-bit target
//      could not see a wrong offset or seed). The control: the references of every two scalers
//      differ by more than twice that somewhere at every size but the picture's own, so a pass that
//      drew with another kernel (or none) could not pass.
//   2. A ramp cut into bands of one step, over noise and a hard edge, debanded: within 0.05 of a
//      step of the reference in all but 0.5% of the pixels (a place right at the threshold can go
//      either way in float). The control: without debanding the bands' edges are whole steps
//      between columns, with it less than half of that, and the edge and the noise stay.
//   3. With --time: how long the two passes take for the console's 1080p drawn into a 3440x1440
//      window, by each scaler, with debanding (the device's own timestamps, the middle of 7 draws).
int run_scaling(bool time) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "psvp_render_check";
    app.apiVersion = render::kVideoApi;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (!good(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance")) return 3;
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(instance, &count, physicals.data());
    std::optional<render::MadeDevice> made;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    VkPhysicalDeviceProperties properties{};
    std::uint32_t timestamp_bits = 0;
    for (const VkPhysicalDevice candidate : physicals) {
        vkGetPhysicalDeviceProperties(candidate, &properties);
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        const auto graphics = std::ranges::find_if(families, [](const VkQueueFamilyProperties& f) { return (f.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0; });
        if (graphics == families.end()) continue;
        const auto index = static_cast<std::uint32_t>(graphics - families.begin());
        auto attempt = render::make_device(render::DeviceRequest{instance, render::kVideoApi, {}, candidate, properties.apiVersion, index, properties.deviceName, false, {}});
        if (!attempt) continue;
        made = std::move(*attempt);
        physical = candidate;
        family = index;
        timestamp_bits = graphics->timestampValidBits;
        break;
    }
    if (!made) {
        vkDestroyInstance(instance, nullptr);
        return fail(3, "no Vulkan device here can draw");
    }
    const VkDevice device = made->device;
    int code = 0;
    {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = family;
        VkCommandPool pool = VK_NULL_HANDLE;
        vkCreateCommandPool(device, &pool_info, nullptr, &pool);
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device, &allocate, &commands);
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(device, &fence_info, nullptr, &fence);
        VkQueryPool timing = VK_NULL_HANDLE;
        if (timestamp_bits != 0) {
            VkQueryPoolCreateInfo query{};
            query.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = 3;
            if (!good(vkCreateQueryPool(device, &query, nullptr, &timing), "vkCreateQueryPool")) timing = VK_NULL_HANDLE;
        }
        {
            render::VideoPass pass;
            Target target(physical, device, true);
            bool broken = false;  // nothing more can be drawn
            if (!pass.create(physical, device, false) || !target.create_pass() || !pass.create_pipeline(target.pass(), false)) {
                code = fail(1, "the pass could not be set up");
                broken = true;
            }
            const auto draw_at = [&](const GrayPicture& g, VkExtent2D size, VkQueryPool query = VK_NULL_HANDLE) {
                const media::Picture picture = g.picture();
                return draw(device, made->queue, family, nullptr, commands, fence, pass, target, picture, size, query);
            };
            std::printf("render-check: scaling and debanding on %s\n", properties.deviceName);
            const std::vector<float> tables = media::weight_tables();

            // 1. The scalers.
            const GrayPicture detail = gray_picture(96, 64, [](std::uint32_t x, std::uint32_t y) {
                if (y < 32) {
                    const std::uint32_t cell = x < 32 ? 1 : x < 64 ? 2 : 4;
                    return ((x / cell + y / cell) % 2 == 0) ? 40 : 210;
                }
                if (x < 32) return 3.0 * static_cast<double>(x) > 2.0 * static_cast<double>(y - 32) + 20.0 ? 220 : 30;
                if (x < 64) return 30 + static_cast<int>(media::hash32(x * 977U + y) % 190U);
                return 20 + static_cast<int>((x - 64) * 6U + (y - 32));
            });
            // As the converted picture holds them: 16 bits a channel (half floats where a device cannot draw into those).
            const bool unorm = pass.converted_format() == VK_FORMAT_R16G16B16A16_UNORM;
            const auto stored = [&](float share) { return unorm ? std::round(std::clamp(share, 0.0F, 1.0F) * 65535.0F) / 65535.0F : share; };
            std::vector<float> detail_shares = detail.shares();
            for (float& share : detail_shares) share = stored(share);
            // How close a pixel must be, in steps of 255: a few hundredths, which half floats' own steps exceed.
            const double most = unorm ? 0.05 : 0.3;
            const VkExtent2D sizes[] = {{144, 96}, {227, 151}, {96, 64}, {58, 39}, {31, 21}};
            pass.set_deband(false);
            for (const VkExtent2D size : sizes) {
                if (broken) break;
                std::map<media::Scaler, std::vector<float>> references;
                media::Placement placed;
                for (const media::Scaler scaler : media::kScalers) {
                    pass.set_scaler(scaler);
                    if (!draw_at(detail, size)) {
                        code = fail(1, "the picture could not be drawn at {}x{}", size.width, size.height);
                        broken = true;
                        break;
                    }
                    placed = pass.placement();
                    auto& reference = references[scaler];
                    reference = media::scale_plane(detail_shares, detail.width, detail.height, placed.width, placed.height, scaler, tables);
                    double worst = 0.0;
                    std::string where;
                    for (std::uint32_t y = 0; y < placed.height; ++y) {
                        for (std::uint32_t x = 0; x < placed.width; ++x) {
                            const double want = std::clamp(static_cast<double>(reference[std::size_t{y} * placed.width + x]), 0.0, 1.0) * 255.0;
                            for (unsigned c = 0; c < 3; ++c) {
                                const double got = target.value(x + static_cast<std::uint32_t>(placed.x), y + static_cast<std::uint32_t>(placed.y), c);
                                if (std::abs(got - want) > worst) {
                                    worst = std::abs(got - want);
                                    where = std::format(" (at {},{}: drawn {:.3f}, wanted {:.3f})", x, y, got, want);
                                }
                            }
                        }
                    }
                    std::printf("render-check: scaler %s at %ux%u: the largest difference from the reference %.3f of a step of 255%s: %s\n",
                                std::string(media::scaler_name(scaler)).c_str(), placed.width, placed.height, worst, worst > most ? where.c_str() : "",
                                worst <= most ? "same" : "DIFFERENT");
                    if (worst > most) code = 1;
                }
                if (broken || (placed.width == detail.width && placed.height == detail.height)) continue;
                // The control: every two scalers' references apart somewhere by more than the check lets pass.
                std::size_t fewest = SIZE_MAX;
                std::string closest;
                for (std::size_t a = 0; a < media::kScalers.size(); ++a) {
                    for (std::size_t b = a + 1; b < media::kScalers.size(); ++b) {
                        const auto& one = references[media::kScalers[a]];
                        const auto& other = references[media::kScalers[b]];
                        std::size_t apart = 0;
                        for (std::size_t k = 0; k < one.size(); ++k) apart += std::abs(static_cast<double>(one[k] - other[k])) * 255.0 > 2.0 * most ? 1U : 0U;
                        if (apart < fewest) {
                            fewest = apart;
                            closest = std::format("{} and {}", media::scaler_name(media::kScalers[a]), media::scaler_name(media::kScalers[b]));
                        }
                    }
                }
                std::printf("render-check: control at %ux%u: the closest two scalers (%s) differ by more than %.2f in %zu pixels\n", placed.width, placed.height,
                            closest.c_str(), 2.0 * most, fewest);
                if (fewest < 5) {
                    std::printf("render-check: the control failed: the check could not tell %s apart\n", closest.c_str());
                    code = 1;
                }
            }

            // 2. Debanding.
            const std::uint32_t band_width = 256, band_height = 128;
            const GrayPicture banded = gray_picture(band_width, band_height, [](std::uint32_t x, std::uint32_t y) {
                if (y < 64) return 100 + static_cast<int>(x / 16);
                if (x < 128) return 60 + static_cast<int>(media::hash32(x * 131U + y * 7U) % 41U);
                return ((x / 8 + y / 8) % 2 == 0) ? 50 : 190;
            });
            // Each drawing's gray, in steps of 255.
            const auto gray_of = [&] {
                std::vector<double> gray(std::size_t{band_width} * band_height);
                for (std::uint32_t y = 0; y < band_height; ++y) {
                    for (std::uint32_t x = 0; x < band_width; ++x) gray[std::size_t{y} * band_width + x] = target.value(x, y, 0);
                }
                return gray;
            };
            std::vector<double> plain, smoothed;
            pass.set_scaler(media::kDefaultScaler);
            pass.set_deband(false);
            if (!broken && draw_at(banded, {})) plain = gray_of();
            pass.set_deband(true);
            if (!broken && draw_at(banded, {})) smoothed = gray_of();
            if (!broken && (plain.empty() || smoothed.empty())) {
                code = fail(1, "the banded picture could not be drawn");
                broken = true;
            }
            if (!broken) {
                const media::Deband settings;
                std::vector<float> samples(banded.y.size());
                for (std::size_t k = 0; k < samples.size(); ++k) samples[k] = static_cast<float>(banded.y[k]) / 255.0F;
                const std::vector<float> debanded = media::deband_plane(samples, band_width, band_height, settings, pass.seed());
                std::size_t off = 0;
                double worst = 0.0;
                for (std::uint32_t y = 0; y < band_height; ++y) {
                    for (std::uint32_t x = 0; x < band_width; ++x) {
                        const std::size_t k = std::size_t{y} * band_width + x;
                        const double rgb = std::clamp((static_cast<double>(debanded[k]) * 255.0 - 16.0) / 219.0, 0.0, 1.0);
                        const float with_grain = stored(static_cast<float>(rgb) + media::deband_grain(media::place_hash(x, y, pass.seed()), settings.grain));
                        const double apart = std::abs(smoothed[k] - static_cast<double>(with_grain) * 255.0);
                        worst = std::max(worst, apart);
                        off += apart > most ? 1U : 0U;
                    }
                }
                const double off_share = static_cast<double>(off) / static_cast<double>(band_width * band_height);
                std::printf("render-check: debanding: %.2f%% of the pixels more than %.2f of a step of 255 from the reference (the largest %.3f)\n", 100.0 * off_share, most, worst);
                if (off_share > 0.005) code = 1;
                // The control: in the banded half, each column's mean over its rows (the grain averages out),
                // and the largest step between two columns side by side: a band's edge (1.16 of 255) without
                // debanding, much less with it.
                const auto largest_step = [&](const std::vector<double>& gray) {
                    std::vector<double> column(band_width, 0.0);
                    for (std::uint32_t y = 8; y < 56; ++y) {
                        for (std::uint32_t x = 0; x < band_width; ++x) column[x] += gray[std::size_t{y} * band_width + x] / 48.0;
                    }
                    double step = 0.0;
                    for (std::uint32_t x = 24; x + 1 < band_width - 24; ++x) step = std::max(step, std::abs(column[x + 1] - column[x]));
                    return step;
                };
                const double plain_error = largest_step(plain), smoothed_error = largest_step(smoothed);
                std::size_t kept = 0, detail_pixels = 0;
                for (std::uint32_t y = 64; y < band_height; ++y) {
                    for (std::uint32_t x = 0; x < band_width; ++x, ++detail_pixels) {
                        const std::size_t k = std::size_t{y} * band_width + x;
                        kept += std::abs(plain[k] - smoothed[k]) <= 1.0 ? 1U : 0U;
                    }
                }
                const double kept_share = static_cast<double>(kept) / static_cast<double>(detail_pixels);
                std::printf("render-check: control: the bands' largest step between columns is %.3f of 255 without debanding, %.3f with; %.1f%% of the edge and noise kept\n",
                            plain_error, smoothed_error, 100.0 * kept_share);
                if (!(smoothed_error < 0.5 * plain_error) || plain_error < 1.0 || kept_share < 0.9) {
                    std::printf("render-check: the control failed\n");
                    code = 1;
                }
            }

            // 3. What it costs.
            if (!broken && time && timing != VK_NULL_HANDLE) {
                const GrayPicture console = gray_picture(1920, 1080, [](std::uint32_t x, std::uint32_t y) { return 16 + static_cast<int>(media::hash32(x + y * 1920U) % 220U); });
                pass.set_deband(true);
                for (const media::Scaler scaler : media::kScalers) {
                    pass.set_scaler(scaler);
                    std::vector<double> convert_ms, draw_ms;
                    for (int i = 0; i < 7 && !broken; ++i) {
                        if (!draw_at(console, {3440, 1440}, timing)) {
                            code = fail(1, "the 1080p picture could not be drawn");
                            broken = true;
                            break;
                        }
                        std::uint64_t stamps[3]{};
                        if (vkGetQueryPoolResults(device, timing, 0, 3, sizeof stamps, stamps, sizeof stamps[0], VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) break;
                        const double period = static_cast<double>(properties.limits.timestampPeriod) / 1e6;
                        convert_ms.push_back(static_cast<double>(stamps[1] - stamps[0]) * period);
                        draw_ms.push_back(static_cast<double>(stamps[2] - stamps[1]) * period);
                    }
                    if (convert_ms.size() != 7) break;
                    std::ranges::sort(convert_ms);
                    std::ranges::sort(draw_ms);
                    std::printf("render-check: time: 1920x1080 to %ux%u by %s: converting (with the copy and debanding) %.3f ms, drawing %.3f ms\n", pass.placement().width,
                                pass.placement().height, std::string(media::scaler_name(scaler)).c_str(), convert_ms[3], draw_ms[3]);
                }
            }
            vkDeviceWaitIdle(device);
            pass.destroy();
        }
        if (timing != VK_NULL_HANDLE) vkDestroyQueryPool(device, timing, nullptr);
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
    }
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--scaling") {
        if (argc > 3 || (argc == 3 && std::string_view(argv[2]) != "--time")) return fail(2, "usage: psvp_render_check --scaling [--time]");
        return run_scaling(argc == 3);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "--pattern") {
        std::vector<double> peaks;
        for (int i = 2; i + 1 < argc; i += 2) {
            if (std::string_view(argv[i]) != "--peak") return fail(2, "unknown option {}", argv[i]);
            peaks.push_back(std::atof(argv[i + 1]));
        }
        if (peaks.empty()) peaks.push_back(media::kAssumedHdrPeakNits);
        return run_pattern(peaks);
    }
    if (argc < 2) return fail(2, "usage: psvp_render_check FOLDER|CLIP [--every N] [--checks N] | --pattern [--peak NITS]... | --scaling [--time]");
    const std::filesystem::path folder(argv[1]);
    unsigned every = 10;
    int checks = -1;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::string_view(argv[i]) == "--every") every = static_cast<unsigned>(std::max(1, std::atoi(argv[i + 1])));
        else if (std::string_view(argv[i]) == "--checks") checks = std::max(1, std::atoi(argv[i + 1]));
        else return fail(2, "unknown option {}", argv[i]);
    }

    const auto read = [&](const std::filesystem::path& name) -> std::optional<std::string> {
        std::ifstream file(name, std::ios::binary);
        if (!file) return std::nullopt;
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    std::optional<std::string> video_bytes, video_list;
    if (std::filesystem::is_regular_file(folder)) {
        // A clip: the first three units are the header, every unit after them a frame. Laid out as a dump is.
        video_bytes = read(folder);
        if (!video_bytes) return fail(2, "{} cannot be read", folder.string());
        std::vector<std::size_t> starts;
        const std::string& bytes = *video_bytes;
        for (std::size_t i = 0; i + 3 <= bytes.size(); ++i) {
            if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) starts.push_back(i > 0 && bytes[i - 1] == 0 ? i - 1 : i);
        }
        if (starts.size() < 4) return fail(2, "{} has no frames after its parameter sets", folder.string());
        std::string list = "frame\tbytes\trepaired\tlost-before\tpicture-size\theader-bytes\n";
        for (std::size_t i = 3; i < starts.size(); ++i) {
            const std::size_t end = i + 1 < starts.size() ? starts[i + 1] : bytes.size();
            list += std::format("{}\t{}\t0\t0\t0\t{}\n", i - 2, end - starts[i], i == 3 ? starts[3] : 0);
        }
        video_list = std::move(list);
    } else {
        video_bytes = read(folder / "video.bin");
        video_list = read(folder / "video.tsv");
    }
    if (!video_bytes || !video_list) return fail(2, "{} is not a folder that 'session --dump' wrote", folder.string());

    // Vulkan: the instance, the first device that can decode and draw.
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "psvp_render_check";
    app.apiVersion = render::kVideoApi;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (!good(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance")) return 3;
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(instance, &count, physicals.data());
    std::optional<render::MadeDevice> made;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    std::string reasons;
    for (const VkPhysicalDevice candidate : physicals) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        const auto graphics = std::ranges::find_if(families, [](const VkQueueFamilyProperties& f) { return (f.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0; });
        if (graphics == families.end()) continue;
        const auto index = static_cast<std::uint32_t>(graphics - families.begin());
        auto attempt = render::make_device(render::DeviceRequest{instance, render::kVideoApi, {}, candidate, properties.apiVersion, index, properties.deviceName, true, {}});
        if (!attempt) continue;
        if (!attempt->why_not.empty()) {
            reasons += std::format(" {}: {};", properties.deviceName, attempt->why_not);
            vkDestroyDevice(attempt->device, nullptr);
            continue;
        }
        made = std::move(*attempt);
        physical = candidate;
        family = index;
        break;
    }
    if (!made) {
        vkDestroyInstance(instance, nullptr);
        return fail(3, "no device here decodes on the graphics card:{}", reasons.empty() ? std::string(" none found") : reasons);
    }
    const VkDevice device = made->device;
    int code = 1;
    {
        auto gpu = media::share_device(made->share);
        if (!gpu) return fail(3, "{}", gpu.error());
        std::printf("render-check: decoding and drawing on %s\n", gpu->description().c_str());

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = family;
        VkCommandPool pool = VK_NULL_HANDLE;
        vkCreateCommandPool(device, &pool_info, nullptr, &pool);
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device, &allocate, &commands);
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(device, &fence_info, nullptr, &fence);
        {
            render::VideoPass gpu_pass, software_pass;
            Target gpu_target(physical, device), software_target(physical, device);
            if (!gpu_pass.create(physical, device, true) || !software_pass.create(physical, device, true) || !gpu_target.create_pass() ||
                !software_target.create_pass() || !gpu_pass.create_pipeline(gpu_target.pass(), false) ||
                !software_pass.create_pipeline(software_target.pass(), false)) {
                return fail(1, "the passes could not be set up");
            }

            std::optional<media::VideoDecoder> decoder;
            std::map<std::uint64_t, std::string> headers;
            std::size_t at = 0;
            std::uint64_t card_pictures = 0, drawn = 0, same = 0, unlike = 0, unlike_picture = 0;
            unsigned largest = 0, largest_from_picture = 0;
            double largest_delta_e = 0.0, largest_apart_delta_e = 0.0;  // of HDR pictures
            std::string first_unlike;
            for (const auto& row : rows(*video_list)) {
                if (row.size() < 6 || row[5] > video_bytes->size() - at || row[1] > video_bytes->size() - at - row[5]) return fail(1, "video.tsv does not fit video.bin");
                if (row[5] != 0) headers[row[4]] = video_bytes->substr(at, row[5]);
                at += row[5];
                const std::span<const std::uint8_t> frame(reinterpret_cast<const std::uint8_t*>(video_bytes->data()) + at, row[1]);
                at += row[1];
                if (!decoder) {
                    const std::string& header = headers[row[4]];
                    const std::span<const std::uint8_t> header_bytes(reinterpret_cast<const std::uint8_t*>(header.data()), header.size());
                    const auto codec = media::codec_of_header(header_bytes);
                    if (!codec) return fail(1, "the stream's header is neither H.264's nor HEVC's");
                    auto opened = media::VideoDecoder::open(*codec, header_bytes, &*gpu, checks);
                    if (!opened) return fail(1, "{}", opened.error());
                    decoder.emplace(std::move(*opened));
                }
                auto decoded = decoder->decode(static_cast<std::uint16_t>(row[0]), frame);
                if (!decoded.picture || !decoded.picture->on_gpu) continue;
                if (card_pictures++ % every != 0) continue;
                const media::Picture& on_card = *decoded.picture;
                const auto read_back = media::download(on_card);
                if (!read_back) return fail(1, "picture {} could not be read back", row[0]);
                if (!draw(device, made->queue, family, &*gpu, commands, fence, gpu_pass, gpu_target, on_card) ||
                    !draw(device, made->queue, family, &*gpu, commands, fence, software_pass, software_target, *read_back)) {
                    return fail(1, "picture {} could not be drawn", row[0]);
                }
                ++drawn;
                // The software way's pixels are the picture's (else two broken draws would agree) ...
                const bool hdr = read_back->transfer != media::ColorTransfer::sdr;
                const Off off = off_from_picture(*read_back, software_target.pixels());
                largest_from_picture = std::max(largest_from_picture, off.steps);
                largest_delta_e = std::max(largest_delta_e, off.delta_e);
                // Too few even places to say it is the picture counts as not the picture.
                if ((hdr ? off.delta_e > kMostDeltaE : off.steps > 2) || off.looked < 16) ++unlike_picture;
                // ... and the card's are the same as those.
                unsigned apart = 0;
                double apart_delta_e = 0.0;
                const std::size_t bytes = std::size_t{on_card.width} * on_card.height * 4;
                const std::uint8_t* const card = gpu_target.pixels();
                const std::uint8_t* const soft = software_target.pixels();
                for (std::size_t i = 0; i < bytes; i += 4) {
                    unsigned here = 0;
                    for (std::size_t c = 0; c < 3; ++c) here = std::max(here, static_cast<unsigned>(std::abs(card[i + c] - soft[i + c])));
                    apart = std::max(apart, here);
                    if (hdr && here > 1) {
                        apart_delta_e = std::max(apart_delta_e, media::delta_e_2000(media::lab_of_srgb(card[i], card[i + 1], card[i + 2]),
                                                                                    media::lab_of_srgb(soft[i], soft[i + 1], soft[i + 2])));
                    }
                }
                largest = std::max(largest, apart);
                largest_apart_delta_e = std::max(largest_apart_delta_e, apart_delta_e);
                // PSVP_RENDER_CHECK_SAVE=FOLDER: the first pair drawn, and the first pair unlike, as pictures to look at.
                const auto save = [&](std::uint64_t number) {
                    const char* to = std::getenv("PSVP_RENDER_CHECK_SAVE");
                    if (to == nullptr) return;
                    for (const auto& [name, pixels] : {std::pair{"card", gpu_target.pixels()}, std::pair{"software", software_target.pixels()}}) {
                        std::ofstream out(std::filesystem::path(to) / std::format("{}-{}.ppm", name, number), std::ios::binary);
                        out << std::format("P6\n{} {}\n255\n", on_card.width, on_card.height);
                        for (std::size_t i = 0; i < bytes; i += 4) out.write(reinterpret_cast<const char*>(pixels + i), 3);
                    }
                };
                if (drawn == 1) save(row[0]);
                if (hdr ? apart_delta_e <= kMostDeltaE : apart <= 1) {
                    ++same;
                } else {
                    ++unlike;
                    if (first_unlike.empty()) {
                        first_unlike = std::format(" (the first: frame {}, off by {}, CIEDE2000 {:.2f})", row[0], apart, apart_delta_e);
                        save(row[0]);
                    }
                }
            }
            const bool tone_mapped = largest_delta_e > 0.0 || largest_apart_delta_e > 0.0;
            std::printf("render-check: %s; %llu pictures came out on the card, %llu drawn both ways: %llu the same %s, %llu not%s "
                        "(the largest difference %u%s); the software way's pixels were the picture's colors within %u%s (%llu off by more than %s, or with too few even places to tell)\n",
                        decoder ? decoder->description().c_str() : "nothing decoded", static_cast<unsigned long long>(card_pictures),
                        static_cast<unsigned long long>(drawn), static_cast<unsigned long long>(same),
                        tone_mapped ? "to the eye (HDR, tone mapped: CIEDE2000 at most 1)" : "within 1 of 255", static_cast<unsigned long long>(unlike), first_unlike.c_str(),
                        largest, tone_mapped ? std::format(", CIEDE2000 {:.2f}", largest_apart_delta_e).c_str() : "", largest_from_picture,
                        tone_mapped ? std::format(" (CIEDE2000 {:.2f})", largest_delta_e).c_str() : "", static_cast<unsigned long long>(unlike_picture),
                        tone_mapped ? "CIEDE2000 1" : "2");
            code = drawn != 0 && unlike == 0 && unlike_picture == 0 ? 0 : 1;
            decoder.reset();
            vkDeviceWaitIdle(device);
            gpu_pass.destroy();
            software_pass.destroy();
        }
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
    }
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return code;
}
