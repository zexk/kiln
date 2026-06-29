#include "render_internal.h"

void copy_to_buffer(VkBuffer dst, VkDeviceSize dst_offset, VkDeviceSize size, const void *data) {
    assert(size <= g_vk.staging_size && "staging buffer too small");

    void *mapped;
    if (vkMapMemory(g_vk.device, g_vk.staging_memory, 0, size, 0, &mapped) != VK_SUCCESS) return;
    memcpy(mapped, data, size);
    vkUnmapMemory(g_vk.device, g_vk.staging_memory);

    if (g_active_cmd != VK_NULL_HANDLE && !g_in_render_pass) {
        VkBufferCopy copy = {.srcOffset = 0, .dstOffset = dst_offset, .size = size};
        vkCmdCopyBuffer(g_active_cmd, g_vk.staging_buffer, dst, 1, &copy);
    } else {
        VkCommandBuffer cmd = begin_one_time_cmd();
        if (cmd == VK_NULL_HANDLE) return;
        VkBufferCopy copy = {.srcOffset = 0, .dstOffset = dst_offset, .size = size};
        vkCmdCopyBuffer(cmd, g_vk.staging_buffer, dst, 1, &copy);
        end_one_time_cmd(cmd);
    }
}

/* ============================================================================
 * Public API: buffers
 * ============================================================================ */

R_Buffer renderer_create_buffer(void) {
    CHECK_DEVICE_RET(R_INVALID_HANDLE);
    uint32_t idx;
    if (g_vk.buffer_free_count > 0)        idx = g_vk.buffer_free[--g_vk.buffer_free_count];
    else if (g_vk.buffer_count < MAX_BUFFERS) idx = g_vk.buffer_count++;
    else return R_INVALID_HANDLE;
    g_vk.buffers[idx]         = VK_NULL_HANDLE;
    g_vk.buffer_memories[idx] = VK_NULL_HANDLE;
    g_vk.buffer_sizes[idx]    = 0;
    return idx;
}

void renderer_destroy_buffer(R_Buffer buffer) {
    CHECK_DEVICE();
    if (buffer >= g_vk.buffer_count) return;
    if (g_vk.buffers[buffer]) {
        vkDestroyBuffer(g_vk.device, g_vk.buffers[buffer], NULL);
        vkFreeMemory(g_vk.device, g_vk.buffer_memories[buffer], NULL);
        g_vk.buffers[buffer]         = VK_NULL_HANDLE;
        g_vk.buffer_memories[buffer] = VK_NULL_HANDLE;
    }
    g_vk.buffer_sizes[buffer] = 0;
    /* Reclaim the slot so a streaming world doesn't exhaust the pool. */
    g_vk.buffer_free[g_vk.buffer_free_count++] = (uint32_t)buffer;
}

void renderer_bind_buffer(R_BufferTarget target, R_Buffer buffer) {
    if (buffer == R_INVALID_HANDLE) {
        g_vk.bound_vbo          = VK_NULL_HANDLE;
        g_vk.bound_vbo_handle   = R_INVALID_HANDLE;
        g_vk.bound_index_buffer = VK_NULL_HANDLE;
        g_vk.bound_ibo_handle   = R_INVALID_HANDLE;
        return;
    }
    if (buffer >= g_vk.buffer_count) return;
    VkBuffer vk_buf = g_vk.buffers[buffer];
    if (target == R_BUF_ELEMENT) {
        g_vk.bound_index_buffer = vk_buf;
        g_vk.bound_ibo_handle   = buffer;
        if (g_current_vao != R_INVALID_HANDLE && g_current_vao < MAX_VAO)
            g_vk.vao_index_buffers[g_current_vao] = vk_buf;
    } else {
        g_vk.bound_vbo        = vk_buf;
        g_vk.bound_vbo_handle = buffer;
        if (g_current_vao != R_INVALID_HANDLE && g_current_vao < MAX_VAO)
            g_vk.vao_buffers[g_current_vao] = vk_buf;
    }
}

void renderer_buffer_data(R_BufferTarget target, size_t size, const void *data, R_Usage usage) {
    CHECK_DEVICE();
    (void)usage;

    R_Buffer h = (target == R_BUF_ELEMENT) ? g_vk.bound_ibo_handle : g_vk.bound_vbo_handle;
    if (h == R_INVALID_HANDLE || h >= g_vk.buffer_count) return;

    bool reuse = (g_vk.buffers[h] != VK_NULL_HANDLE && size <= g_vk.buffer_sizes[h]);
    if (!reuse && g_vk.buffers[h]) {
        vkDestroyBuffer(g_vk.device, g_vk.buffers[h], NULL);
        vkFreeMemory(g_vk.device, g_vk.buffer_memories[h], NULL);
        g_vk.buffers[h]         = VK_NULL_HANDLE;
        g_vk.buffer_memories[h] = VK_NULL_HANDLE;
    }

    VkBufferUsageFlags uf = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if      (target == R_BUF_ARRAY)          uf |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    else if (target == R_BUF_ELEMENT)        uf |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    else if (target == R_BUF_SHADER_STORAGE) uf |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    else if (target == R_BUF_DRAW_INDIRECT)  uf |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

    VkMemoryPropertyFlags mf = (size > (size_t)(1024 * 1024))
        ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    if (!reuse) {
        if (!vk_create_buffer(size, uf, mf, &g_vk.buffers[h], &g_vk.buffer_memories[h])) return;
        g_vk.buffer_sizes[h] = size;
    }

    if (data && size > 0) {
        if (mf & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            void *mapped;
            if (vkMapMemory(g_vk.device, g_vk.buffer_memories[h], 0, size, 0, &mapped) == VK_SUCCESS) {
                memcpy(mapped, data, size);
                vkUnmapMemory(g_vk.device, g_vk.buffer_memories[h]);
            }
        } else {
            copy_to_buffer(g_vk.buffers[h], 0, size, data);
        }
    }

    if (target == R_BUF_ELEMENT) {
        g_vk.bound_index_buffer = g_vk.buffers[h];
        if (g_current_vao != R_INVALID_HANDLE && g_current_vao < MAX_VAO)
            g_vk.vao_index_buffers[g_current_vao] = g_vk.buffers[h];
    } else {
        g_vk.bound_vbo = g_vk.buffers[h];
        if (g_current_vao != R_INVALID_HANDLE && g_current_vao < MAX_VAO)
            g_vk.vao_buffers[g_current_vao] = g_vk.buffers[h];
    }
}

void renderer_buffer_sub_data(R_BufferTarget target, size_t offset, size_t size, const void *data) {
    if (!data || size == 0) return;
    R_Buffer h = (target == R_BUF_ELEMENT) ? g_vk.bound_ibo_handle : g_vk.bound_vbo_handle;
    if (h == R_INVALID_HANDLE || h >= g_vk.buffer_count) return;
    if (offset + size > g_vk.buffer_sizes[h]) return;

    void *mapped;
    if (vkMapMemory(g_vk.device, g_vk.buffer_memories[h], 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
        memcpy((char *)mapped + offset, data, size);
        vkUnmapMemory(g_vk.device, g_vk.buffer_memories[h]);
    } else {
        copy_to_buffer(g_vk.buffers[h], offset, size, data);
    }
}

void renderer_get_buffer_sub_data(R_BufferTarget target, size_t offset, size_t size, void *data) {
    (void)target; (void)offset; (void)size; (void)data;
    fprintf(stderr, "[renderer] UNIMPLEMENTED: %s\n", __func__);
}

void renderer_bind_buffer_base(R_BufferTarget target, int index, R_Buffer buffer) {
    (void)target; (void)index; (void)buffer;
    fprintf(stderr, "[renderer] UNIMPLEMENTED: %s\n", __func__);
}

/* ============================================================================
 * Public API: VAOs
 * ============================================================================ */

R_VAO renderer_create_vao(void) {
    CHECK_DEVICE_RET(R_INVALID_HANDLE);
    uint32_t idx;
    if (g_vk.vao_free_count > 0)        idx = g_vk.vao_free[--g_vk.vao_free_count];
    else if (g_vk.vao_count < MAX_VAO)  idx = g_vk.vao_count++;
    else return R_INVALID_HANDLE;
    g_vk.vao_buffers[idx]       = VK_NULL_HANDLE;
    g_vk.vao_index_buffers[idx] = VK_NULL_HANDLE;
    return idx;
}

void renderer_destroy_vao(R_VAO vao) {
    CHECK_DEVICE();
    if (vao >= g_vk.vao_count) return;
    /* VAOs own no GPU objects (they cache buffer handles owned elsewhere);
       just reclaim the slot so streaming chunk meshes can reuse it. */
    g_vk.vao_buffers[vao]       = VK_NULL_HANDLE;
    g_vk.vao_index_buffers[vao] = VK_NULL_HANDLE;
    g_vk.vao_free[g_vk.vao_free_count++] = (uint32_t)vao;
}

void renderer_bind_vao(R_VAO vao) {
    if (vao == R_INVALID_HANDLE) {
        g_current_vao              = R_INVALID_HANDLE;
        g_vao_state.buffer         = VK_NULL_HANDLE;
        g_vao_state.index_buffer   = VK_NULL_HANDLE;
        return;
    }
    if (vao >= g_vk.vao_count) return;
    g_current_vao              = vao;
    g_vao_state.buffer         = g_vk.vao_buffers[vao];
    g_vao_state.index_buffer   = g_vk.vao_index_buffers[vao];
    /* Discard stale explicit bind; streaming callers re-set it via renderer_bind_buffer. */
    g_vk.bound_vbo             = VK_NULL_HANDLE;
    g_vk.bound_vbo_handle      = R_INVALID_HANDLE;
}

void renderer_attrib_pointer(int index, int size, R_Type type,
                             bool normalized, int stride, int offset) {
    (void)index; (void)size; (void)type; (void)normalized; (void)stride; (void)offset;
}

void renderer_enable_attrib(int index) { (void)index; }
