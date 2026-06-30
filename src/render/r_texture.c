#include "render_internal.h"

VkImage create_image(uint32_t width, uint32_t height, uint32_t depth,
                     uint32_t array_layers, VkFormat format,
                     VkImageUsageFlags usage, VkDeviceMemory *out_memory) {
    VkImageCreateInfo ci = {0};
    ci.sType   = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.format  = format;
    ci.tiling  = VK_IMAGE_TILING_OPTIMAL;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage   = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.mipLevels = 1;
    ci.extent.width  = width;
    ci.extent.height = height;
    if (array_layers > 1) {
        ci.imageType    = VK_IMAGE_TYPE_2D;
        ci.extent.depth = 1;
        ci.arrayLayers  = array_layers;
    } else {
        ci.imageType    = (depth > 1) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ci.extent.depth = depth;
        ci.arrayLayers  = 1;
    }

    VkImage img;
    if (vkCreateImage(g_vk.device, &ci, NULL, &img) != VK_SUCCESS) return VK_NULL_HANDLE;

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(g_vk.device, img, &reqs);

    VkMemoryAllocateInfo ai = {0};
    ai.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = reqs.size;
    uint32_t mt;
    if (!vk_find_memory_type(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mt)) {
        vkDestroyImage(g_vk.device, img, NULL); return VK_NULL_HANDLE;
    }
    ai.memoryTypeIndex = mt;

    if (vkAllocateMemory(g_vk.device, &ai, NULL, out_memory) != VK_SUCCESS) {
        vkDestroyImage(g_vk.device, img, NULL);
        return VK_NULL_HANDLE;
    }
    vkBindImageMemory(g_vk.device, img, *out_memory, 0);
    return img;
}

VkImageView create_image_view(VkImage image, VkFormat format, VkImageViewType view_type) {
    VkImageViewCreateInfo ci = {0};
    ci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image                           = image;
    ci.viewType                        = view_type;
    ci.format                          = format;
    ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    ci.subresourceRange.levelCount     = 1;
    ci.subresourceRange.layerCount     = 1;
    VkImageView view;
    if (vkCreateImageView(g_vk.device, &ci, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    return view;
}

static VkImageView create_image_array_view(VkImage image, VkFormat format, uint32_t layers) {
    VkImageViewCreateInfo ci = {0};
    ci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image                           = image;
    ci.viewType                        = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    ci.format                          = format;
    ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    ci.subresourceRange.levelCount     = 1;
    ci.subresourceRange.baseArrayLayer = 0;
    ci.subresourceRange.layerCount     = layers;
    VkImageView view;
    if (vkCreateImageView(g_vk.device, &ci, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    return view;
}

VkCommandBuffer begin_one_time_cmd(void) {
    VkCommandBufferAllocateInfo ai = {0};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = g_vk.cmd_pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(g_vk.device, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void end_one_time_cmd(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {0};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(g_vk.graphics_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_vk.graphics_queue);
    vkFreeCommandBuffers(g_vk.device, g_vk.cmd_pool, 1, &cmd);
}

/* Multi-layer color barrier used for texture array transitions (layerCount > 1). */
static void array_barrier(VkCommandBuffer cmd, VkImage image,
                           VkImageLayout old_layout, VkImageLayout new_layout,
                           uint32_t layers) {
    VkAccessFlags src_access = 0, dst_access = 0;
    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        dst_access = VK_ACCESS_TRANSFER_WRITE_BIT;
        dst_stage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
        dst_access = VK_ACCESS_SHADER_READ_BIT;
        src_stage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    VkImageMemoryBarrier b = {0};
    b.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout                       = old_layout;
    b.newLayout                       = new_layout;
    b.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    b.image                           = image;
    b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount     = 1;
    b.subresourceRange.layerCount     = layers;
    b.srcAccessMask                   = src_access;
    b.dstAccessMask                   = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

bool transition_image_layout(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout) {
    VkCommandBuffer cmd = begin_one_time_cmd();
    if (cmd == VK_NULL_HANDLE) return false;

    VkAccessFlags src_access = 0, dst_access = 0;
    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        dst_access = VK_ACCESS_TRANSFER_WRITE_BIT;
        dst_stage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
        dst_access = VK_ACCESS_SHADER_READ_BIT;
        src_stage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    vk_image_barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                     old_layout, new_layout,
                     src_access, dst_access, src_stage, dst_stage);
    end_one_time_cmd(cmd);
    return true;
}

void upload_image_data(VkImage image, uint32_t width, uint32_t height, uint32_t depth,
                       const void *data, VkDeviceSize data_size) {
    if (data_size > g_vk.staging_size) {
        fprintf(stderr, "Staging buffer too small for texture upload\n");
        return;
    }

    void *mapped;
    if (vkMapMemory(g_vk.device, g_vk.staging_memory, 0, data_size, 0, &mapped) != VK_SUCCESS) return;
    memcpy(mapped, data, data_size);
    vkUnmapMemory(g_vk.device, g_vk.staging_memory);

    if (!transition_image_layout(image, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL))
        return;

    VkCommandBuffer cmd = begin_one_time_cmd();
    if (cmd == VK_NULL_HANDLE) return;

    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = (VkExtent3D){width, height, depth};
    vkCmdCopyBufferToImage(cmd, g_vk.staging_buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    end_one_time_cmd(cmd);

    transition_image_layout(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static void upload_array_layer(VkImage image, uint32_t layer,
                               uint32_t width, uint32_t height,
                               const void *data, VkDeviceSize data_size) {
    if (data_size > g_vk.staging_size) {
        fprintf(stderr, "Staging buffer too small for array layer upload\n");
        return;
    }
    void *mapped;
    vkMapMemory(g_vk.device, g_vk.staging_memory, 0, data_size, 0, &mapped);
    memcpy(mapped, data, data_size);
    vkUnmapMemory(g_vk.device, g_vk.staging_memory);

    VkCommandBuffer cmd = begin_one_time_cmd();
    if (cmd == VK_NULL_HANDLE) return;

    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask    = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.baseArrayLayer = layer;
    region.imageSubresource.layerCount    = 1;
    region.imageExtent = (VkExtent3D){width, height, 1};
    vkCmdCopyBufferToImage(cmd, g_vk.staging_buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    end_one_time_cmd(cmd);
}

/* ============================================================================
 * Public API: textures
 * ============================================================================ */

R_Texture renderer_create_texture(void) {
    CHECK_DEVICE_RET(R_INVALID_HANDLE);
    if (g_vk.texture_count >= MAX_TEXTURES) return R_INVALID_HANDLE;
    uint32_t idx = g_vk.texture_count++;
    g_vk.texture_samplers[idx] = g_vk.default_sampler;
    return idx;
}

void renderer_destroy_texture(R_Texture texture) {
    CHECK_DEVICE();
    if (texture >= g_vk.texture_count) return;
    if (g_vk.textures[texture]) {
        vkDestroyImageView(g_vk.device, g_vk.texture_views[texture], NULL);
        vkDestroyImage(g_vk.device, g_vk.textures[texture], NULL);
        vkFreeMemory(g_vk.device, g_vk.texture_memories[texture], NULL);
        g_vk.texture_views[texture]    = VK_NULL_HANDLE;
        g_vk.texture_memories[texture] = VK_NULL_HANDLE;
        g_vk.textures[texture]         = VK_NULL_HANDLE;
    }
    if (g_vk.texture_samplers[texture] != g_vk.default_sampler)
        vkDestroySampler(g_vk.device, g_vk.texture_samplers[texture], NULL);
    g_vk.texture_samplers[texture] = VK_NULL_HANDLE;
}

void renderer_bind_texture(R_TextureTarget target, R_Texture texture) {
    (void)target;
    if (texture >= g_vk.texture_count) return;
    g_bound_textures[g_active_texture_unit] = texture;

    if (g_vk.active_pipeline < g_vk.pipeline_count && texture != R_INVALID_HANDLE) {
        Pipeline *pipe = &g_vk.pipelines[g_vk.active_pipeline];
        VkDescriptorSet ds = pipe->desc_sets[g_frame_index % MAX_FRAMES_IN_FLIGHT];
        if (ds != VK_NULL_HANDLE && g_vk.texture_views[texture] != VK_NULL_HANDLE) {
            VkDescriptorImageInfo img = {0};
            img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            img.imageView   = g_vk.texture_views[texture];
            img.sampler     = g_vk.texture_samplers[texture];

            VkWriteDescriptorSet w = {0};
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = ds;
            w.dstBinding      = 0;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.descriptorCount = 1;
            w.pImageInfo      = &img;
            vkUpdateDescriptorSets(g_vk.device, 1, &w, 0, NULL);
        }
    }
}

void renderer_active_texture(int unit) { g_active_texture_unit = unit; }

R_Texture renderer_create_texture_array(int width, int height, int layers) {
    CHECK_DEVICE_RET(R_INVALID_HANDLE);
    R_Texture tex = renderer_create_texture();
    if (tex == R_INVALID_HANDLE) return tex;

    g_vk.textures[tex] = create_image((uint32_t)width, (uint32_t)height, 1, (uint32_t)layers,
                                      VK_FORMAT_R8G8B8A8_SRGB,
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                      &g_vk.texture_memories[tex]);
    if (g_vk.textures[tex] == VK_NULL_HANDLE) return R_INVALID_HANDLE;

    g_vk.texture_widths[tex]  = (uint32_t)width;
    g_vk.texture_heights[tex] = (uint32_t)height;
    g_vk.texture_depths[tex]  = (uint32_t)layers;

    g_vk.texture_views[tex] = create_image_array_view(g_vk.textures[tex],
                                                       VK_FORMAT_R8G8B8A8_SRGB,
                                                       (uint32_t)layers);
    if (g_vk.texture_views[tex] == VK_NULL_HANDLE) { renderer_destroy_texture(tex); return R_INVALID_HANDLE; }

    /* Transition all layers to TRANSFER_DST so sub-image uploads work */
    VkCommandBuffer cmd = begin_one_time_cmd();
    if (cmd != VK_NULL_HANDLE) {
        array_barrier(cmd, g_vk.textures[tex],
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      (uint32_t)layers);
        end_one_time_cmd(cmd);
    }
    return tex;
}

void renderer_tex_sub_image_array(int layer, int width, int height, const void *data) {
    CHECK_DEVICE();
    R_Texture tex = g_bound_textures[g_active_texture_unit];
    if (tex >= g_vk.texture_count || !g_vk.textures[tex] || !data) return;
    if (layer < 0 || (uint32_t)layer >= g_vk.texture_depths[tex]) return;

    VkDeviceSize sz = (VkDeviceSize)width * (VkDeviceSize)height * 4;
    upload_array_layer(g_vk.textures[tex], (uint32_t)layer, (uint32_t)width, (uint32_t)height, data, sz);

    if ((uint32_t)layer + 1 == g_vk.texture_depths[tex]) {
        VkCommandBuffer cmd = begin_one_time_cmd();
        if (cmd != VK_NULL_HANDLE) {
            array_barrier(cmd, g_vk.textures[tex],
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          g_vk.texture_depths[tex]);
            end_one_time_cmd(cmd);
        }
    }
}

static void setup_tex(R_Texture tex, uint32_t width, uint32_t height, uint32_t depth,
                      const void *data, VkImageViewType view_type) {
    bool same = (g_vk.textures[tex] &&
                 g_vk.texture_widths[tex]  == width &&
                 g_vk.texture_heights[tex] == height &&
                 g_vk.texture_depths[tex]  == depth);
    if (!same && g_vk.textures[tex]) renderer_destroy_texture(tex);
    if (!same) {
        g_vk.textures[tex] = create_image(width, height, depth, 1,
                                           VK_FORMAT_R8G8B8A8_SRGB,
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                           &g_vk.texture_memories[tex]);
        g_vk.texture_widths[tex]   = width;
        g_vk.texture_heights[tex]  = height;
        g_vk.texture_depths[tex]   = depth;
        g_vk.texture_samplers[tex] = g_vk.default_sampler;
        g_vk.texture_views[tex]    = create_image_view(g_vk.textures[tex],
                                                        VK_FORMAT_R8G8B8A8_SRGB, view_type);
    }
    if (data)
        upload_image_data(g_vk.textures[tex], width, height, depth,
                          data, (VkDeviceSize)width * height * depth * 4);
}

void renderer_tex_image_2d(int width, int height, const void *data) {
    CHECK_DEVICE();
    R_Texture tex = g_bound_textures[g_active_texture_unit];
    if (tex >= g_vk.texture_count) return;
    setup_tex(tex, (uint32_t)width, (uint32_t)height, 1, data, VK_IMAGE_VIEW_TYPE_2D);
}

void renderer_tex_image_3d(int width, int height, int depth, const void *data) {
    CHECK_DEVICE();
    R_Texture tex = g_bound_textures[g_active_texture_unit];
    if (tex >= g_vk.texture_count) return;
    setup_tex(tex, (uint32_t)width, (uint32_t)height, (uint32_t)depth, data, VK_IMAGE_VIEW_TYPE_3D);
}

void renderer_tex_sub_image_3d(int x, int y, int z, int w, int h, int d, const void *data) {
    (void)x; (void)y; (void)z; (void)w; (void)h; (void)d; (void)data;
    fprintf(stderr, "[renderer] UNIMPLEMENTED: %s\n", __func__);
}

void renderer_tex_param(R_TextureTarget target, R_TexParam param, R_TexValue value) {
    (void)target;
    R_Texture tex = g_bound_textures[g_active_texture_unit];
    if (tex >= g_vk.texture_count) return;

    VkSamplerCreateInfo ci = {0};
    ci.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter        = VK_FILTER_NEAREST;
    ci.minFilter        = VK_FILTER_NEAREST;
    ci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxAnisotropy    = 1.0f;
    ci.borderColor      = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    ci.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    if (param == R_TEX_MIN_FILTER || param == R_TEX_MAG_FILTER) {
        VkFilter f = (value == R_TEX_LINEAR) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        if (param == R_TEX_MIN_FILTER) ci.minFilter = f;
        else                           ci.magFilter = f;
    }
    if (param == R_TEX_WRAP_S || param == R_TEX_WRAP_T || param == R_TEX_WRAP_R) {
        VkSamplerAddressMode m = (value == R_TEX_REPEAT)
            ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (param == R_TEX_WRAP_S) ci.addressModeU = m;
        if (param == R_TEX_WRAP_T) ci.addressModeV = m;
        if (param == R_TEX_WRAP_R) ci.addressModeW = m;
    }

    if (g_vk.texture_samplers[tex] != g_vk.default_sampler)
        vkDestroySampler(g_vk.device, g_vk.texture_samplers[tex], NULL);
    vkCreateSampler(g_vk.device, &ci, NULL, &g_vk.texture_samplers[tex]);
}

void renderer_generate_mipmap(void) {
    fprintf(stderr, "[renderer] UNIMPLEMENTED: %s\n", __func__);
}

void renderer_bind_image_texture(int unit, R_Texture texture, R_Access access) {
    (void)unit; (void)texture; (void)access;
    fprintf(stderr, "[renderer] UNIMPLEMENTED: %s\n", __func__);
}
