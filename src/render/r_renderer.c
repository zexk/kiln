#include "render_internal.h"

#ifdef ENABLE_VALIDATION
VkDebugUtilsMessengerEXT g_debug_messenger = VK_NULL_HANDLE;
#endif

VulkanContext  g_vk             = {0};
VkPipelineCache g_pipeline_cache = VK_NULL_HANDLE;

UniformMapping g_uniforms[32];
int            g_uniform_count  = 0;
uint8_t        g_push_constants[256];
bool           g_push_dirty     = false;

int       g_active_texture_unit    = 0;
R_Texture g_bound_textures[16]     = {R_INVALID_HANDLE};

VAOState g_vao_state  = {0};
R_VAO    g_current_vao = R_INVALID_HANDLE;

static char g_screenshot_path[512];

void renderer_save_screenshot(const char *path) {
    if (!path) { g_screenshot_path[0] = '\0'; return; }
    strncpy(g_screenshot_path, path, sizeof(g_screenshot_path) - 1);
    g_screenshot_path[sizeof(g_screenshot_path) - 1] = '\0';
}

/* Record a copy of the just-rendered swapchain image into a host-visible buffer.
   Returns the buffer/memory (caller maps + frees after the fence signals), or
   false if the image can't be captured. Leaves the image in COLOR_ATTACHMENT
   layout so the existing present path is unaffected. */
static bool screenshot_record_copy(VkCommandBuffer cmd, VkBuffer *out_buf,
                                    VkDeviceMemory *out_mem) {
    VkImage img = g_vk.swap_images[g_vk.image_index];
    uint32_t w = g_vk.swap_extent.width, h = g_vk.swap_extent.height;
    VkDeviceSize sz = (VkDeviceSize)w * h * 4;

    VkBuffer buf = create_buffer(sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 out_mem);
    if (buf == VK_NULL_HANDLE) return false;

    VkImageMemoryBarrier b = {0};
    b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.image                       = img;
    b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.oldLayout                   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask               = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask               = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    VkBufferImageCopy bic = {0};
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent = (VkExtent3D){w, h, 1};
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);

    b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    *out_buf = buf;
    return true;
}

/* Map the captured buffer and write it out as a binary PPM. swap_format is
   B8G8R8A8_SRGB, so swap B/R; the sRGB-encoded bytes go straight to the file. */
static void screenshot_write_ppm(VkDeviceMemory mem) {
    uint32_t w = g_vk.swap_extent.width, h = g_vk.swap_extent.height;
    void *mapped;
    if (vkMapMemory(g_vk.device, mem, 0, (VkDeviceSize)w * h * 4, 0, &mapped) != VK_SUCCESS)
        return;
    FILE *f = fopen(g_screenshot_path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        const uint8_t *px = mapped;
        for (uint32_t i = 0; i < w * h; i++) {
            fputc(px[i * 4 + 2], f); /* R (from BGRA) */
            fputc(px[i * 4 + 1], f); /* G */
            fputc(px[i * 4 + 0], f); /* B */
        }
        fclose(f);
        fprintf(stderr, "[render] screenshot -> %s\n", g_screenshot_path);
    }
    vkUnmapMemory(g_vk.device, mem);
}

/* ============================================================================
 * Helpers (definitions referenced by all translation units)
 * ============================================================================ */

uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(g_vk.physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    fprintf(stderr, "Failed to find suitable memory type\n");
    return UINT32_MAX;
}

VkShaderModule create_shader_module(const char *code, size_t size) {
    VkShaderModuleCreateInfo ci = {0};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size;
    ci.pCode    = (const uint32_t *)code;
    VkShaderModule mod;
    if (vkCreateShaderModule(g_vk.device, &ci, NULL, &mod) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create shader module\n");
        return VK_NULL_HANDLE;
    }
    return mod;
}

/* ============================================================================
 * Init / shutdown
 * ============================================================================ */

bool renderer_init(int width, int height, const platform_native_handles_t *native) {
    g_vk.width          = width;
    g_vk.height         = height;
    g_vk.native_handles = *native;

    if (!create_instance())          { fprintf(stderr, "Failed to create instance\n");         return false; }
    if (!select_physical_device())   { fprintf(stderr, "Failed to select physical device\n");
        vkDestroyInstance(g_vk.instance, NULL); return false; }
    if (!create_surface())           { fprintf(stderr, "Failed to create surface\n");
        vkDestroyInstance(g_vk.instance, NULL); return false; }
    if (!find_queue_families())      { fprintf(stderr, "Failed to find queue families\n");
        vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
        vkDestroyInstance(g_vk.instance, NULL); return false; }
    if (!create_device())            { fprintf(stderr, "Failed to create device\n");
        vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
        vkDestroyInstance(g_vk.instance, NULL); return false; }

    g_vk.present_mode = VK_PRESENT_MODE_FIFO_KHR;

    if (!create_swapchain())         { fprintf(stderr, "Failed to create swapchain\n");        goto fail_dev; }
    if (!create_swap_image_views())  { fprintf(stderr, "Failed to create swap image views\n"); goto fail_swap; }

    if (g_vk.swap_extent.width == 0 || g_vk.swap_extent.height == 0) {
        fprintf(stderr, "Invalid swap extent: %ux%u\n",
                g_vk.swap_extent.width, g_vk.swap_extent.height);
        goto fail_swap;
    }

    if (!create_depth_buffer())      { fprintf(stderr, "Failed to create depth buffer\n");  goto fail_swap; }
    if (!create_command_pool())      { fprintf(stderr, "Failed to create command pool\n");  goto fail_depth; }
    if (!create_command_buffers())   { fprintf(stderr, "Failed to create command buffers\n"); goto fail_cmd; }
    if (!create_sync_objects())      { fprintf(stderr, "Failed to create sync objects\n");  goto fail_cmd; }
    if (!create_descriptor_pool())   { fprintf(stderr, "Failed to create descriptor pool\n"); goto fail_sync; }
    if (!create_default_sampler())   { fprintf(stderr, "Failed to create default sampler\n"); goto fail_pool; }

    init_resource_arrays();

    g_vk.staging_size   = 16 * 1024 * 1024;
    g_vk.staging_buffer = create_buffer(g_vk.staging_size,
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                        &g_vk.staging_memory);
    if (g_vk.staging_buffer == VK_NULL_HANDLE) {
        fprintf(stderr, "Failed to create staging buffer\n");
        renderer_shutdown();
        return false;
    }

    g_vk.cull_mode          = VK_CULL_MODE_BACK_BIT;
    g_vk.topology           = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    g_vk.line_width         = 1.0f;
    g_vk.poly_offset_factor = 0.0f;
    g_vk.poly_offset_units  = 0.0f;

    printf("Vulkan renderer initialized\n");
    return true;

fail_pool:
    vkDestroyDescriptorPool(g_vk.device, g_vk.desc_pool, NULL);
fail_sync:
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(g_vk.device, g_vk.image_avail_sems[i], NULL);
        vkDestroySemaphore(g_vk.device, g_vk.render_done_sems[i], NULL);
        vkDestroyFence(g_vk.device, g_vk.in_flight_fences[i], NULL);
    }
    free(g_vk.image_avail_sems);
    free(g_vk.render_done_sems);
    free(g_vk.in_flight_fences);
fail_cmd:
    vkDestroyCommandPool(g_vk.device, g_vk.cmd_pool, NULL);
fail_depth:
    vkDestroyImageView(g_vk.device, g_vk.depth_view, NULL);
    vkDestroyImage(g_vk.device, g_vk.depth_image, NULL);
    vkFreeMemory(g_vk.device, g_vk.depth_memory, NULL);
fail_swap:
    if (g_vk.swap_views) {
        for (uint32_t i = 0; i < g_vk.swap_image_count; i++)
            vkDestroyImageView(g_vk.device, g_vk.swap_views[i], NULL);
        free(g_vk.swap_views);
    }
    free(g_vk.swap_images);
    vkDestroySwapchainKHR(g_vk.device, g_vk.swapchain, NULL);
fail_dev:
    vkDestroyDevice(g_vk.device, NULL);
    vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    vkDestroyInstance(g_vk.instance, NULL);
    return false;
}

void renderer_shutdown(void) {
    if (g_vk.device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(g_vk.device);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(g_vk.device, g_vk.image_avail_sems[i], NULL);
        vkDestroySemaphore(g_vk.device, g_vk.render_done_sems[i], NULL);
        vkDestroyFence(g_vk.device, g_vk.in_flight_fences[i], NULL);
    }
    vkDestroyCommandPool(g_vk.device, g_vk.cmd_pool, NULL);
    vkDestroyDescriptorPool(g_vk.device, g_vk.desc_pool, NULL);

    for (uint32_t i = 0; i < g_vk.swap_image_count; i++)
        vkDestroyImageView(g_vk.device, g_vk.swap_views[i], NULL);
    vkDestroyImageView(g_vk.device, g_vk.depth_view, NULL);
    vkDestroyImage(g_vk.device, g_vk.depth_image, NULL);
    vkFreeMemory(g_vk.device, g_vk.depth_memory, NULL);
    vkDestroySwapchainKHR(g_vk.device, g_vk.swapchain, NULL);
    vkDestroyPipelineCache(g_vk.device, g_pipeline_cache, NULL);
    vkDestroySampler(g_vk.device, g_vk.default_sampler, NULL);

    if (g_vk.staging_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_vk.device, g_vk.staging_buffer, NULL);
        vkFreeMemory(g_vk.device, g_vk.staging_memory, NULL);
    }

    for (uint32_t i = 0; i < g_vk.pipeline_count; i++) {
        if (g_vk.pipelines[i].pipeline)         vkDestroyPipeline(g_vk.device, g_vk.pipelines[i].pipeline, NULL);
        if (g_vk.pipelines[i].layout)           vkDestroyPipelineLayout(g_vk.device, g_vk.pipelines[i].layout, NULL);
        if (g_vk.pipelines[i].desc_set_layout)  vkDestroyDescriptorSetLayout(g_vk.device, g_vk.pipelines[i].desc_set_layout, NULL);
    }
    for (uint32_t i = 0; i < g_vk.buffer_count; i++) {
        if (g_vk.buffers[i]) {
            vkDestroyBuffer(g_vk.device, g_vk.buffers[i], NULL);
            vkFreeMemory(g_vk.device, g_vk.buffer_memories[i], NULL);
        }
    }
    for (uint32_t i = 0; i < g_vk.texture_count; i++) {
        if (g_vk.texture_views[i])  vkDestroyImageView(g_vk.device, g_vk.texture_views[i], NULL);
        if (g_vk.textures[i]) {
            vkDestroyImage(g_vk.device, g_vk.textures[i], NULL);
            vkFreeMemory(g_vk.device, g_vk.texture_memories[i], NULL);
        }
        if (g_vk.texture_samplers[i] != g_vk.default_sampler &&
            g_vk.texture_samplers[i] != VK_NULL_HANDLE)
            vkDestroySampler(g_vk.device, g_vk.texture_samplers[i], NULL);
    }
    for (uint32_t i = 0; i < g_vk.vao_count; i++) {
        if (g_vk.vaos[i])             vkDestroyBuffer(g_vk.device, g_vk.vaos[i], NULL);
        if (g_vk.vao_index_buffers[i]) vkDestroyBuffer(g_vk.device, g_vk.vao_index_buffers[i], NULL);
    }

    free(g_vk.swap_images);
    free(g_vk.swap_views);
    free(g_vk.pipelines);
    free(g_vk.buffers);
    free(g_vk.buffer_memories);
    free(g_vk.buffer_sizes);
    free(g_vk.textures);
    free(g_vk.texture_memories);
    free(g_vk.texture_views);
    free(g_vk.texture_samplers);
    free(g_vk.texture_widths);
    free(g_vk.texture_heights);
    free(g_vk.texture_depths);
    free(g_vk.vaos);
    free(g_vk.cmd_buffers);
    free(g_vk.image_avail_sems);
    free(g_vk.render_done_sems);
    free(g_vk.in_flight_fences);

    vkDestroyDevice(g_vk.device, NULL);
    vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);

#ifdef ENABLE_VALIDATION
    if (g_debug_messenger != VK_NULL_HANDLE) {
        PFN_vkDestroyDebugUtilsMessengerEXT fn =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                g_vk.instance, "vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(g_vk.instance, g_debug_messenger, NULL);
    }
#endif
    vkDestroyInstance(g_vk.instance, NULL);
}

/* ============================================================================
 * Frame recording state
 * ============================================================================ */

VkCommandBuffer g_active_cmd   = VK_NULL_HANDLE;
bool            g_frame_started = false;
bool            g_in_render_pass = false;
float           g_clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
bool            g_clear_depth   = false;

/* ============================================================================
 * Frame control
 * ============================================================================ */

void renderer_clear(float r, float g, float b, float a) {
    CHECK_DEVICE();
    g_clear_color[0] = r; g_clear_color[1] = g;
    g_clear_color[2] = b; g_clear_color[3] = a;
    g_clear_depth = true;

    if (g_frame_started) return;

    if (g_vk.framebuffer_resized) {
        g_vk.framebuffer_resized = false;
        recreate_swapchain();
    }

    VkResult fence_res = vkWaitForFences(g_vk.device, 1,
                                         &g_vk.in_flight_fences[g_vk.current_frame],
                                         VK_TRUE, FENCE_TIMEOUT_NS);
    if (fence_res == VK_TIMEOUT) {
        fprintf(stderr, "Warning: fence wait timed out, recreating swapchain\n");
        vkResetFences(g_vk.device, 1, &g_vk.in_flight_fences[g_vk.current_frame]);
        recreate_swapchain();
        vkResetFences(g_vk.device, 1, &g_vk.in_flight_fences[g_vk.current_frame]);
    }

    VkResult res = vkAcquireNextImageKHR(g_vk.device, g_vk.swapchain, FENCE_TIMEOUT_NS,
                                         g_vk.image_avail_sems[g_vk.current_frame],
                                         VK_NULL_HANDLE, &g_vk.image_index);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        if (recreate_swapchain())
            res = vkAcquireNextImageKHR(g_vk.device, g_vk.swapchain, UINT64_MAX,
                                        g_vk.image_avail_sems[g_vk.current_frame],
                                        VK_NULL_HANDLE, &g_vk.image_index);
        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_TIMEOUT) return;
    } else if (res == VK_TIMEOUT) {
        vkResetFences(g_vk.device, 1, &g_vk.in_flight_fences[g_vk.current_frame]);
        recreate_swapchain();
        res = vkAcquireNextImageKHR(g_vk.device, g_vk.swapchain, UINT64_MAX,
                                    g_vk.image_avail_sems[g_vk.current_frame],
                                    VK_NULL_HANDLE, &g_vk.image_index);
        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_TIMEOUT) return;
    }

    if (g_vk.image_index >= g_vk.swap_image_count) {
        fprintf(stderr, "Invalid image index %u >= %u\n",
                g_vk.image_index, g_vk.swap_image_count);
        return;
    }

    VkCommandBuffer cmd = g_vk.cmd_buffers[g_vk.current_frame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    g_active_cmd    = cmd;
    g_frame_started = true;

    VkRenderingAttachmentInfo color_att = {0};
    color_att.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_att.imageView   = g_vk.swap_views[g_vk.image_index];
    color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_att.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.clearValue.color.float32[0] = g_clear_color[0];
    color_att.clearValue.color.float32[1] = g_clear_color[1];
    color_att.clearValue.color.float32[2] = g_clear_color[2];
    color_att.clearValue.color.float32[3] = g_clear_color[3];

    VkRenderingAttachmentInfo depth_att = {0};
    depth_att.sType                          = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_att.imageView                      = g_vk.depth_view;
    depth_att.imageLayout                    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_att.loadOp                         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_att.storeOp                        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_att.clearValue.depthStencil.depth  = 1.0f;
    depth_att.clearValue.depthStencil.stencil = 0;

    VkRenderingInfo ri = {0};
    ri.sType                  = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.offset.x    = 0;
    ri.renderArea.offset.y    = 0;
    ri.renderArea.extent      = g_vk.swap_extent;
    ri.layerCount             = 1;
    ri.colorAttachmentCount   = 1;
    ri.pColorAttachments      = &color_att;
    ri.pDepthAttachment       = &depth_att;

    vkCmdBeginRendering(cmd, &ri);
    g_in_render_pass = true;
}

void renderer_swap(void) {
    CHECK_DEVICE();
    if (!g_frame_started || g_active_cmd == VK_NULL_HANDLE) return;

    vkCmdEndRendering(g_active_cmd);
    g_in_render_pass = false;

    VkBuffer       ss_buf = VK_NULL_HANDLE;
    VkDeviceMemory ss_mem = VK_NULL_HANDLE;
    bool ss = g_screenshot_path[0] && screenshot_record_copy(g_active_cmd, &ss_buf, &ss_mem);

    vkEndCommandBuffer(g_active_cmd);

    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo si = {0};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &g_vk.image_avail_sems[g_vk.current_frame];
    si.pWaitDstStageMask    = wait_stages;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &g_vk.cmd_buffers[g_vk.current_frame];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &g_vk.render_done_sems[g_vk.current_frame];

    vkResetFences(g_vk.device, 1, &g_vk.in_flight_fences[g_vk.current_frame]);
    VkResult sub = vkQueueSubmit(g_vk.graphics_queue, 1, &si,
                                 g_vk.in_flight_fences[g_vk.current_frame]);
    if (sub == VK_ERROR_DEVICE_LOST) {
        fprintf(stderr, "VK_ERROR_DEVICE_LOST during submit\n");
        if (ss) { vkDestroyBuffer(g_vk.device, ss_buf, NULL); vkFreeMemory(g_vk.device, ss_mem, NULL); }
        g_screenshot_path[0] = '\0';
        recreate_swapchain();
        g_active_cmd    = VK_NULL_HANDLE;
        g_frame_started = false;
        return;
    }

    VkPresentInfoKHR pi = {0};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &g_vk.render_done_sems[g_vk.current_frame];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &g_vk.swapchain;
    pi.pImageIndices      = &g_vk.image_index;

    VkResult pres = vkQueuePresentKHR(g_vk.present_queue, &pi);
    if (pres == VK_ERROR_DEVICE_LOST) {
        fprintf(stderr, "VK_ERROR_DEVICE_LOST during present\n");
        recreate_swapchain();
    } else if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR ||
               g_vk.framebuffer_resized) {
        g_vk.framebuffer_resized = false;
        recreate_swapchain();
    }

    if (ss) {
        vkWaitForFences(g_vk.device, 1, &g_vk.in_flight_fences[g_vk.current_frame],
                        VK_TRUE, UINT64_MAX);
        screenshot_write_ppm(ss_mem);
        vkDestroyBuffer(g_vk.device, ss_buf, NULL);
        vkFreeMemory(g_vk.device, ss_mem, NULL);
        g_screenshot_path[0] = '\0';
    } else {
        g_screenshot_path[0] = '\0'; /* clear even if capture couldn't be recorded */
    }

    g_vk.current_frame = (g_vk.current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
    g_active_cmd       = VK_NULL_HANDLE;
    g_frame_started    = false;
}

void renderer_swap_interval(int interval) {
    VkPresentModeKHR mode = (interval == 0)
        ? VK_PRESENT_MODE_MAILBOX_KHR
        : VK_PRESENT_MODE_FIFO_KHR;
    if (g_vk.present_mode == mode) return;
    g_vk.present_mode = mode;
    recreate_swapchain();
}

void renderer_clear_depth(void) { /* no-op; depth is cleared in renderer_clear */ }

void renderer_get_size(int *width, int *height) {
    *width  = g_vk.width;
    *height = g_vk.height;
}

void renderer_viewport(int x, int y, int width, int height) {
    (void)x; (void)y;
    if (g_vk.width != width || g_vk.height != height) {
        g_vk.width  = width;
        g_vk.height = height;
        g_vk.framebuffer_resized = true;
    }
}

void renderer_enable(R_Cap cap)         { (void)cap; }
void renderer_disable(R_Cap cap)        { (void)cap; }
void renderer_depth_mask(bool write)    { (void)write; }
void renderer_depth_func(R_DepthFunc f) { (void)f; }
void renderer_blend_func(R_BlendFactor src, R_BlendFactor dst) { (void)src; (void)dst; }

void renderer_polygon_offset(float factor, float units) {
    g_vk.poly_offset_factor = factor;
    g_vk.poly_offset_units  = units;
}

void renderer_line_width(float width) { g_vk.line_width = width; }
void renderer_push_attrib(void)       {}
void renderer_pop_attrib(void)        {}
