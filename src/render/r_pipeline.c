#include "render_internal.h"

char *load_spirv_file(const char *path, size_t *out_size) {
    char spv_path[512];
    snprintf(spv_path, sizeof(spv_path), "%s.spv", path);

    char *resolved = platform_resolve_path(spv_path);
    if (!resolved) return NULL;

    FILE *f = fopen(resolved, "rb");
    if (!f) { fprintf(stderr, "Failed to open shader: %s\n", resolved); free(resolved); return NULL; }
    free(resolved);

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fprintf(stderr, "Invalid shader size\n"); fclose(f); return NULL; }

    char *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "Failed to read shader\n");
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

VkShaderModule load_shader_module(const char *path) {
    size_t size;
    char *code = load_spirv_file(path, &size);
    if (!code) return VK_NULL_HANDLE;
    VkShaderModule mod = create_shader_module(code, size);
    free(code);
    return mod;
}

VkDescriptorSetLayout create_texture_descriptor_layout(void) {
    VkDescriptorSetLayoutBinding b = {0};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo ci = {0};
    ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 1;
    ci.pBindings    = &b;

    VkDescriptorSetLayout layout;
    if (vkCreateDescriptorSetLayout(g_vk.device, &ci, NULL, &layout) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return layout;
}

VkPipelineLayout create_pipeline_layout(VkDescriptorSetLayout tex_layout) {
    VkPushConstantRange pc = {0};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset     = 0;
    pc.size       = 256;

    VkPipelineLayoutCreateInfo ci = {0};
    ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount         = tex_layout ? 1 : 0;
    ci.pSetLayouts            = tex_layout ? &tex_layout : NULL;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges    = &pc;

    VkPipelineLayout layout;
    if (vkCreatePipelineLayout(g_vk.device, &ci, NULL, &layout) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return layout;
}

void get_pipeline_config(const char *vert_path, const char *frag_path, PipelineConfig *cfg) {
    (void)frag_path;

    *cfg = (PipelineConfig){
        .vformat            = VERTEX_FORMAT_TERRAIN,
        .topology           = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .depth_test_enable  = VK_TRUE,
        .depth_write_enable = VK_TRUE,
        .depth_compare      = VK_COMPARE_OP_LESS,
        .cull_mode          = VK_CULL_MODE_BACK_BIT,
        .blend_enable       = VK_FALSE,
        .blend_src          = VK_BLEND_FACTOR_SRC_ALPHA,
        .blend_dst          = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .has_texture        = true,
        .depth_bias_enable  = VK_FALSE,
        .push_constant_size = 256,
    };

    if (strstr(vert_path, "skybox")) {
        cfg->vformat            = VERTEX_FORMAT_SKYBOX;
        cfg->depth_write_enable = VK_FALSE;
        cfg->depth_compare      = VK_COMPARE_OP_LESS_OR_EQUAL;
        cfg->cull_mode          = VK_CULL_MODE_NONE;
        cfg->has_texture        = false;
        cfg->push_constant_size = 128;
    } else if (strstr(vert_path, "basic")) {
        cfg->cull_mode = VK_CULL_MODE_NONE;
    } else if (strstr(vert_path, "outline")) {
        cfg->vformat            = VERTEX_FORMAT_OUTLINE;
        cfg->topology           = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        cfg->depth_write_enable = VK_FALSE;
        cfg->cull_mode          = VK_CULL_MODE_NONE;
        cfg->depth_bias_enable  = VK_TRUE;
        cfg->has_texture        = false;
        cfg->push_constant_size = 208;
    } else if (strstr(vert_path, "ui")) {
        cfg->vformat            = VERTEX_FORMAT_UI;
        cfg->depth_test_enable  = VK_FALSE;
        cfg->depth_write_enable = VK_FALSE;
        cfg->cull_mode          = VK_CULL_MODE_NONE;
        cfg->blend_enable       = VK_TRUE;
        cfg->has_texture        = true;
        cfg->push_constant_size = 8;
    } else if (strstr(vert_path, "hud")) {
        cfg->vformat            = VERTEX_FORMAT_HUD;
        cfg->topology           = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        cfg->depth_test_enable  = VK_FALSE;
        cfg->depth_write_enable = VK_FALSE;
        cfg->cull_mode          = VK_CULL_MODE_NONE;
        cfg->blend_enable       = VK_TRUE;
        cfg->has_texture        = false;
        cfg->push_constant_size = 16;
    }
}

VkPipeline create_graphics_pipeline(VkShaderModule vert, VkShaderModule frag,
                                    VkPipelineLayout layout, const PipelineConfig *cfg) {
    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription binding = {0};
    binding.binding   = 0;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[5] = {0};
    uint32_t attr_count = 0;

    switch (cfg->vformat) {
    case VERTEX_FORMAT_TERRAIN:
        binding.stride = 64;
        attrs[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attrs[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32_SFLOAT, 16};
        attrs[2] = (VkVertexInputAttributeDescription){2, 0, VK_FORMAT_R32G32B32_SFLOAT, 32};
        attrs[3] = (VkVertexInputAttributeDescription){3, 0, VK_FORMAT_R32_SFLOAT,        44};
        attrs[4] = (VkVertexInputAttributeDescription){4, 0, VK_FORMAT_R32G32_SFLOAT,     48};
        attr_count = 5;
        break;
    case VERTEX_FORMAT_SKYBOX:
    case VERTEX_FORMAT_OUTLINE:
        binding.stride = 12;
        attrs[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attr_count = 1;
        break;
    case VERTEX_FORMAT_HUD:
        binding.stride = 8;
        attrs[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
        attr_count = 1;
        break;
    case VERTEX_FORMAT_UI:
        binding.stride = 20;
        attrs[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT,   0};
        attrs[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32_SFLOAT,   8};
        attrs[2] = (VkVertexInputAttributeDescription){2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16};
        attr_count = 3;
        break;
    }

    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &binding;
    vi.vertexAttributeDescriptionCount = attr_count;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = cfg->topology;

    VkPipelineViewportStateCreateInfo vs = {0};
    vs.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vs.viewportCount = 1;
    vs.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster = {0};
    raster.sType            = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode      = VK_POLYGON_MODE_FILL;
    raster.cullMode         = cfg->cull_mode;
    raster.frontFace        = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthBiasEnable  = cfg->depth_bias_enable;
    raster.lineWidth        = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = cfg->depth_test_enable;
    ds.depthWriteEnable = cfg->depth_write_enable;
    ds.depthCompareOp   = cfg->depth_compare;

    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable         = cfg->blend_enable;
    cba.srcColorBlendFactor = cfg->blend_src;
    cba.dstColorBlendFactor = cfg->blend_dst;
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = cfg->blend_src;
    cba.dstAlphaBlendFactor = cfg->blend_dst;
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo cb = {0};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    VkDynamicState dyn_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_LINE_WIDTH,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
    };
    VkPipelineDynamicStateCreateInfo dyn = {0};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 4;
    dyn.pDynamicStates    = dyn_states;

    VkPipelineRenderingCreateInfo rendering = {0};
    rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount    = 1;
    rendering.pColorAttachmentFormats = &g_vk.swap_format;
    rendering.depthAttachmentFormat   = VK_FORMAT_D32_SFLOAT;

    VkGraphicsPipelineCreateInfo ci = {0};
    ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext               = &rendering;
    ci.stageCount          = 2;
    ci.pStages             = stages;
    ci.pVertexInputState   = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState      = &vs;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState   = &ms;
    ci.pDepthStencilState  = &ds;
    ci.pColorBlendState    = &cb;
    ci.pDynamicState       = &dyn;
    ci.layout              = layout;
    ci.renderPass          = VK_NULL_HANDLE;

    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(g_vk.device, g_pipeline_cache, 1, &ci, NULL, &pipeline) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pipeline;
}

/* ============================================================================
 * Public API: programs / uniforms
 * ============================================================================ */

R_Program renderer_create_program(const char *vert_path, const char *frag_path) {
    CHECK_DEVICE_RET(R_INVALID_HANDLE);
    if (g_vk.pipeline_count >= MAX_PIPELINES) return R_INVALID_HANDLE;

    VkShaderModule vert = load_shader_module(vert_path);
    VkShaderModule frag = load_shader_module(frag_path);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        if (vert) vkDestroyShaderModule(g_vk.device, vert, NULL);
        if (frag) vkDestroyShaderModule(g_vk.device, frag, NULL);
        return R_INVALID_HANDLE;
    }

    PipelineConfig cfg;
    get_pipeline_config(vert_path, frag_path, &cfg);

    uint32_t idx  = g_vk.pipeline_count++;
    Pipeline *pipe = &g_vk.pipelines[idx];
    pipe->vert_module = vert;
    pipe->frag_module = frag;

    if (cfg.has_texture) {
        pipe->desc_set_layout = create_texture_descriptor_layout();
    } else {
        VkDescriptorSetLayoutCreateInfo li = {0};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        if (vkCreateDescriptorSetLayout(g_vk.device, &li, NULL, &pipe->desc_set_layout) != VK_SUCCESS)
            pipe->desc_set_layout = VK_NULL_HANDLE;
    }

    pipe->layout   = create_pipeline_layout(pipe->desc_set_layout);
    pipe->pipeline = create_graphics_pipeline(vert, frag, pipe->layout, &cfg);

    if (cfg.has_texture) {
        VkDescriptorSetAllocateInfo ai = {0};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = g_vk.desc_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &pipe->desc_set_layout;
        if (vkAllocateDescriptorSets(g_vk.device, &ai, &pipe->desc_set) != VK_SUCCESS)
            pipe->desc_set = VK_NULL_HANDLE;
    } else {
        pipe->desc_set = VK_NULL_HANDLE;
    }

    return idx;
}

R_Program renderer_create_compute(const char *comp_path) {
    (void)comp_path;
    return R_INVALID_HANDLE;
}

void renderer_destroy_program(R_Program program) {
    CHECK_DEVICE();
    if (program >= g_vk.pipeline_count) return;
    Pipeline *pipe = &g_vk.pipelines[program];
    vkDestroyPipeline(g_vk.device, pipe->pipeline, NULL);
    vkDestroyPipelineLayout(g_vk.device, pipe->layout, NULL);
    vkDestroyDescriptorSetLayout(g_vk.device, pipe->desc_set_layout, NULL);
    vkDestroyShaderModule(g_vk.device, pipe->vert_module, NULL);
    vkDestroyShaderModule(g_vk.device, pipe->frag_module, NULL);
    pipe->pipeline = VK_NULL_HANDLE;
}

void renderer_use_program(R_Program program) {
    CHECK_DEVICE();
    g_vk.active_pipeline = program;
    g_push_dirty = false;
}

int renderer_uniform_location(R_Program program, const char *name) {
    if (program == R_INVALID_HANDLE || !name) return -1;
    for (int i = 0; i < g_uniform_count; i++) {
        if (g_uniforms[i].program == program && strcmp(g_uniforms[i].name, name) == 0)
            return i;
    }
    if (g_uniform_count >= 32) return -1;
    int base   = 192;
    int offset = base + g_uniform_count * 16;
    g_uniforms[g_uniform_count].program = program;
    strncpy(g_uniforms[g_uniform_count].name, name, 63);
    g_uniforms[g_uniform_count].name[63] = '\0';
    g_uniforms[g_uniform_count].offset   = offset;
    return g_uniform_count++;
}

void renderer_uniform_mat4(int loc, const float *m) {
    if (loc < 0 || loc >= g_uniform_count || !m) return;
    int offset = g_uniforms[loc].offset;
    const char *n = g_uniforms[loc].name;
    if (strstr(n, "model"))      offset = 0;
    else if (strstr(n, "view"))  offset = 64;
    else if (strstr(n, "proj"))  offset = 128;
    if (offset + 64 > 256) return;
    memcpy(g_push_constants + offset, m, 64);
    g_push_dirty = true;
}

void renderer_uniform_vec3(int loc, float x, float y, float z) {
    if (loc < 0 || loc >= g_uniform_count) return;
    int offset = g_uniforms[loc].offset;
    if (offset + 16 > 256) return;
    float v[4] = {x, y, z, 0.f};
    memcpy(g_push_constants + offset, v, 12);
    g_push_dirty = true;
}

void renderer_uniform_vec2(int loc, float x, float y) {
    if (loc < 0 || loc >= g_uniform_count) return;
    int offset = g_uniforms[loc].offset;
    if (offset + 8 > 256) return;
    float v[2] = {x, y};
    memcpy(g_push_constants + offset, v, 8);
    g_push_dirty = true;
}

void renderer_uniform_float(int loc, float value) {
    if (loc < 0 || loc >= g_uniform_count) return;
    int offset = g_uniforms[loc].offset;
    if (offset + 4 > 256) return;
    memcpy(g_push_constants + offset, &value, 4);
    g_push_dirty = true;
}

void renderer_uniform_int(int loc, int value) {
    if (loc < 0 || loc >= g_uniform_count) return;
    int offset = g_uniforms[loc].offset;
    if (offset + 4 > 256) return;
    memcpy(g_push_constants + offset, &value, 4);
    g_push_dirty = true;
}

void renderer_uniform_ivec2(int loc, int x, int y) {
    if (loc < 0 || loc >= g_uniform_count) return;
    int offset = g_uniforms[loc].offset;
    if (offset + 8 > 256) return;
    int v[2] = {x, y};
    memcpy(g_push_constants + offset, v, 8);
    g_push_dirty = true;
}
