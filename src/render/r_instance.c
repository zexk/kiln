#include "render_internal.h"

bool create_descriptor_pool(void) {
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32},
    };
    VkDescriptorPoolCreateInfo ci = {0};
    ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.poolSizeCount = 2;
    ci.pPoolSizes    = sizes;
    ci.maxSets       = 256;
    if (vkCreateDescriptorPool(g_vk.device, &ci, NULL, &g_vk.desc_pool) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create descriptor pool\n");
        return false;
    }
    return true;
}

bool create_default_sampler(void) {
    VkSamplerCreateInfo ci = {0};
    ci.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter        = VK_FILTER_NEAREST;
    ci.minFilter        = VK_FILTER_NEAREST;
    ci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.anisotropyEnable = VK_FALSE;
    ci.maxAnisotropy    = 1.0f;
    ci.borderColor      = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    ci.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(g_vk.device, &ci, NULL, &g_vk.default_sampler) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create default sampler\n");
        return false;
    }
    return true;
}

void init_resource_arrays(void) {
    g_vk.pipelines        = calloc(MAX_PIPELINES, sizeof(Pipeline));
    g_vk.buffers          = calloc(MAX_BUFFERS,   sizeof(VkBuffer));
    g_vk.buffer_memories  = calloc(MAX_BUFFERS,   sizeof(VkDeviceMemory));
    g_vk.buffer_sizes     = calloc(MAX_BUFFERS,   sizeof(uint64_t));
    g_vk.textures         = calloc(MAX_TEXTURES,  sizeof(VkImage));
    g_vk.texture_memories = calloc(MAX_TEXTURES,  sizeof(VkDeviceMemory));
    g_vk.texture_views    = calloc(MAX_TEXTURES,  sizeof(VkImageView));
    g_vk.texture_samplers = calloc(MAX_TEXTURES,  sizeof(VkSampler));
    g_vk.texture_widths   = calloc(MAX_TEXTURES,  sizeof(uint32_t));
    g_vk.texture_heights  = calloc(MAX_TEXTURES,  sizeof(uint32_t));
    g_vk.texture_depths   = calloc(MAX_TEXTURES,  sizeof(uint32_t));
    g_vk.vaos             = calloc(MAX_VAO,        sizeof(VkBuffer));
    g_vk.buffer_free      = calloc(MAX_BUFFERS,    sizeof(uint32_t));
    g_vk.vao_free         = calloc(MAX_VAO,        sizeof(uint32_t));

    g_vk.buffer_count      = 0;
    g_vk.buffer_free_count = 0;
    g_vk.texture_count     = 0;
    g_vk.vao_count         = 0;
    g_vk.vao_free_count    = 0;
    g_vk.pipeline_count    = 0;
    g_vk.active_pipeline   = 0;

    g_vk.bound_vbo          = VK_NULL_HANDLE;
    g_vk.bound_index_buffer = VK_NULL_HANDLE;
}
