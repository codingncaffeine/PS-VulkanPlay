#include "media/gpu_device.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#if PSVP_HAVE_VULKAN_HEADERS
#include "media/gpu_share.h"
#endif
#if PSVP_GPU_DECODE
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
#include <libavutil/version.h>
}
// The device's queue list (FFmpeg 7.1) and the queue locks a lent device needs (gone in FFmpeg's next major version).
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 34, 100) && LIBAVUTIL_VERSION_MAJOR < 62
#define PSVP_GPU_SHARING 1
#endif
#endif
#ifndef PSVP_GPU_SHARING
#define PSVP_GPU_SHARING 0
#endif

namespace psvp::media {

namespace {

// A picture's own copy of its three planes.
struct OwnedPlanes {
    std::vector<std::uint8_t> bytes[3];
};

}  // namespace

std::optional<PictureDifference> compare_pictures(const Picture& a, const Picture& b) {
    if (a.on_gpu || b.on_gpu || a.width != b.width || a.height != b.height || a.bits != b.bits || a.width == 0 || a.planes[0] == nullptr ||
        b.planes[0] == nullptr) {
        return std::nullopt;
    }
    PictureDifference result;
    const unsigned sample_bytes = a.bits > 8 ? 2 : 1;
    for (int plane = 0; plane < 3; ++plane) {
        const unsigned width = plane == 0 ? a.width : (a.width + 1) / 2;
        const unsigned height = plane == 0 ? a.height : (a.height + 1) / 2;
        for (unsigned y = 0; y < height; ++y) {
            const std::uint8_t* row_a = a.planes[plane] + std::size_t{y} * a.strides[plane];
            const std::uint8_t* row_b = b.planes[plane] + std::size_t{y} * b.strides[plane];
            for (unsigned x = 0; x < width; ++x) {
                unsigned sample_a = 0, sample_b = 0;
                if (sample_bytes == 1) {
                    sample_a = row_a[x];
                    sample_b = row_b[x];
                } else {
                    std::uint16_t wide_a = 0, wide_b = 0;
                    std::memcpy(&wide_a, row_a + 2 * x, 2);
                    std::memcpy(&wide_b, row_b + 2 * x, 2);
                    sample_a = wide_a;
                    sample_b = wide_b;
                }
                ++result.samples;
                if (sample_a != sample_b) {
                    ++result.different;
                    result.largest = std::max(result.largest, sample_a > sample_b ? sample_a - sample_b : sample_b - sample_a);
                }
            }
        }
    }
    return result;
}

#if PSVP_GPU_SHARING

namespace {

// What FFmpeg's device context points at, and keeps alive until it is freed.
struct Shared {
    std::vector<std::string> instance_extensions, device_extensions;
    std::vector<const char*> instance_names, device_names;
    VkPhysicalDeviceVulkan11Features v11{};
    VkPhysicalDeviceVulkan12Features v12{};
    VkPhysicalDeviceVulkan13Features v13{};
    VkPhysicalDeviceVideoMaintenance1FeaturesKHR video_maintenance1{};
    // One lock per queue, made up front: only looked up from then on.
    std::unordered_map<std::uint64_t, std::unique_ptr<std::mutex>> queues;
    std::mutex& queue(std::uint32_t family, std::uint32_t index) {
        static std::mutex unknown;  // a queue nobody listed: still never used by two at once
        const auto found = queues.find((std::uint64_t{family} << 32) | index);
        return found != queues.end() ? *found->second : unknown;
    }
};

Shared* shared_of(AVHWDeviceContext* context) {
    return static_cast<std::shared_ptr<Shared>*>(context->user_opaque)->get();
}
void lock_queue_for_ffmpeg(AVHWDeviceContext* context, std::uint32_t family, std::uint32_t index) {
    shared_of(context)->queue(family, index).lock();
}
void unlock_queue_for_ffmpeg(AVHWDeviceContext* context, std::uint32_t family, std::uint32_t index) {
    shared_of(context)->queue(family, index).unlock();
}
void free_shared(AVHWDeviceContext* context) {
    delete static_cast<std::shared_ptr<Shared>*>(context->user_opaque);
    context->user_opaque = nullptr;
}

std::string error_text(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return text;
}

std::string codecs_text(const AVVulkanDeviceContext& device) {
    VkVideoCodecOperationFlagsKHR decodes = 0;
    for (int i = 0; i < device.nb_qf; ++i) {
        if ((device.qf[i].flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) != 0) decodes |= static_cast<VkVideoCodecOperationFlagsKHR>(device.qf[i].video_caps);
    }
    std::string text;
    if ((decodes & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) != 0) text = "H.264";
    if ((decodes & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) != 0) text += text.empty() ? "HEVC" : ", HEVC";
    return text.empty() ? std::string("no video it can decode") : text;
}

}  // namespace

struct GpuDevice::State {
    AVBufferRef* device = nullptr;
    std::shared_ptr<Shared> shared;  // for a device lent by the window; none for FFmpeg's own
    std::string description;
    ~State() { av_buffer_unref(&device); }
};

std::optional<std::string> GpuDevice::unavailable_reason() { return std::nullopt; }

std::expected<GpuDevice, std::string> GpuDevice::create_own() {
    auto state = std::make_shared<State>();
    if (const int result = av_hwdevice_ctx_create(&state->device, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0); result < 0) {
        return std::unexpected(std::format("FFmpeg found no Vulkan device to decode on: {}", error_text(result)));
    }
    const auto* context = reinterpret_cast<AVHWDeviceContext*>(state->device->data);
    const auto* device = static_cast<const AVVulkanDeviceContext*>(context->hwctx);
    std::string name = "a Vulkan device";
    if (device->get_proc_addr != nullptr) {
        const auto properties_of =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(device->get_proc_addr(device->inst, "vkGetPhysicalDeviceProperties"));
        if (properties_of != nullptr) {
            VkPhysicalDeviceProperties properties{};
            properties_of(device->phys_dev, &properties);
            name = properties.deviceName;
        }
    }
    state->description = std::format("{} ({})", name, codecs_text(*device));
    return GpuDevice(std::move(state));
}

std::expected<GpuDevice, std::string> share_device(const VulkanShare& share) {
    auto state = std::make_shared<GpuDevice::State>();
    state->device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (state->device == nullptr) return std::unexpected(std::string("this FFmpeg was built without Vulkan"));
    auto* context = reinterpret_cast<AVHWDeviceContext*>(state->device->data);
    auto* device = static_cast<AVVulkanDeviceContext*>(context->hwctx);

    auto shared = std::make_shared<Shared>();
    shared->instance_extensions = share.instance_extensions;
    shared->device_extensions = share.device_extensions;
    for (const std::string& name : shared->instance_extensions) shared->instance_names.push_back(name.c_str());
    for (const std::string& name : shared->device_extensions) shared->device_names.push_back(name.c_str());
    for (const VulkanQueueFamily& family : share.families) {
        for (std::uint32_t index = 0; index < family.count; ++index) {
            shared->queues.emplace((std::uint64_t{family.index} << 32) | index, std::make_unique<std::mutex>());
        }
    }
    // The features as they were enabled, for FFmpeg to read.
    shared->v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    shared->v11.samplerYcbcrConversion = share.sampler_ycbcr_conversion ? VK_TRUE : VK_FALSE;
    shared->v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    shared->v12.timelineSemaphore = share.timeline_semaphore ? VK_TRUE : VK_FALSE;
    shared->v12.hostQueryReset = share.host_query_reset ? VK_TRUE : VK_FALSE;
    shared->v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    shared->v13.synchronization2 = share.synchronization2 ? VK_TRUE : VK_FALSE;
    shared->video_maintenance1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR;
    shared->video_maintenance1.videoMaintenance1 = share.video_maintenance1 ? VK_TRUE : VK_FALSE;
    shared->v11.pNext = &shared->v12;
    shared->v12.pNext = &shared->v13;
    shared->v13.pNext = share.video_maintenance1 ? &shared->video_maintenance1 : nullptr;

    device->get_proc_addr = share.get_proc_addr;
    device->inst = share.instance;
    device->phys_dev = share.physical;
    device->act_dev = share.device;
    device->device_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    device->device_features.pNext = &shared->v11;
    device->enabled_inst_extensions = shared->instance_names.empty() ? nullptr : shared->instance_names.data();
    device->nb_enabled_inst_extensions = static_cast<int>(shared->instance_names.size());
    device->enabled_dev_extensions = shared->device_names.empty() ? nullptr : shared->device_names.data();
    device->nb_enabled_dev_extensions = static_cast<int>(shared->device_names.size());
    device->nb_qf = 0;
    for (const VulkanQueueFamily& family : share.families) {
        if (device->nb_qf == static_cast<int>(std::size(device->qf))) break;
        AVVulkanDeviceQueueFamily& entry = device->qf[device->nb_qf++];
        entry.idx = static_cast<int>(family.index);
        entry.num = static_cast<int>(family.count);
        entry.flags = static_cast<VkQueueFlagBits>(family.flags);
        entry.video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(family.video);
    }
    // The window submits to the same queue as FFmpeg's own work on the frames: one lock for both.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    device->lock_queue = lock_queue_for_ffmpeg;
    device->unlock_queue = unlock_queue_for_ffmpeg;
#pragma GCC diagnostic pop
    context->user_opaque = new std::shared_ptr<Shared>(shared);
    context->free = free_shared;
    state->shared = shared;

    if (const int result = av_hwdevice_ctx_init(state->device); result < 0) {
        return std::unexpected(std::format("FFmpeg cannot decode on {}: {}", share.device_name, error_text(result)));
    }
    state->description = std::format("{} ({})", share.device_name, codecs_text(*device));
    return GpuDevice(std::move(state));
}

void GpuDevice::lock_queue(std::uint32_t family, std::uint32_t index) const {
    if (state_->shared) state_->shared->queue(family, index).lock();
}
void GpuDevice::unlock_queue(std::uint32_t family, std::uint32_t index) const {
    if (state_->shared) state_->shared->queue(family, index).unlock();
}
const std::string& GpuDevice::description() const noexcept { return state_->description; }
void* GpuDevice::context() const noexcept { return state_->device; }
long GpuDevice::users() const noexcept { return state_.use_count() - 1 + av_buffer_get_ref_count(state_->device); }

namespace {

AVFrame* frame_of(const Picture& picture) {
    return picture.on_gpu && picture.keep ? static_cast<AVFrame*>(picture.keep.get()) : nullptr;
}

}  // namespace

std::optional<Picture> download(const Picture& picture) {
    AVFrame* frame = frame_of(picture);
    if (frame == nullptr) return std::nullopt;
    AVFrame* copy = av_frame_alloc();
    if (copy == nullptr) return std::nullopt;
    const std::unique_ptr<AVFrame, void (*)(AVFrame*)> owned(copy, [](AVFrame* f) { av_frame_free(&f); });
    if (av_hwframe_transfer_data(copy, frame, 0) < 0) return std::nullopt;
    const AVPixFmtDescriptor* layout = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(copy->format));
    // The two-plane layouts a video decoder writes: brightness, then the two color differences side by side.
    if (layout == nullptr || layout->nb_components != 3 || layout->log2_chroma_w != 1 || layout->log2_chroma_h != 1 ||
        (layout->flags & (AV_PIX_FMT_FLAG_PLANAR | AV_PIX_FMT_FLAG_BE)) != AV_PIX_FMT_FLAG_PLANAR || layout->comp[1].plane != 1 ||
        layout->comp[2].plane != 1) {
        return std::nullopt;
    }
    const unsigned bits = static_cast<unsigned>(layout->comp[0].depth);
    const unsigned shift = static_cast<unsigned>(layout->comp[0].shift);
    const unsigned sample_bytes = bits > 8 ? 2 : 1;
    const unsigned width = static_cast<unsigned>(copy->width), height = static_cast<unsigned>(copy->height);
    const unsigned color_width = (width + 1) / 2, color_height = (height + 1) / 2;

    auto planes = std::make_shared<OwnedPlanes>();
    planes->bytes[0].resize(std::size_t{width} * height * sample_bytes);
    planes->bytes[1].resize(std::size_t{color_width} * color_height * sample_bytes);
    planes->bytes[2].resize(planes->bytes[1].size());
    const auto sample = [&](const std::uint8_t* row, unsigned x) -> unsigned {
        if (sample_bytes == 1) return row[x];
        std::uint16_t value = 0;
        std::memcpy(&value, row + 2 * x, 2);
        return static_cast<unsigned>(value) >> shift;
    };
    const auto put = [&](std::vector<std::uint8_t>& to, std::size_t at, unsigned value) {
        if (sample_bytes == 1) {
            to[at] = static_cast<std::uint8_t>(value);
        } else {
            const auto wide = static_cast<std::uint16_t>(value);
            std::memcpy(to.data() + 2 * at, &wide, 2);
        }
    };
    for (unsigned y = 0; y < height; ++y) {
        const std::uint8_t* row = copy->data[0] + std::size_t{y} * static_cast<std::size_t>(copy->linesize[0]);
        for (unsigned x = 0; x < width; ++x) put(planes->bytes[0], std::size_t{y} * width + x, sample(row, x));
    }
    const unsigned cb_at = static_cast<unsigned>(layout->comp[1].offset) / sample_bytes;
    const unsigned cr_at = static_cast<unsigned>(layout->comp[2].offset) / sample_bytes;
    for (unsigned y = 0; y < color_height; ++y) {
        const std::uint8_t* row = copy->data[1] + std::size_t{y} * static_cast<std::size_t>(copy->linesize[1]);
        for (unsigned x = 0; x < color_width; ++x) {
            put(planes->bytes[1], std::size_t{y} * color_width + x, sample(row, 2 * x + cb_at));
            put(planes->bytes[2], std::size_t{y} * color_width + x, sample(row, 2 * x + cr_at));
        }
    }

    Picture result = picture;
    result.on_gpu = false;
    result.bits = bits;
    for (int i = 0; i < 3; ++i) {
        result.planes[i] = planes->bytes[i].data();
        result.strides[i] = std::size_t{i == 0 ? width : color_width} * sample_bytes;
    }
    result.keep = planes;
    return result;
}

std::expected<GpuImageUse, std::string> begin_use(const Picture& picture) {
    AVFrame* frame = frame_of(picture);
    if (frame == nullptr || frame->format != AV_PIX_FMT_VULKAN || frame->hw_frames_ctx == nullptr) {
        return std::unexpected(std::string("the picture is not one decoded on the graphics card"));
    }
    auto* frames = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
    auto* vulkan_frames = static_cast<AVVulkanFramesContext*>(frames->hwctx);
    auto* image = reinterpret_cast<AVVkFrame*>(frame->data[0]);
    const AVPixFmtDescriptor* layout = av_pix_fmt_desc_get(frames->sw_format);
    if (image == nullptr || image->img[1] != VK_NULL_HANDLE || layout == nullptr || layout->nb_components != 3 || layout->comp[1].plane != 1 ||
        layout->comp[2].plane != 1) {
        return std::unexpected(std::format("the graphics card's pictures are in a layout this window does not draw ({})",
                                           layout != nullptr ? layout->name : "unknown"));
    }
    vulkan_frames->lock_frame(frames, image);
    GpuImageUse use;
    use.image = image->img[0];
    use.layout = image->layout[0];
    use.access = static_cast<VkAccessFlags2>(image->access[0]);
    use.semaphore = image->sem[0];
    use.wait_value = image->sem_value[0];
    use.family = image->queue_family[0];
    use.wide = layout->comp[0].depth > 8;
    use.image_width = static_cast<std::uint32_t>(frames->width);
    use.image_height = static_cast<std::uint32_t>(frames->height);
    return use;
}

void finish_use(const Picture& picture, bool submitted, VkImageLayout layout, VkAccessFlags2 access) {
    AVFrame* frame = frame_of(picture);
    if (frame == nullptr) return;
    auto* frames = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
    auto* vulkan_frames = static_cast<AVVulkanFramesContext*>(frames->hwctx);
    auto* image = reinterpret_cast<AVVkFrame*>(frame->data[0]);
    if (submitted) {
        image->layout[0] = layout;
        // Its type is VkAccessFlagBits before FFmpeg 8 and VkAccessFlagBits2 since: the field's own type.
        image->access[0] = static_cast<std::remove_reference_t<decltype(image->access[0])>>(access);
        image->sem_value[0] += 1;
    }
    vulkan_frames->unlock_frame(frames, image);
}

#else  // decoding on a graphics card is not built in

struct GpuDevice::State {
    std::string description;
};

std::optional<std::string> GpuDevice::unavailable_reason() {
#if !PSVP_GPU_DECODE
    return std::string("this program was built without decoding on a graphics card (FFmpeg without Vulkan, or no Vulkan headers)");
#else
    return std::string("this program was built with an FFmpeg older than 7.1, whose Vulkan device cannot be shared");
#endif
}
std::expected<GpuDevice, std::string> GpuDevice::create_own() { return std::unexpected(*unavailable_reason()); }
void GpuDevice::lock_queue(std::uint32_t, std::uint32_t) const {}
void GpuDevice::unlock_queue(std::uint32_t, std::uint32_t) const {}
const std::string& GpuDevice::description() const noexcept { return state_->description; }
void* GpuDevice::context() const noexcept { return nullptr; }
long GpuDevice::users() const noexcept { return state_.use_count(); }
std::optional<Picture> download(const Picture&) { return std::nullopt; }

#if PSVP_HAVE_VULKAN_HEADERS
std::expected<GpuDevice, std::string> share_device(const VulkanShare&) { return std::unexpected(*GpuDevice::unavailable_reason()); }
std::expected<GpuImageUse, std::string> begin_use(const Picture&) { return std::unexpected(*GpuDevice::unavailable_reason()); }
void finish_use(const Picture&, bool, VkImageLayout, VkAccessFlags2) {}
#endif

#endif

}  // namespace psvp::media
