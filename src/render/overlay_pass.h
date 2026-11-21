// Draws what lies over the picture: panels and text, as src/overlay lays them out
// in window pixels. The glyphs come from one atlas texture, uploaded once.
#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "overlay/text.h"

namespace psvp::render {

class OverlayPass {
public:
    static constexpr std::size_t kMaxQuads = 4096;

    OverlayPass() = default;
    OverlayPass(const OverlayPass&) = delete;
    OverlayPass& operator=(const OverlayPass&) = delete;

    // What does not depend on the swapchain. False (with the reason logged) when a call failed.
    [[nodiscard]] bool create(VkPhysicalDevice physical, VkDevice device);
    // `target_encodes`: the target's format encodes what is written to it (an sRGB format).
    [[nodiscard]] bool create_pipeline(VkRenderPass render_pass, bool target_encodes);
    void destroy_pipeline();
    void destroy();

    // The quads to draw from now on (more than kMaxQuads are left out). Only while no command
    // buffer that uses this pass is under way.
    void set_quads(const std::vector<overlay::Quad>& quads);
    // The atlas, the first time. Outside a render pass.
    void record_upload(VkCommandBuffer commands);
    // Inside the render pass, after the picture.
    void record_draw(VkCommandBuffer commands, VkExtent2D target);

private:
    [[nodiscard]] std::uint32_t memory_type(std::uint32_t allowed, VkMemoryPropertyFlags wanted) const;
    [[nodiscard]] bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, void** mapped);

    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vertex_ = VK_NULL_HANDLE;
    VkShaderModule fragment_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    bool target_encodes_ = false;

    VkImage atlas_ = VK_NULL_HANDLE;
    VkDeviceMemory atlas_memory_ = VK_NULL_HANDLE;
    VkImageView atlas_view_ = VK_NULL_HANDLE;
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
    bool atlas_uploaded_ = false;

    VkBuffer vertices_ = VK_NULL_HANDLE;
    VkDeviceMemory vertices_memory_ = VK_NULL_HANDLE;
    float* vertex_data_ = nullptr;
    std::uint32_t vertex_count_ = 0;
    std::vector<overlay::Quad> quads_;  // kept to be written again when the target's encoding changes
};

}  // namespace psvp::render
