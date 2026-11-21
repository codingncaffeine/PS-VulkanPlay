// Draws a decoded picture into the window: its three planes are copied into
// textures, and a shader turns their samples into the screen's colors.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "media/color.h"
#include "media/gpu_share.h"
#include "media/video_decoder.h"

namespace psvp::render {

class VideoPass {
public:
    VideoPass() = default;
    VideoPass(const VideoPass&) = delete;
    VideoPass& operator=(const VideoPass&) = delete;

    // What does not depend on the swapchain. False (with the reason logged) when a call failed.
    // `gpu_pictures`: the device can draw pictures decoded on it (Vulkan 1.3, synchronization2).
    [[nodiscard]] bool create(VkPhysicalDevice physical, VkDevice device, bool gpu_pictures = false);
    // The pipeline for a render pass; `target_encodes` when the target's format encodes what is written to it.
    [[nodiscard]] bool create_pipeline(VkRenderPass render_pass, bool target_encodes);
    void destroy_pipeline();
    void destroy();

    // Takes a picture to show from now on. Only while no command buffer that uses this pass is
    // under way: the textures are made anew when the picture's size or depth is another.
    [[nodiscard]] bool set_picture(const media::Picture& picture);
    [[nodiscard]] bool has_picture() const noexcept { return width_ != 0; }
    // Copies the picture taken last into the textures, or, for a picture decoded on the graphics
    // card, makes its image ready to be read. Outside a render pass.
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
    void set_hdr_peak(double nits) noexcept { hdr_peak_nits_ = std::clamp(nits, media::kLeastHdrPeakNits, media::kMostHdrPeakNits); }
    [[nodiscard]] double hdr_peak() const noexcept { return hdr_peak_nits_; }
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
