#include "render/overlay_pass.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "app/log.h"
#include "media/tone_map.h"

namespace psvp::render {

namespace {

#include "psvp/overlay_vert.inc"
#include "psvp/overlay_frag.inc"

// x, y, u, v, r, g, b, a, glyph
constexpr std::size_t kFloatsPerVertex = 9;
constexpr std::size_t kVerticesPerQuad = 6;

bool good(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return true;
    log::error("render: overlay: {} failed with code {}", what, static_cast<int>(result));
    return false;
}

float srgb_to_linear(float encoded) {
    return encoded <= 0.04045F ? encoded / 12.92F : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

}  // namespace

std::uint32_t OverlayPass::memory_type(std::uint32_t allowed, VkMemoryPropertyFlags wanted) const {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((allowed & (1U << i)) != 0 && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    }
    return UINT32_MAX;
}

bool OverlayPass::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, void** mapped) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!good(vkCreateBuffer(device_, &info, nullptr, &buffer), "vkCreateBuffer")) return false;
    VkMemoryRequirements needs{};
    vkGetBufferMemoryRequirements(device_, buffer, &needs);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = needs.size;
    allocate.memoryTypeIndex = memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (allocate.memoryTypeIndex == UINT32_MAX) {
        log::error("render: overlay: no memory the processor can write for a buffer");
        return false;
    }
    if (!good(vkAllocateMemory(device_, &allocate, nullptr, &memory), "vkAllocateMemory")) return false;
    if (!good(vkBindBufferMemory(device_, buffer, memory, 0), "vkBindBufferMemory")) return false;
    return good(vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, mapped), "vkMapMemory");
}

bool OverlayPass::create(VkPhysicalDevice physical, VkDevice device) {
    physical_ = physical;
    device_ = device;
    const auto width = static_cast<std::uint32_t>(overlay::kAtlasWidth);
    const auto height = static_cast<std::uint32_t>(overlay::kAtlasHeight);

    // The atlas: one channel of 8 bits, read with smoothing (the distance field needs it).
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8_UNORM;
    image.extent = {width, height, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!good(vkCreateImage(device_, &image, nullptr, &atlas_), "vkCreateImage")) return false;
    VkMemoryRequirements needs{};
    vkGetImageMemoryRequirements(device_, atlas_, &needs);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = needs.size;
    allocate.memoryTypeIndex = memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocate.memoryTypeIndex == UINT32_MAX) allocate.memoryTypeIndex = memory_type(needs.memoryTypeBits, 0);
    if (!good(vkAllocateMemory(device_, &allocate, nullptr, &atlas_memory_), "vkAllocateMemory")) return false;
    if (!good(vkBindImageMemory(device_, atlas_, atlas_memory_, 0), "vkBindImageMemory")) return false;
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = atlas_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8_UNORM;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!good(vkCreateImageView(device_, &view, nullptr, &atlas_view_), "vkCreateImageView")) return false;

    void* staging_bytes = nullptr;
    const VkDeviceSize atlas_size = VkDeviceSize{width} * height;
    if (!create_buffer(atlas_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging_, staging_memory_, &staging_bytes)) return false;
    std::memcpy(staging_bytes, overlay::atlas_pixels(), static_cast<std::size_t>(atlas_size));
    void* vertex_bytes = nullptr;
    if (!create_buffer(VkDeviceSize{kMaxQuads * kVerticesPerQuad * kFloatsPerVertex * sizeof(float)}, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices_,
                       vertices_memory_, &vertex_bytes)) {
        return false;
    }
    vertex_data_ = static_cast<float*>(vertex_bytes);

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!good(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "vkCreateSampler")) return false;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout{};
    set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout.bindingCount = 1;
    set_layout.pBindings = &binding;
    if (!good(vkCreateDescriptorSetLayout(device_, &set_layout, nullptr, &set_layout_), "vkCreateDescriptorSetLayout")) return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (!good(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_), "vkCreateDescriptorPool")) return false;
    VkDescriptorSetAllocateInfo set{};
    set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set.descriptorPool = pool_;
    set.descriptorSetCount = 1;
    set.pSetLayouts = &set_layout_;
    if (!good(vkAllocateDescriptorSets(device_, &set, &set_), "vkAllocateDescriptorSets")) return false;
    VkDescriptorImageInfo atlas{sampler_, atlas_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &atlas;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float)};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &set_layout_;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    if (!good(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_), "vkCreatePipelineLayout")) return false;

    const auto module = [&](const std::uint32_t* code, std::size_t bytes, VkShaderModule& out) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = bytes;
        info.pCode = code;
        return good(vkCreateShaderModule(device_, &info, nullptr, &out), "vkCreateShaderModule");
    };
    return module(kOverlayVertexShader, sizeof kOverlayVertexShader, vertex_) &&
           module(kOverlayFragmentShader, sizeof kOverlayFragmentShader, fragment_);
}

bool OverlayPass::create_pipeline(VkRenderPass render_pass, TargetKind target) {
    destroy_pipeline();
    if (target_ != target) {
        target_ = target;
        set_quads(std::vector<overlay::Quad>(quads_));
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, kFloatsPerVertex * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[4]{
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 4 * sizeof(float)},
        {3, 0, VK_FORMAT_R32_SFLOAT, 8 * sizeof(float)},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 4;
    vertex_input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Over the picture: the color by its opacity, what is behind by the rest.
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_;
    info.renderPass = render_pass;
    return good(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_), "vkCreateGraphicsPipelines");
}

void OverlayPass::set_quads(const std::vector<overlay::Quad>& quads) {
    quads_ = quads;
    vertex_count_ = 0;
    if (vertex_data_ == nullptr) return;
    const std::size_t count = std::min(quads.size(), kMaxQuads);
    float* out = vertex_data_;
    for (std::size_t i = 0; i < count; ++i) {
        const overlay::Quad& q = quads[i];
        float rgb[3] = {static_cast<float>(q.color.r) / 255.0F, static_cast<float>(q.color.g) / 255.0F, static_cast<float>(q.color.b) / 255.0F};
        if (target_ == TargetKind::sdr_srgb) {
            for (float& channel : rgb) channel = srgb_to_linear(channel);
        } else if (target_ == TargetKind::hdr10) {
            const auto signal = media::hdr10_signal({static_cast<double>(rgb[0]), static_cast<double>(rgb[1]), static_cast<double>(rgb[2])}, media::ColorTransfer::sdr);
            for (std::size_t c = 0; c < 3; ++c) rgb[c] = static_cast<float>(signal[c]);
        }
        const float corners[6][4] = {
            {q.x0, q.y0, q.u0, q.v0}, {q.x1, q.y0, q.u1, q.v0}, {q.x1, q.y1, q.u1, q.v1},
            {q.x0, q.y0, q.u0, q.v0}, {q.x1, q.y1, q.u1, q.v1}, {q.x0, q.y1, q.u0, q.v1},
        };
        for (const auto& corner : corners) {
            *out++ = corner[0];
            *out++ = corner[1];
            *out++ = corner[2];
            *out++ = corner[3];
            *out++ = rgb[0];
            *out++ = rgb[1];
            *out++ = rgb[2];
            *out++ = q.color.alpha;
            *out++ = q.glyph ? 1.0F : 0.0F;
        }
    }
    vertex_count_ = static_cast<std::uint32_t>(count * kVerticesPerQuad);
}

void OverlayPass::record_upload(VkCommandBuffer commands) {
    if (atlas_uploaded_ || atlas_ == VK_NULL_HANDLE) return;
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = atlas_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<std::uint32_t>(overlay::kAtlasWidth), static_cast<std::uint32_t>(overlay::kAtlasHeight), 1};
    vkCmdCopyBufferToImage(commands, staging_, atlas_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    atlas_uploaded_ = true;
}

void OverlayPass::record_draw(VkCommandBuffer commands, VkExtent2D target) {
    if (vertex_count_ == 0 || pipeline_ == VK_NULL_HANDLE || !atlas_uploaded_ || target.width == 0 || target.height == 0) return;
    VkViewport viewport{};
    viewport.width = static_cast<float>(target.width);
    viewport.height = static_cast<float>(target.height);
    viewport.maxDepth = 1.0F;
    VkRect2D scissor{{0, 0}, target};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
    const float size[2] = {static_cast<float>(target.width), static_cast<float>(target.height)};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    vkCmdPushConstants(commands, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof size, size);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, &vertices_, &offset);
    vkCmdDraw(commands, vertex_count_, 1, 0, 0);
}

void OverlayPass::destroy_pipeline() {
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
}

void OverlayPass::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    destroy_pipeline();
    if (vertices_memory_ != VK_NULL_HANDLE && vertex_data_ != nullptr) vkUnmapMemory(device_, vertices_memory_);
    if (vertices_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, vertices_, nullptr);
    if (vertices_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, vertices_memory_, nullptr);
    if (staging_memory_ != VK_NULL_HANDLE) vkUnmapMemory(device_, staging_memory_);
    if (staging_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, staging_, nullptr);
    if (staging_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, staging_memory_, nullptr);
    if (atlas_view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, atlas_view_, nullptr);
    if (atlas_ != VK_NULL_HANDLE) vkDestroyImage(device_, atlas_, nullptr);
    if (atlas_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, atlas_memory_, nullptr);
    if (fragment_ != VK_NULL_HANDLE) vkDestroyShaderModule(device_, fragment_, nullptr);
    if (vertex_ != VK_NULL_HANDLE) vkDestroyShaderModule(device_, vertex_, nullptr);
    if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    vertex_ = fragment_ = VK_NULL_HANDLE;
    atlas_ = VK_NULL_HANDLE;
    atlas_memory_ = VK_NULL_HANDLE;
    atlas_view_ = VK_NULL_HANDLE;
    staging_ = VK_NULL_HANDLE;
    staging_memory_ = VK_NULL_HANDLE;
    atlas_uploaded_ = false;
    vertices_ = VK_NULL_HANDLE;
    vertices_memory_ = VK_NULL_HANDLE;
    vertex_data_ = nullptr;
    vertex_count_ = 0;
    quads_.clear();
    device_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
}

}  // namespace psvp::render
