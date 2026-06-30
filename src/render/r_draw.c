#include "render_internal.h"

static void setup_dynamic_state(void) {
    VkViewport vp = {0};
    vp.x        = 0;
    vp.y        = (float)g_vk.swap_extent.height;
    vp.width    = (float)g_vk.swap_extent.width;
    vp.height   = -(float)g_vk.swap_extent.height; /* flip Y for right-handed NDC */
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(g_active_cmd, 0, 1, &vp);

    VkRect2D scissor = {0};
    scissor.extent = g_vk.swap_extent;
    vkCmdSetScissor(g_active_cmd, 0, 1, &scissor);

    vkCmdSetLineWidth(g_active_cmd, g_vk.line_width);
    vkCmdSetDepthBias(g_active_cmd, g_vk.poly_offset_factor, 0.0f, 0.0f);
}

void renderer_draw_arrays(R_Primitive primitive, int first, int count) {
    CHECK_DEVICE();
    if (g_active_cmd == VK_NULL_HANDLE) return;
    if (g_vk.active_pipeline >= g_vk.pipeline_count) return;
    if (g_vk.bound_vbo == VK_NULL_HANDLE && g_vao_state.buffer == VK_NULL_HANDLE) return;

    Pipeline *pipe = &g_vk.pipelines[g_vk.active_pipeline];
    vkCmdBindPipeline(g_active_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);

    VkDescriptorSet ds = pipe->desc_sets[g_frame_index % MAX_FRAMES_IN_FLIGHT];
    if (ds != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(g_active_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipe->layout, 0, 1, &ds, 0, NULL);

    if (g_push_dirty) {
        vkCmdPushConstants(g_active_cmd, pipe->layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, R_PUSH_CONSTANT_SIZE, g_push_constants);
        g_push_dirty = false;
    }

    VkBuffer vbo = (g_vk.bound_vbo != VK_NULL_HANDLE) ? g_vk.bound_vbo : g_vao_state.buffer;
    if (vbo != VK_NULL_HANDLE) {
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(g_active_cmd, 0, 1, &vbo, &off);
    }

    setup_dynamic_state();
    (void)primitive;
    vkCmdDraw(g_active_cmd, (uint32_t)count, 1, (uint32_t)first, 0);
}

void renderer_draw_elements(R_Primitive primitive, int count, int offset) {
    CHECK_DEVICE();
    if (g_active_cmd == VK_NULL_HANDLE) return;
    if (g_vk.active_pipeline >= g_vk.pipeline_count) return;
    if (g_vk.bound_index_buffer == VK_NULL_HANDLE) return;

    Pipeline *pipe = &g_vk.pipelines[g_vk.active_pipeline];
    vkCmdBindPipeline(g_active_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);

    VkDescriptorSet ds2 = pipe->desc_sets[g_frame_index % MAX_FRAMES_IN_FLIGHT];
    if (ds2 != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(g_active_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipe->layout, 0, 1, &ds2, 0, NULL);

    if (g_push_dirty) {
        vkCmdPushConstants(g_active_cmd, pipe->layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, R_PUSH_CONSTANT_SIZE, g_push_constants);
        g_push_dirty = false;
    }

    VkBuffer vbo = (g_vk.bound_vbo != VK_NULL_HANDLE) ? g_vk.bound_vbo : g_vao_state.buffer;
    if (vbo != VK_NULL_HANDLE) {
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(g_active_cmd, 0, 1, &vbo, &off);
    }
    vkCmdBindIndexBuffer(g_active_cmd, g_vk.bound_index_buffer, 0, VK_INDEX_TYPE_UINT16);

    setup_dynamic_state();
    (void)primitive;
    vkCmdDrawIndexed(g_active_cmd, (uint32_t)count, 1, (uint32_t)offset, 0, 0);
}

void renderer_draw_arrays_indirect(void) {
    fprintf(stderr, "[renderer] UNIMPLEMENTED: %s\n", __func__);
}

void renderer_dispatch_compute(int groups_x, int groups_y, int groups_z) {
    (void)groups_x; (void)groups_y; (void)groups_z;
    fprintf(stderr, "[renderer] UNIMPLEMENTED: %s\n", __func__);
}

void renderer_memory_barrier(R_BarrierBits bits) {
    (void)bits;
    fprintf(stderr, "[renderer] UNIMPLEMENTED: %s\n", __func__);
}
