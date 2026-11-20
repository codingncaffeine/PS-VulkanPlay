// Draws a decoded picture into the window: its three planes are copied into
// textures, and a shader turns their samples into the screen's colors.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "media/color.h"
#include "media/video_decoder.h"

namespace psvp::render {

class VideoPass {
public:
    VideoPass() = default;
    VideoPass(const VideoPass&) = delete;
    VideoPass& operator=(const VideoPass&) = delete;

    // What does not depend on the swapchain. False (with the reason logged) when a call failed.
    [[nodiscard]] bool create(VkPhysicalDevice physical, VkDevice device);
    // The pipeline for a render pass; `target_encodes` when the target's format encodes what is written to it.
    [[nodiscard]] bool create_pipeline(VkRenderPass render_pass, bool target_encodes);
    void destroy_pipeline();
    void destroy();

    // Takes a picture to show from now on. Only while no command buffer that uses this pass is
    // under way: the textures are made anew when the picture's size or depth is another.
    [[nodiscard]] bool set_picture(const media::Picture& picture);
    [[nodiscard]] bool has_picture() const noexcept { return width_ != 0; }
    // Copies the picture taken last into the textures. Outside a render pass; does nothing when there is no new one.
    void record_upload(VkCommandBuffer commands);
    // Draws the picture as large as it fits into the target. Inside the render pass.
    void record_draw(VkCommandBuffer commands, VkExtent2D target);
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

    Plane planes_[3];
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
    std::uint8_t* staging_bytes_ = nullptr;
    std::uint32_t width_ = 0, height_ = 0;  // of the picture the textures are for; 0: none
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
