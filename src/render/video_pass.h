// Draws a decoded picture into the window, in two steps: its three planes (copied into textures, or
// read from a graphics card decoder's image) are converted to red, green and blue at the picture's
// own size, debanded where asked for (convert.frag, once per picture), and that is drawn at the
// size it takes in the window by the scaler chosen, then tone mapped (video.frag, every frame).
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "media/color.h"
#include "media/gpu_share.h"
#include "media/peak_detect.h"
#include "media/scaler.h"
#include "media/video_decoder.h"

namespace psvp::render {

class VideoPass {
public:
    VideoPass() = default;
    VideoPass(const VideoPass&) = delete;
    VideoPass& operator=(const VideoPass&) = delete;

    // What does not depend on the swapchain. False (with the reason logged) when a call failed.
    // `gpu_pictures`: the device can draw pictures decoded on it (Vulkan 1.3, synchronization2).
    // `compute`: the queue the pass is recorded for runs compute work (measuring HDR pictures).
    [[nodiscard]] bool create(VkPhysicalDevice physical, VkDevice device, bool gpu_pictures = false, bool compute = true);
    // The pipeline for a render pass; `target_encodes` when the target's format encodes what is written to it.
    [[nodiscard]] bool create_pipeline(VkRenderPass render_pass, bool target_encodes);
    void destroy_pipeline();
    void destroy();

    // Takes a picture to show from now on. Only while no command buffer that uses this pass is
    // under way: the textures are made anew when the picture's size or depth is another.
    [[nodiscard]] bool set_picture(const media::Picture& picture);
    [[nodiscard]] bool has_picture() const noexcept { return width_ != 0; }
    // Copies the picture taken last into the textures, or, for a picture decoded on the graphics
    // card, makes its image ready to be read; then converts it (its own render pass). Only once
    // per picture, or after a setting it depends on changed. Outside a render pass.
    void record_upload(VkCommandBuffer commands);
    // For a picture decoded on the graphics card, what the submission of this frame must wait for
    // (from `record_upload` on) and signal: its frame stays locked until `after_submit`.
    struct GpuWait {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        std::uint64_t wait = 0, signal = 0;
    };
    [[nodiscard]] std::optional<GpuWait> gpu_wait() const;
    // Right after the frame's submission (`submitted`: it went to the queue).
    void after_submit(bool submitted);
    // Draws the picture as large as it fits into the target. Inside the render pass.
    void record_draw(VkCommandBuffer commands, VkExtent2D target);
    // How bright a PQ picture is taken to be at its brightest, in cd/m² (the console's streams do not
    // say): what is brighter than SDR white is fitted between it and that (media/tone_map.h).
    void set_hdr_peak(double nits) noexcept {
        hdr_peak_nits_ = std::clamp(nits, media::kLeastHdrPeakNits, media::kMostHdrPeakNits);
        measure_ = false;
    }
    // Instead: every PQ picture measured (media/peak_detect.h) and tone mapped from what was measured
    // (from the picture before: its counts are read when the next one is taken).
    void set_hdr_peak_measured() noexcept { measure_ = true; }
    // Whether PQ pictures are measured (asked for, and the device can run the compute pass).
    [[nodiscard]] bool hdr_peak_measured() const noexcept { return measure_ && compute_ != VK_NULL_HANDLE; }
    [[nodiscard]] std::optional<double> measured_peak() const noexcept { return detector_.peak_nits(); }
    [[nodiscard]] double hdr_peak() const noexcept { return hdr_peak_nits_; }
    // How the picture is brought to the size it is drawn at (media/scaler.h).
    void set_scaler(media::Scaler scaler) noexcept { scaler_ = scaler; }
    [[nodiscard]] media::Scaler scaler() const noexcept { return scaler_; }
    // Whether banding is smoothed out of the picture (media::Deband's settings).
    void set_deband(bool on) noexcept {
        if (on != deband_) convert_due_ = true;
        deband_ = on;
    }
    [[nodiscard]] bool deband() const noexcept { return deband_; }
    // The seed the last conversion drew its random numbers from (one per conversion).
    [[nodiscard]] std::uint32_t seed() const noexcept { return seed_; }
    // What the converted picture is held in (16-bit UNORM, or half floats where a device cannot draw into that).
    [[nodiscard]] VkFormat converted_format() const noexcept { return converted_format_; }
    // Where the picture was drawn last, and a line about it for the log.
    [[nodiscard]] const media::Placement& placement() const noexcept { return placement_; }
    [[nodiscard]] std::string describe() const;

private:
    struct Plane {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        std::uint32_t width = 0, height = 0;
        VkDeviceSize offset = 0;  // of its samples in the staging buffer
    };
    [[nodiscard]] bool create_textures(std::uint32_t width, std::uint32_t height, bool wide);
    void destroy_textures();
    [[nodiscard]] bool set_gpu_picture(const media::Picture& picture);
    [[nodiscard]] bool make_pipeline(VkRenderPass render_pass, VkShaderModule fragment, VkPipelineLayout layout, VkPipeline& pipeline);
    [[nodiscard]] bool create_converted(std::uint32_t width, std::uint32_t height);
    void destroy_converted();
    void record_convert(VkCommandBuffer commands);
    void release_gpu_picture();
    void write_descriptors(VkImageView y, VkImageView cb, VkImageView cr);
    [[nodiscard]] std::uint32_t memory_type(std::uint32_t allowed, VkMemoryPropertyFlags wanted) const;

    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    bool wide_filterable_ = false;  // 16-bit textures can be read with smoothing
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vertex_ = VK_NULL_HANDLE;
    VkShaderModule fragment_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    bool target_encodes_ = false;

    // The conversion (pass A): its own render pass into the converted picture, at the picture's size.
    VkShaderModule convert_fragment_ = VK_NULL_HANDLE;
    VkPipelineLayout convert_layout_ = VK_NULL_HANDLE;
    VkRenderPass convert_pass_ = VK_NULL_HANDLE;
    VkPipeline convert_pipeline_ = VK_NULL_HANDLE;
    VkFormat converted_format_ = VK_FORMAT_UNDEFINED;
    VkImage converted_ = VK_NULL_HANDLE;
    VkDeviceMemory converted_memory_ = VK_NULL_HANDLE;
    VkImageView converted_view_ = VK_NULL_HANDLE;
    VkFramebuffer converted_framebuffer_ = VK_NULL_HANDLE;
    std::uint32_t converted_width_ = 0, converted_height_ = 0;
    bool convert_due_ = false;   // the converted picture is not the picture taken last (or its settings)
    bool converted_ready_ = false;  // it holds a picture
    bool deband_ = false;
    std::uint32_t seed_ = 0;

    // Measuring HDR pictures: a compute pass over the converted picture into a histogram the machine reads.
    bool measure_ = false;
    VkShaderModule peak_module_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout peak_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet peak_set_ = VK_NULL_HANDLE;
    VkPipelineLayout peak_layout_ = VK_NULL_HANDLE;
    VkPipeline compute_ = VK_NULL_HANDLE;
    VkBuffer histogram_ = VK_NULL_HANDLE;
    VkDeviceMemory histogram_memory_ = VK_NULL_HANDLE;
    std::uint32_t* histogram_counts_ = nullptr;
    bool histogram_due_ = false;  // a measurement was recorded whose counts are not read yet
    media::PeakDetector detector_;
    void read_histogram();
    [[nodiscard]] bool create_measuring();
    void record_measure(VkCommandBuffer commands);

    // The drawing (pass B): the converted picture and the kernels' weights.
    VkSampler nearest_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout draw_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet draw_set_ = VK_NULL_HANDLE;
    VkBuffer weights_ = VK_NULL_HANDLE;
    VkDeviceMemory weights_memory_ = VK_NULL_HANDLE;
    media::Scaler scaler_ = media::kDefaultScaler;
    double hdr_peak_nits_ = media::kAssumedHdrPeakNits;

    Plane planes_[3];
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
    std::uint8_t* staging_bytes_ = nullptr;
    std::uint32_t width_ = 0, height_ = 0;  // of the picture shown; 0: none
    std::uint32_t texture_width_ = 0, texture_height_ = 0;  // of the textures for pictures in the machine's memory; 0: none
    bool textures_bound_ = false;           // the descriptors name the textures (not a picture on the graphics card)

    // A picture decoded on the graphics card, drawn from its own image: views of its two planes.
    bool gpu_pictures_ = false;
    PFN_vkCmdPipelineBarrier2 pipeline_barrier2_ = nullptr;
    std::optional<media::Picture> gpu_picture_;
    VkImageView gpu_views_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool gpu_wide_ = false;
    std::uint32_t gpu_image_width_ = 0, gpu_image_height_ = 0;  // of the decoder's image the picture is in
    std::optional<media::GpuImageUse> gpu_use_;  // from record_upload to after_submit
    bool gpu_refused_ = false;                   // said once
    bool wide_ = false;                     // the textures hold 16-bit samples
    bool upload_due_ = false;
    bool first_upload_ = true;              // the textures have no contents yet

    unsigned bits_ = 8;
    bool narrowed_ = false;                 // samples of more than 8 bits were cut to 8 for textures of 8
    media::ColorTransform transform_{};
    media::ColorMatrix matrix_ = media::ColorMatrix::bt709;
    media::ColorTransfer transfer_ = media::ColorTransfer::sdr;
    bool full_range_ = false;
    media::Placement placement_;
};

}  // namespace psvp::render
