#include "render/video_pass.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "app/log.h"

namespace psvp::render {

namespace {

#include "psvp/video_vert.inc"
#include "psvp/video_frag.inc"

struct Constants {
    float to_rgb[16];
    float options[4];
    float scale[4];  // x, y: the share of the texture the picture fills; z: transfer (0 SDR, 1 PQ, 2 HLG); w: PQ peak in cd/m²
    float limit[4];  // the middles of the last brightness (x, y) and color (z, w) samples of the picture
};

bool good(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return true;
    log::error("render: {} failed with code {}", what, static_cast<int>(result));
    return false;
}

}  // namespace

std::uint32_t VideoPass::memory_type(std::uint32_t allowed, VkMemoryPropertyFlags wanted) const {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((allowed & (1U << i)) != 0 && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    }
    return UINT32_MAX;
}

bool VideoPass::create(VkPhysicalDevice physical, VkDevice device, bool gpu_pictures) {
    physical_ = physical;
    device_ = device;
    if (gpu_pictures) {
        pipeline_barrier2_ = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(vkGetDeviceProcAddr(device_, "vkCmdPipelineBarrier2"));
        gpu_pictures_ = pipeline_barrier2_ != nullptr;
    }

    VkFormatProperties wide{};
    vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_R16_UNORM, &wide);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    wide_filterable_ = (wide.optimalTilingFeatures & needed) == needed;

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!good(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "vkCreateSampler")) return false;

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout{};
    set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout.bindingCount = 3;
    set_layout.pBindings = bindings;
    if (!good(vkCreateDescriptorSetLayout(device_, &set_layout, nullptr, &set_layout_), "vkCreateDescriptorSetLayout")) return false;

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (!good(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_), "vkCreateDescriptorPool")) return false;
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout_;
    if (!good(vkAllocateDescriptorSets(device_, &allocate, &set_), "vkAllocateDescriptorSets")) return false;

    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Constants)};
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
    return module(kVideoVertexShader, sizeof kVideoVertexShader, vertex_) && module(kVideoFragmentShader, sizeof kVideoFragmentShader, fragment_);
}

bool VideoPass::create_pipeline(VkRenderPass render_pass, bool target_encodes) {
    destroy_pipeline();
    target_encodes_ = target_encodes;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
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
    VkPipelineColorBlendAttachmentState attachment{};
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

void VideoPass::destroy_pipeline() {
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
}

void VideoPass::destroy_textures() {
    for (Plane& plane : planes_) {
        if (plane.view != VK_NULL_HANDLE) vkDestroyImageView(device_, plane.view, nullptr);
        if (plane.image != VK_NULL_HANDLE) vkDestroyImage(device_, plane.image, nullptr);
        if (plane.memory != VK_NULL_HANDLE) vkFreeMemory(device_, plane.memory, nullptr);
        plane = Plane{};
    }
    if (staging_memory_ != VK_NULL_HANDLE && staging_bytes_ != nullptr) vkUnmapMemory(device_, staging_memory_);
    if (staging_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, staging_, nullptr);
    if (staging_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, staging_memory_, nullptr);
    staging_ = VK_NULL_HANDLE;
    staging_memory_ = VK_NULL_HANDLE;
    staging_bytes_ = nullptr;
    texture_width_ = texture_height_ = 0;
    textures_bound_ = false;
}

void VideoPass::release_gpu_picture() {
    for (VkImageView& view : gpu_views_) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device_, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    gpu_picture_.reset();
}

void VideoPass::write_descriptors(VkImageView y, VkImageView cb, VkImageView cr) {
    const VkImageView views[3] = {y, cb, cr};
    VkDescriptorImageInfo images[3]{};
    VkWriteDescriptorSet writes[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        images[i].sampler = sampler_;
        images[i].imageView = views[i];
        images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
}

void VideoPass::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    destroy_pipeline();
    destroy_textures();
    release_gpu_picture();
    if (fragment_ != VK_NULL_HANDLE) vkDestroyShaderModule(device_, fragment_, nullptr);
    if (vertex_ != VK_NULL_HANDLE) vkDestroyShaderModule(device_, vertex_, nullptr);
    if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
    fragment_ = vertex_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

bool VideoPass::create_textures(std::uint32_t width, std::uint32_t height, bool wide) {
    destroy_textures();
    const VkFormat format = wide ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
    const VkDeviceSize sample_bytes = wide ? 2 : 1;
    VkDeviceSize staging_size = 0;
    for (int i = 0; i < 3; ++i) {
        Plane& plane = planes_[i];
        // The color planes are half as wide and half as high, rounded up.
        plane.width = i == 0 ? width : (width + 1) / 2;
        plane.height = i == 0 ? height : (height + 1) / 2;
        plane.offset = staging_size;
        staging_size += VkDeviceSize{plane.width} * plane.height * sample_bytes;
        // Offsets of a copy are multiples of four at least, whatever a device asks for.
        staging_size = (staging_size + 3) & ~VkDeviceSize{3};

        VkImageCreateInfo image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = format;
        image.extent = {plane.width, plane.height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!good(vkCreateImage(device_, &image, nullptr, &plane.image), "vkCreateImage")) return false;
        VkMemoryRequirements needs{};
        vkGetImageMemoryRequirements(device_, plane.image, &needs);
        VkMemoryAllocateInfo memory{};
        memory.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memory.allocationSize = needs.size;
        memory.memoryTypeIndex = memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memory.memoryTypeIndex == UINT32_MAX) memory.memoryTypeIndex = memory_type(needs.memoryTypeBits, 0);
        if (memory.memoryTypeIndex == UINT32_MAX) {
            log::error("render: no memory for a picture's texture");
            return false;
        }
        if (!good(vkAllocateMemory(device_, &memory, nullptr, &plane.memory), "vkAllocateMemory") ||
            !good(vkBindImageMemory(device_, plane.image, plane.memory, 0), "vkBindImageMemory")) {
            return false;
        }
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = plane.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!good(vkCreateImageView(device_, &view, nullptr, &plane.view), "vkCreateImageView")) return false;
    }

    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = staging_size;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!good(vkCreateBuffer(device_, &buffer, nullptr, &staging_), "vkCreateBuffer")) return false;
    VkMemoryRequirements needs{};
    vkGetBufferMemoryRequirements(device_, staging_, &needs);
    VkMemoryAllocateInfo memory{};
    memory.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memory.allocationSize = needs.size;
    memory.memoryTypeIndex = memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory.memoryTypeIndex == UINT32_MAX) {
        log::error("render: no memory this program can write a picture into");
        return false;
    }
    void* mapped = nullptr;
    if (!good(vkAllocateMemory(device_, &memory, nullptr, &staging_memory_), "vkAllocateMemory") ||
        !good(vkBindBufferMemory(device_, staging_, staging_memory_, 0), "vkBindBufferMemory") ||
        !good(vkMapMemory(device_, staging_memory_, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory")) {
        return false;
    }
    staging_bytes_ = static_cast<std::uint8_t*>(mapped);

    write_descriptors(planes_[0].view, planes_[1].view, planes_[2].view);
    textures_bound_ = true;
    texture_width_ = width;
    texture_height_ = height;
    wide_ = wide;
    first_upload_ = true;
    return true;
}

bool VideoPass::set_gpu_picture(const media::Picture& picture) {
    // The picture shown before is done with: the frame drawn from it has finished.
    release_gpu_picture();
    if (!gpu_pictures_) return false;
    auto use = media::begin_use(picture);
    if (!use) {
        if (!std::exchange(gpu_refused_, true)) log::error("render: {}", use.error());
        return false;
    }
    const VkImage image = use->image;
    gpu_wide_ = use->wide;
    gpu_image_width_ = std::max(use->image_width, picture.width);
    gpu_image_height_ = std::max(use->image_height, picture.height);
    media::finish_use(picture, false, VK_IMAGE_LAYOUT_UNDEFINED, 0);
    // Views of the image's two planes: the brightness, and the two color differences side by side.
    const VkFormat formats[2] = {gpu_wide_ ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM, gpu_wide_ ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM};
    const VkImageAspectFlags aspects[2] = {VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_ASPECT_PLANE_1_BIT};
    for (int i = 0; i < 2; ++i) {
        VkImageViewUsageCreateInfo usage{};
        usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
        usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.pNext = &usage;
        view.image = image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = formats[i];
        view.subresourceRange = {aspects[i], 0, 1, 0, 1};
        if (!good(vkCreateImageView(device_, &view, nullptr, &gpu_views_[i]), "vkCreateImageView")) {
            release_gpu_picture();
            return false;
        }
    }
    write_descriptors(gpu_views_[0], gpu_views_[1], gpu_views_[1]);
    textures_bound_ = false;
    gpu_picture_ = picture;
    return true;
}

bool VideoPass::set_picture(const media::Picture& picture) {
    if (picture.width == 0 || picture.height == 0 || (picture.bits != 8 && (picture.bits < 9 || picture.bits > 16))) return false;
    if (picture.on_gpu) {
        if (!set_gpu_picture(picture)) return false;
        width_ = picture.width;
        height_ = picture.height;
        bits_ = picture.bits;
        narrowed_ = false;
        matrix_ = picture.matrix;
        transfer_ = picture.transfer;
        full_range_ = picture.full_range;
        transform_ = media::color_transform(picture.matrix, picture.full_range, picture.bits);
        return true;
    }
    if (picture.planes[0] == nullptr) return false;
    release_gpu_picture();
    const bool wide = picture.bits > 8 && wide_filterable_;
    if (picture.width != texture_width_ || picture.height != texture_height_ || wide != wide_) {
        if (!create_textures(picture.width, picture.height, wide)) {
            destroy_textures();
            width_ = height_ = 0;
            return false;
        }
    }
    if (!textures_bound_) {
        write_descriptors(planes_[0].view, planes_[1].view, planes_[2].view);
        textures_bound_ = true;
    }
    width_ = picture.width;
    height_ = picture.height;
    bits_ = picture.bits;
    narrowed_ = picture.bits > 8 && !wide;
    matrix_ = picture.matrix;
    transfer_ = picture.transfer;
    full_range_ = picture.full_range;
    // Samples cut to 8 bits are 8-bit samples to the conversion.
    transform_ = media::color_transform(picture.matrix, picture.full_range, narrowed_ ? 8 : picture.bits);

    for (int i = 0; i < 3; ++i) {
        const Plane& plane = planes_[i];
        std::uint8_t* to = staging_bytes_ + plane.offset;
        const std::uint8_t* from = picture.planes[i];
        if (picture.bits == 8) {
            for (std::uint32_t row = 0; row < plane.height; ++row) std::memcpy(to + std::size_t{row} * plane.width, from + std::size_t{row} * picture.strides[i], plane.width);
        } else if (wide) {
            for (std::uint32_t row = 0; row < plane.height; ++row) {
                std::memcpy(to + std::size_t{row} * plane.width * 2, from + std::size_t{row} * picture.strides[i], std::size_t{plane.width} * 2);
            }
        } else {
            const unsigned shift = picture.bits - 8;
            for (std::uint32_t row = 0; row < plane.height; ++row) {
                for (std::uint32_t x = 0; x < plane.width; ++x) {
                    std::uint16_t sample = 0;
                    std::memcpy(&sample, from + std::size_t{row} * picture.strides[i] + std::size_t{x} * 2, 2);
                    to[std::size_t{row} * plane.width + x] = static_cast<std::uint8_t>(sample >> shift);
                }
            }
        }
    }
    upload_due_ = true;
    return true;
}

void VideoPass::record_upload(VkCommandBuffer commands) {
    if (gpu_picture_) {
        // Every frame that reads the image waits for whoever wrote it last, from the layout it left
        // it in: the decoder may have used it since, as a picture later ones are built on.
        auto use = media::begin_use(*gpu_picture_);
        if (!use) return;
        gpu_use_ = *use;
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = use->access;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = use->layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = use->image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        pipeline_barrier2_(commands, &dependency);
        return;
    }
    if (!upload_due_ || texture_width_ == 0) return;
    upload_due_ = false;
    VkImageMemoryBarrier barriers[3]{};
    for (int i = 0; i < 3; ++i) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].srcAccessMask = first_upload_ ? VkAccessFlags{0} : VkAccessFlags{VK_ACCESS_SHADER_READ_BIT};
        barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        // What the textures held is not needed: the whole of each is written.
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = planes_[i].image;
        barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(commands, first_upload_ ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 3, barriers);
    for (const Plane& plane : planes_) {
        VkBufferImageCopy copy{};
        copy.bufferOffset = plane.offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {plane.width, plane.height, 1};
        vkCmdCopyBufferToImage(commands, staging_, plane.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    for (VkImageMemoryBarrier& barrier : barriers) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3, barriers);
    first_upload_ = false;
}

std::optional<VideoPass::GpuWait> VideoPass::gpu_wait() const {
    if (!gpu_use_) return std::nullopt;
    return GpuWait{gpu_use_->semaphore, gpu_use_->wait_value, gpu_use_->wait_value + 1};
}

void VideoPass::after_submit(bool submitted) {
    if (!gpu_use_ || !gpu_picture_) return;
    media::finish_use(*gpu_picture_, submitted, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    gpu_use_.reset();
}

void VideoPass::record_draw(VkCommandBuffer commands, VkExtent2D target) {
    if (width_ == 0 || pipeline_ == VK_NULL_HANDLE) return;
    if (gpu_picture_ ? !gpu_use_ : first_upload_) return;
    placement_ = media::place_picture(width_, height_, target.width, target.height);
    VkViewport viewport{};
    viewport.x = static_cast<float>(placement_.x);
    viewport.y = static_cast<float>(placement_.y);
    viewport.width = static_cast<float>(placement_.width);
    viewport.height = static_cast<float>(placement_.height);
    viewport.maxDepth = 1.0F;
    VkRect2D scissor{{placement_.x, placement_.y}, {placement_.width, placement_.height}};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);

    Constants constants{};
    std::copy(transform_.begin(), transform_.end(), constants.to_rgb);
    // A texture of 16 bits gives a sample of fewer bits as a share of 65535: brought to a share of its own largest value.
    // A graphics card's decoder puts samples of 10 bits in the high bits of 16: a share of 65535 that
    // is 64 times the sample.
    const unsigned largest = (1U << bits_) - 1U;
    if (gpu_picture_) constants.options[0] = gpu_wide_ ? 65535.0F / static_cast<float>(largest << (16U - bits_)) : 1.0F;
    else constants.options[0] = wide_ ? 65535.0F / static_cast<float>(largest) : 1.0F;
    // A color sample sits on the left one of the two brightness samples it belongs to, not between them (as the
    // streams of a console say): a quarter of its own width left of where a plane of half the size puts it.
    // The texture may be larger than the picture (a decoder's image is): only the picture's share is drawn.
    const std::uint32_t texture_width = gpu_picture_ ? gpu_image_width_ : width_;
    const std::uint32_t texture_height = gpu_picture_ ? gpu_image_height_ : height_;
    constants.scale[0] = static_cast<float>(width_) / static_cast<float>(texture_width);
    constants.scale[1] = static_cast<float>(height_) / static_cast<float>(texture_height);
    const auto half = [](std::uint32_t size) { return static_cast<float>((size + 1) / 2); };
    constants.limit[0] = (static_cast<float>(width_) - 0.5F) / static_cast<float>(texture_width);
    constants.limit[1] = (static_cast<float>(height_) - 0.5F) / static_cast<float>(texture_height);
    constants.limit[2] = (half(width_) - 0.5F) / half(texture_width);
    constants.limit[3] = (half(height_) - 0.5F) / half(texture_height);
    constants.options[1] = 0.25F / static_cast<float>((texture_width + 1) / 2);
    constants.options[2] = target_encodes_ ? 1.0F : 0.0F;
    constants.options[3] = gpu_picture_ ? 1.0F : 0.0F;
    constants.scale[2] = transfer_ == media::ColorTransfer::pq ? 1.0F : transfer_ == media::ColorTransfer::hlg ? 2.0F : 0.0F;
    constants.scale[3] = static_cast<float>(hdr_peak_nits_);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    vkCmdPushConstants(commands, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants, &constants);
    vkCmdDraw(commands, 3, 1, 0, 0);
}

std::string VideoPass::describe() const {
    return std::format("{}x{}, {} bits{}, {} range, {}{}, drawn at {}x{}{}", width_, height_, bits_, narrowed_ ? " (shown as 8)" : "", full_range_ ? "full" : "limited",
                       matrix_ == media::ColorMatrix::bt2020 ? "BT.2020" : matrix_ == media::ColorMatrix::bt601 ? "BT.601" : "BT.709",
                       transfer_ == media::ColorTransfer::pq    ? std::format(", HDR (PQ) tone mapped from {:.0f} cd/m² to SDR", hdr_peak_nits_)
                       : transfer_ == media::ColorTransfer::hlg ? std::string(", HDR (HLG) tone mapped to SDR")
                                                                 : std::string(),
                       placement_.width, placement_.height,
                       gpu_picture_ ? ", read straight from the graphics card's decoder" : "");
}

}  // namespace psvp::render
