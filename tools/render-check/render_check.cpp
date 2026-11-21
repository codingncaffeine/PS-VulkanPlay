// Draws pictures decoded on the graphics card the way the stream window draws them (its own
// VideoPass, from the card's memory, with the decoder's semaphores) into an image nobody sees,
// and compares every pixel with the same samples drawn the software way, read back from the card.
// A stream window on a virtual screen cannot do this: the drivers of graphics cards that decode
// do not present to a virtual X server.
//
//   psvp_render_check FOLDER|CLIP [--every N] [--checks N]
//       FOLDER as 'session --dump' wrote it, or CLIP: a stream file laid out as tests/data/clip.h265
//       (its parameter sets, then a frame per unit); every Nth picture of the card is drawn twice
//       (10); --checks: how many of the card's first pictures the decoder compares (as when playing)
//
// Exit: 0 every picture the same both ways | 1 not | 2 usage | 3 this machine cannot decode on a
// graphics card (nothing checked)
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
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
#include "media/gpu_device.h"
#include "media/gpu_share.h"
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
    Target(VkPhysicalDevice physical, VkDevice device) : physical_(physical), device_(device) {}
    ~Target() { destroy(); }
    Target(const Target&) = delete;
    Target& operator=(const Target&) = delete;

    bool create_pass() {
        VkAttachmentDescription color{};
        color.format = VK_FORMAT_R8G8B8A8_UNORM;
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
        image.format = VK_FORMAT_R8G8B8A8_UNORM;
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
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
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
        buffer.size = VkDeviceSize{width} * height * 4;
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

// Draws one picture into the target as the window does, and waits until it is there.
bool draw(VkDevice device, VkQueue queue, std::uint32_t family, const media::GpuDevice& gpu, VkCommandBuffer commands, VkFence fence,
          render::VideoPass& video, Target& target, const media::Picture& picture) {
    if (!video.set_picture(picture)) return false;
    if (!target.size(picture.width, picture.height)) return false;
    if (!good(vkResetCommandBuffer(commands, 0), "vkResetCommandBuffer")) return false;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!good(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer")) return false;
    bool submitted = false;
    video.record_upload(commands);
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
    gpu.lock_queue(family, 0);
    const VkResult queued = vkQueueSubmit(queue, 1, &submit, fence);
    gpu.unlock_queue(family, 0);
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
// The largest difference, and how many places were looked at.
std::pair<unsigned, unsigned> off_from_picture(const media::Picture& picture, const std::uint8_t* pixels) {
    const media::ColorTransform transform = media::color_transform(picture.matrix, picture.full_range, picture.bits);
    unsigned worst = 0, looked = 0;
    const auto sample = [&](int plane, unsigned x, unsigned y) -> int {
        const std::uint8_t* row = picture.planes[plane] + std::size_t{y} * picture.strides[plane];
        if (picture.bits == 8) return row[x];
        std::uint16_t value = 0;
        std::memcpy(&value, row + 2 * x, 2);
        return value;
    };
    const unsigned color_width = (picture.width + 1) / 2, color_height = (picture.height + 1) / 2;
    const int flat = 1 << (picture.bits - 8);  // one step of an 8-bit sample
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
                                                      static_cast<unsigned>(sample(2, cx, cy)));
            const std::uint8_t* pixel = pixels + (std::size_t{y} * picture.width + x) * 4;
            for (int c = 0; c < 3; ++c) worst = std::max(worst, static_cast<unsigned>(std::abs(expected[static_cast<std::size_t>(c)] - pixel[c])));
            ++looked;
        }
    }
    return {worst, looked};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return fail(2, "usage: psvp_render_check FOLDER|CLIP [--every N] [--checks N]");
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
                if (!draw(device, made->queue, family, *gpu, commands, fence, gpu_pass, gpu_target, on_card) ||
                    !draw(device, made->queue, family, *gpu, commands, fence, software_pass, software_target, *read_back)) {
                    return fail(1, "picture {} could not be drawn", row[0]);
                }
                ++drawn;
                // The software way's pixels are the picture's (else two broken draws would agree) ...
                const auto [from_picture, looked] = off_from_picture(*read_back, software_target.pixels());
                largest_from_picture = std::max(largest_from_picture, from_picture);
                // Too few even places to say it is the picture counts as not the picture.
                if (from_picture > 2 || looked < 16) ++unlike_picture;
                // ... and the card's are the same as those.
                unsigned apart = 0;
                const std::size_t bytes = std::size_t{on_card.width} * on_card.height * 4;
                for (std::size_t i = 0; i < bytes; ++i) {
                    if ((i & 3) == 3) continue;
                    apart = std::max(apart, static_cast<unsigned>(std::abs(gpu_target.pixels()[i] - software_target.pixels()[i])));
                }
                largest = std::max(largest, apart);
                if (apart <= 1) {
                    ++same;
                } else {
                    ++unlike;
                    if (first_unlike.empty()) {
                        first_unlike = std::format(" (the first: frame {}, off by {})", row[0], apart);
                        // PSVP_RENDER_CHECK_SAVE=FOLDER: that pair as pictures to look at.
                        if (const char* save = std::getenv("PSVP_RENDER_CHECK_SAVE")) {
                            for (const auto& [name, pixels] : {std::pair{"card", gpu_target.pixels()}, std::pair{"software", software_target.pixels()}}) {
                                std::ofstream out(std::filesystem::path(save) / std::format("{}-{}.ppm", name, row[0]), std::ios::binary);
                                out << std::format("P6\n{} {}\n255\n", on_card.width, on_card.height);
                                for (std::size_t i = 0; i < bytes; i += 4) out.write(reinterpret_cast<const char*>(pixels + i), 3);
                            }
                        }
                    }
                }
            }
            std::printf("render-check: %s; %llu pictures came out on the card, %llu drawn both ways: %llu the same within 1 of 255, %llu not%s "
                        "(the largest difference %u); the software way's pixels were the picture's colors within %u (%llu off by more than 2, or with too few even places to tell)\n",
                        decoder ? decoder->description().c_str() : "nothing decoded", static_cast<unsigned long long>(card_pictures),
                        static_cast<unsigned long long>(drawn), static_cast<unsigned long long>(same), static_cast<unsigned long long>(unlike),
                        first_unlike.c_str(), largest, largest_from_picture, static_cast<unsigned long long>(unlike_picture));
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
