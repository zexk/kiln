#include "render_internal.h"

/* Bridge to the rich renderer (render_vk.c) — resolved at link time.
   The thin renderer borrows the Vulkan context rather than creating its own. */
extern void  render_vk_get_context(VkInstance *inst, VkPhysicalDevice *phys,
                                    VkDevice *dev, VkQueue *queue,
                                    uint32_t *family, VkFormat *fmt,
                                    VkExtent2D *ext);
extern void  render_set_overlay_hook(void (*fn)(void *ud), void *ud);
extern void *render_get_overlay_cmd(void);
extern void  render_get_overlay_extent(uint32_t *w, uint32_t *h);
extern int   render_get_frame_index(void);

/* User overlay callback registered via renderer_set_overlay_fn(). */
static void (*g_user_overlay_fn)(void *ud) = NULL;
static void  *g_user_overlay_ud            = NULL;

/* Registered with the rich renderer; fired each frame inside the overlay pass.
   Sets g_active_cmd so thin-renderer draw calls record into the right buffer. */
static void r_overlay_hook(void *ud) {
    (void)ud;
    g_active_cmd  = (VkCommandBuffer)render_get_overlay_cmd();
    g_frame_index = render_get_frame_index();
    uint32_t w, h;
    render_get_overlay_extent(&w, &h);
    g_vk.swap_extent.width  = w;
    g_vk.swap_extent.height = h;
    r_deferred_deletes_flush();
    if (g_user_overlay_fn)
        g_user_overlay_fn(g_user_overlay_ud);
    g_active_cmd = VK_NULL_HANDLE;
}

void renderer_set_overlay_fn(void (*fn)(void *ud), void *ud) {
    g_user_overlay_fn = fn;
    g_user_overlay_ud = ud;
}

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
R_Texture g_bound_textures[16]     = {
    R_INVALID_HANDLE, R_INVALID_HANDLE, R_INVALID_HANDLE, R_INVALID_HANDLE,
    R_INVALID_HANDLE, R_INVALID_HANDLE, R_INVALID_HANDLE, R_INVALID_HANDLE,
    R_INVALID_HANDLE, R_INVALID_HANDLE, R_INVALID_HANDLE, R_INVALID_HANDLE,
    R_INVALID_HANDLE, R_INVALID_HANDLE, R_INVALID_HANDLE, R_INVALID_HANDLE,
};

VAOState g_vao_state  = {0};
R_VAO    g_current_vao = R_INVALID_HANDLE;

/* Screenshots are handled by the rich renderer (render_save_screenshot). */
void renderer_save_screenshot(const char *path) { (void)path; }

/* ============================================================================
 * Helpers (definitions referenced by all translation units)
 * ============================================================================ */

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
    (void)native;
    memset(&g_vk, 0, sizeof(g_vk));
    g_vk.width  = width;
    g_vk.height = height;

    /* Borrow device/queue/format from the rich renderer instead of creating
       a second Vulkan context.  render_init() must have been called first. */
    render_vk_get_context(&g_vk.instance, &g_vk.physical_device,
                           &g_vk.device, &g_vk.graphics_queue,
                           &g_vk.graphics_family, &g_vk.swap_format,
                           &g_vk.swap_extent);

    if (g_vk.device == VK_NULL_HANDLE) {
        fprintf(stderr, "[renderer] render_init must be called before renderer_init\n");
        return false;
    }

    /* Thin renderer uses the graphics queue for one-time transfers. */
    g_vk.compute_family = g_vk.graphics_family;
    g_vk.compute_queue  = g_vk.graphics_queue;
    g_vk.present_family = g_vk.graphics_family;
    g_vk.present_queue  = g_vk.graphics_queue;

    if (!create_command_pool()) {
        fprintf(stderr, "[renderer] command pool failed\n");
        g_vk.device = VK_NULL_HANDLE;
        return false;
    }
    if (!create_descriptor_pool()) {
        fprintf(stderr, "[renderer] descriptor pool failed\n");
        vkDestroyCommandPool(g_vk.device, g_vk.cmd_pool, NULL);
        g_vk.device = VK_NULL_HANDLE;
        return false;
    }
    if (!create_default_sampler()) {
        fprintf(stderr, "[renderer] default sampler failed\n");
        vkDestroyDescriptorPool(g_vk.device, g_vk.desc_pool, NULL);
        vkDestroyCommandPool(g_vk.device, g_vk.cmd_pool, NULL);
        g_vk.device = VK_NULL_HANDLE;
        return false;
    }

    init_resource_arrays();

    g_vk.staging_size = 16 * 1024 * 1024;
    if (!vk_create_buffer(g_vk.staging_size,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &g_vk.staging_buffer,
                          &g_vk.staging_memory)) {
        fprintf(stderr, "[renderer] staging buffer failed\n");
        renderer_shutdown();
        return false;
    }

    g_vk.cull_mode          = VK_CULL_MODE_BACK_BIT;
    g_vk.topology           = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    g_vk.line_width         = 1.0f;
    g_vk.poly_offset_factor = 0.0f;
    g_vk.poly_offset_units  = 0.0f;

    render_set_overlay_hook(r_overlay_hook, NULL);
    fprintf(stderr, "[renderer] initialized (shared Vulkan context)\n");
    return true;
}

void renderer_shutdown(void) {
    if (g_vk.device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(g_vk.device);
    r_deferred_deletes_flush_all();

    /* Unregister from the rich renderer's frame loop. */
    render_set_overlay_hook(NULL, NULL);

    /* Only destroy resources the thin renderer owns.
       device / instance / surface / swapchain / depth / sync objects
       are owned by the rich renderer and must not be touched here. */
    vkDestroySampler(g_vk.device, g_vk.default_sampler, NULL);
    vkDestroyDescriptorPool(g_vk.device, g_vk.desc_pool, NULL);
    vkDestroyCommandPool(g_vk.device, g_vk.cmd_pool, NULL);

    if (g_vk.staging_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_vk.device, g_vk.staging_buffer, NULL);
        vkFreeMemory(g_vk.device, g_vk.staging_memory, NULL);
    }

    for (uint32_t i = 0; i < g_vk.pipeline_count; i++) {
        if (g_vk.pipelines[i].pipeline)        vkDestroyPipeline(g_vk.device, g_vk.pipelines[i].pipeline, NULL);
        if (g_vk.pipelines[i].layout)          vkDestroyPipelineLayout(g_vk.device, g_vk.pipelines[i].layout, NULL);
        if (g_vk.pipelines[i].desc_set_layout) vkDestroyDescriptorSetLayout(g_vk.device, g_vk.pipelines[i].desc_set_layout, NULL);
        if (g_vk.pipelines[i].vert_module)     vkDestroyShaderModule(g_vk.device, g_vk.pipelines[i].vert_module, NULL);
        if (g_vk.pipelines[i].frag_module)     vkDestroyShaderModule(g_vk.device, g_vk.pipelines[i].frag_module, NULL);
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
        if (g_vk.texture_samplers &&
            g_vk.texture_samplers[i] != g_vk.default_sampler &&
            g_vk.texture_samplers[i] != VK_NULL_HANDLE)
            vkDestroySampler(g_vk.device, g_vk.texture_samplers[i], NULL);
    }
    /* VAOs don't own their buffers — ownership is in g_vk.buffers[]; no destroy here. */

    free(g_vk.pipelines);
    free(g_vk.buffers);
    free(g_vk.buffer_memories);
    free(g_vk.buffer_sizes);
    free(g_vk.buffer_free);
    free(g_vk.textures);
    free(g_vk.texture_memories);
    free(g_vk.texture_views);
    free(g_vk.texture_samplers);
    free(g_vk.texture_widths);
    free(g_vk.texture_heights);
    free(g_vk.texture_depths);
    free(g_vk.vaos);
    free(g_vk.vao_free);

    /* device / instance / surface / swapchain / depth image / sync objects
       are owned by the rich renderer — do not destroy them here. */
    g_vk.device = VK_NULL_HANDLE;
}

/* ============================================================================
 * Frame recording state
 * ============================================================================ */

VkCommandBuffer g_active_cmd    = VK_NULL_HANDLE;
bool            g_frame_started  = false;
bool            g_in_render_pass = false;
float           g_clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
bool            g_clear_depth    = false;
int             g_frame_index    = 0;

/* ============================================================================
 * Frame control
 * ============================================================================ */

void renderer_clear(float r, float g, float b, float a) {
    /* Frame management is owned by the rich renderer; just record state. */
    g_clear_color[0] = r; g_clear_color[1] = g;
    g_clear_color[2] = b; g_clear_color[3] = a;
    g_clear_depth = true;
}

/* Frame submission is owned by the rich renderer — renderer_swap is a no-op. */
void renderer_swap(void) {}

/* VSynck is controlled by the rich renderer via render_set_vsync(). */
void renderer_swap_interval(int interval) { (void)interval; }

void renderer_clear_depth(void) { /* no-op; depth is cleared in renderer_clear */ }

void renderer_get_size(int *width, int *height) {
    *width  = g_vk.width;
    *height = g_vk.height;
}

void renderer_viewport(int x, int y, int width, int height) {
    (void)x; (void)y;
    g_vk.width              = width;
    g_vk.height             = height;
    g_vk.swap_extent.width  = (uint32_t)width;
    g_vk.swap_extent.height = (uint32_t)height;
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
