#ifdef _WIN32
#  define VK_USE_PLATFORM_WIN32_KHR
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  define VK_USE_PLATFORM_XLIB_KHR
#  include <X11/Xlib.h>
#endif
#include <vulkan/vulkan.h>

#include "render.h"
#include "core.h"
#include "frustum.h"
#include "linalg.h"
#include "mesh.h"
#include "font8x8.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES_IN_FLIGHT 2

/* 2D overlay vertex budget per frame: debug text (each lit font pixel costs 6
   verts) plus UI rectangles share this stream. */
#define MAX_TEXT_VERTS (256 * 1024)

/* Resident GPU resources and per-frame draw instances. */
#define MAX_MESHES 256
#define MAX_TEXTURES 256
#define MAX_MATERIALS 256
#define MAX_INSTANCES 4096
#define SHADOW_MAP_SIZE   2048
#define PP_FORMAT         VK_FORMAT_R16G16B16A16_SFLOAT /* HDR offscreen + bloom */
#define MAX_CUBEMAPS      8
#define MAX_INST_TOTAL     16384
#define MAX_INST_BATCHES      64
#define MAX_GPU_EMITTERS       8
#define MAX_EMIT_PER_FRAME   256

#define VK_CHECK(x)                                                         \
    do {                                                                    \
        VkResult _res = (x);                                                \
        if (_res != VK_SUCCESS) {                                           \
            fprintf(stderr, "[render] Vulkan error %d at %s:%d\n", _res,    \
                    __FILE__, __LINE__);                                    \
            return false;                                                   \
        }                                                                   \
    } while (0)

typedef struct {
    float pos[2]; /* screen pixels */
    float color[3];
} text_vertex_t;

/* A GPU-resident indexed mesh. */
typedef struct {
    VkBuffer vertex_buffer;
    VkDeviceMemory vertex_memory;
    VkBuffer index_buffer;
    VkDeviceMemory index_memory;
    uint32_t index_count;
    vec3_t bounds_min; /* local-space AABB for frustum culling */
    vec3_t bounds_max;
} gpu_mesh_t;

/* A sampled 2D image. */
typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
} gpu_texture_t;

/* A material: a small UBO (base colour) plus a descriptor set binding that UBO
   and an albedo texture for the fragment shader. */
typedef struct {
    VkBuffer ubo;
    VkDeviceMemory ubo_memory;
    VkDescriptorSet set;
} gpu_material_t;

/* Layout of the per-material uniform buffer; matches Material in mesh.frag. */
typedef struct {
    float base_color[4];
} material_ubo_t;

/* Per-frame point light entry (3 × vec4 = 48 bytes, 16-byte aligned). */
typedef struct {
    float pos[4];    /* xyz: world-space position, w: unused */
    float color[4];  /* xyz: RGB intensity, w: unused */
    float params[4]; /* x: radius, yzw: unused */
} point_light_ubo_t;

/* Per-frame scene uniform (std140).
   light_dir.w      = active point-light count.
   light_vp[3]      = 3 cascade light view-projection matrices (192 bytes).
   cascade_splits   = xyz: camera-distance split planes (world units).
   point_lights     = up to RENDER_MAX_POINT_LIGHTS × 48 bytes. */
typedef struct {
    float light_dir[4];
    float light_color[4];
    float ambient_color[4];
    float view_pos[4];
    float light_vp[3][16];   /* 3 cascades × mat4 */
    float cascade_splits[4]; /* xyz = near split planes, w = unused */
    point_light_ubo_t point_lights[RENDER_MAX_POINT_LIGHTS];
} scene_ubo_t;

/* Extended GPU vertex: cpu_mesh_t fields plus a tangent computed at upload. */
typedef struct {
    vec3_t position;
    vec3_t normal;
    vec2_t uv;
    vec3_t tangent;
} gpu_vertex_t;

/* One queued single draw. */
typedef struct {
    mesh_handle_t mesh;
    material_handle_t material;
    mat4_t mvp;
    mat4_t model;
} mesh_instance_t;

/* One instanced batch: same mesh+material, N model matrices. */
typedef struct {
    mesh_handle_t    mesh;
    material_handle_t material;
    uint32_t         first; /* index into flat inst_models array */
    uint32_t         count;
} inst_batch_t;

/* Vertex-stage push constants for the mesh pipeline (128 bytes = the minimum
   guaranteed maxPushConstantsSize, so this stays portable). */
typedef struct {
    mat4_t mvp;
    mat4_t model;
} mesh_push_t;

/* Per-particle state stored in device-local SSBO. std430 layout, 48 bytes.
   Must match 'struct Particle' in particles.comp exactly. */
typedef struct {
    float pos[3];
    float life;
    float vel[3];
    float max_life;
    float scale;
    float _pad[3];
} gpu_particle_t;

typedef struct {
    mesh_handle_t     mesh;
    material_handle_t material;
    uint32_t          capacity;
    float             gravity_y;
    bool              active;

    /* Simulation SSBO (device-local; compute reads/writes). */
    VkBuffer      particle_buf;
    VkDeviceMemory particle_mem;

    /* Per-frame: matrix output (STORAGE + VERTEX_BUFFER). */
    VkBuffer      matrix_buf[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory matrix_mem[MAX_FRAMES_IN_FLIGHT];

    /* Per-frame: indirect draw args (HOST_VISIBLE + INDIRECT_BUFFER + STORAGE).
       Kept host-visible so instance_count can be reset to 0 without a transfer. */
    VkBuffer      indirect_buf[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory indirect_mem[MAX_FRAMES_IN_FLIGHT];
    void         *indirect_mapped[MAX_FRAMES_IN_FLIGHT];

    /* Per-frame: staging for CPU-emitted particles (HOST_VISIBLE + STORAGE). */
    VkBuffer      emit_buf[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory emit_mem[MAX_FRAMES_IN_FLIGHT];
    void         *emit_mapped[MAX_FRAMES_IN_FLIGHT];

    /* Per-frame compute descriptor sets (different matrix/indirect/emit per frame). */
    VkDescriptorSet compute_sets[MAX_FRAMES_IN_FLIGHT];

    /* CPU-side emit accumulator (written between render_draw calls). */
    gpu_particle_t emit_pending[MAX_EMIT_PER_FRAME];
    uint32_t       emit_pending_count;
    uint32_t       ring_head; /* next slot to overwrite in particle_buf */

    /* Per-frame snapshot of pending emits (consumed in render_draw). */
    uint32_t       emit_count[MAX_FRAMES_IN_FLIGHT];
    uint32_t       emit_ring_head[MAX_FRAMES_IN_FLIGHT];

    float pending_dt;
    bool  pending;
} gpu_emitter_t;

typedef struct { uint32_t emitter_idx; } gpu_particle_cmd_t;

/* Push constants for particles.comp (32 bytes). */
typedef struct {
    float    dt;
    float    gravity_y;
    uint32_t capacity;
    uint32_t emit_count;
    uint32_t ring_head;
    uint32_t mode;     /* 0 = inject, 1 = simulate */
    uint32_t _pad[2];
} particle_push_t;

/* ---- GPU-driven instanced culling + indirect ---- */

/* Per-instance input to cull.comp: model matrix + which batch this belongs to. */
typedef struct {
    mat4_t   model;
    uint32_t batch_idx;
    uint32_t _pad[3];
} cull_inst_t; /* 80 bytes, std430 aligned */

/* Per-batch input to cull.comp: mesh AABB + output region info. */
typedef struct {
    float    bounds_min[3];
    uint32_t first;    /* base slot in the output matrix buffer */
    float    bounds_max[3];
    uint32_t count;    /* max instances (caps the atomic) */
} cull_batch_t; /* 32 bytes */

/* Push constants for cull.comp (112 bytes). */
typedef struct {
    float    planes[6][4]; /* 6 frustum planes as vec4(a,b,c,d) */
    uint32_t total_instances;
    uint32_t batch_count;
    uint32_t _pad[2];
} cull_push_t;

static struct {
    window_t *window;

    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical_device;
    VkDevice device;
    uint32_t graphics_family;
    uint32_t present_family;
    VkQueue graphics_queue;
    VkQueue present_queue;

    VkSwapchainKHR swapchain;
    VkFormat swapchain_format;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage *images;
    VkImageView *image_views;

    VkFormat depth_format;
    VkImage depth_image;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;

    /* Shadow map: depth image rendered from the light's perspective. */
    VkImage          shadow_image;
    VkDeviceMemory   shadow_memory;
    VkImageView      shadow_view;            /* array view: all 3 layers, for sampling */
    VkImageView      shadow_layer_view[3];   /* single-layer views: for rendering */
    VkSampler        shadow_sampler; /* comparison sampler (LESS_OR_EQUAL) */
    VkPipelineLayout shadow_layout;
    VkPipeline       shadow_pipeline;

    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkPipeline wireframe_pipeline; /* VK_POLYGON_MODE_LINE variant */
    bool wireframe;
    bool wireframe_supported;
    bool aniso_supported;
    float max_anisotropy;
    bool blit_mip_supported; /* VK_FORMAT_R8G8B8A8_SRGB supports linear blit */

    VkCommandPool command_pool;
    VkCommandBuffer command_buffers[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore image_available[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore *render_finished; /* one per swapchain image */
    VkFence in_flight[MAX_FRAMES_IN_FLIGHT];
    uint32_t frame;

    gpu_mesh_t meshes[MAX_MESHES];
    uint32_t mesh_count;

    gpu_texture_t textures[MAX_TEXTURES];
    uint32_t texture_count;
    texture_handle_t default_texture;        /* 1x1 white, for untextured materials */
    texture_handle_t default_normal_texture; /* 1x1 flat (128,128,255) */
    VkSampler sampler;

    VkDescriptorSetLayout material_set_layout;
    VkDescriptorPool descriptor_pool;
    gpu_material_t materials[MAX_MATERIALS];
    uint32_t material_count;

    scene_ubo_t scene_data;
    VkDescriptorSetLayout scene_set_layout;
    VkDescriptorPool scene_pool;
    VkBuffer scene_ubo_buf[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory scene_ubo_mem[MAX_FRAMES_IN_FLIGHT];
    void *scene_ubo_mapped[MAX_FRAMES_IN_FLIGHT];
    VkDescriptorSet scene_sets[MAX_FRAMES_IN_FLIGHT];

    /* Mesh draws queued this frame; drained in record_command_buffer. */
    mat4_t view;
    mat4_t proj;
    frustum_t frustum; /* extracted each frame in render_set_camera */
    mesh_instance_t *instances; /* capacity MAX_INSTANCES */
    uint32_t instance_count;

    /* Point lights queued this frame; cleared each frame like instances. */
    uint32_t point_light_count;

    /* Instanced draw batches + flat model-matrix array (CPU side). */
    mat4_t      *inst_models;    /* capacity MAX_INST_TOTAL */
    uint32_t     inst_model_count;
    inst_batch_t inst_batches[MAX_INST_BATCHES];
    uint32_t     inst_batch_count;

    /* Per-frame GPU buffer for instance model matrices (persistently mapped). */
    VkBuffer     inst_vbuf[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory inst_vmem[MAX_FRAMES_IN_FLIGHT];
    void        *inst_vmapped[MAX_FRAMES_IN_FLIGHT];

    /* Cubemaps */
    struct {
        VkImage        image;
        VkDeviceMemory memory;
        VkImageView    view;
    } cubemaps[MAX_CUBEMAPS];
    uint32_t cubemap_count;

    /* Skybox */
    cubemap_handle_t active_skybox;   /* RENDER_CUBEMAP_INVALID = disabled */
    VkSampler        skybox_sampler;
    VkDescriptorSetLayout skybox_set_layout;
    VkDescriptorPool  skybox_pool;
    VkPipelineLayout  skybox_layout;
    VkPipeline        skybox_pipeline;
    VkDescriptorSet   skybox_sets[MAX_CUBEMAPS]; /* one per cubemap */

    /* Instanced colour pipelines (push = mat4 proj_view, 64 bytes). */
    VkPipelineLayout inst_layout;
    VkPipeline       inst_pipeline;
    VkPipeline       inst_wireframe_pipeline;
    /* Instanced shadow pipeline reuses g.shadow_layout (same push). */
    VkPipeline       shadow_inst_pipeline;

    /* Debug text: a screen-space 2D pipeline with per-frame-in-flight vertex
       buffers (persistently mapped) fed from a CPU staging array. */
    VkPipelineLayout text_layout;
    VkPipeline text_pipeline;
    VkBuffer text_buffer[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory text_memory[MAX_FRAMES_IN_FLIGHT];
    void *text_mapped[MAX_FRAMES_IN_FLIGHT];
    text_vertex_t *text_verts; /* CPU staging, capacity MAX_TEXT_VERTS */
    uint32_t text_vert_count;

    float clear_color[3];
    char shader_dir[1024]; /* resolved at init; .spv files live here */

    VkPresentModeKHR present_mode;
    bool vsync_dirty;

    /* ------------------------------------------------------------------ */
    /* Bloom post-processing                                                 */
    /* ------------------------------------------------------------------ */

    /* HDR offscreen color target (scene renders here, not directly to swapchain). */
    VkImage        color_image;
    VkDeviceMemory color_memory;
    VkImageView    color_view;

    /* Bloom ping-pong images at half resolution. */
    VkImage        bloom_a_image, bloom_b_image;
    VkDeviceMemory bloom_a_memory, bloom_b_memory;
    VkImageView    bloom_a_view,   bloom_b_view;
    VkExtent2D     bloom_extent;

    /* Post-process infrastructure (created once, not swapchain-dependent). */
    VkSampler             pp_sampler;
    VkDescriptorSetLayout pp_set_layout;
    VkDescriptorPool      pp_pool;
    VkPipelineLayout      pp_layout;
    VkPipeline            pp_threshold_pipeline;
    VkPipeline            pp_blur_pipeline;
    VkPipeline            pp_composite_pipeline;

    /* Post-process descriptor sets (recreated with bloom images). */
    VkDescriptorSet pp_threshold_set; /* sources: color_image, dummy */
    VkDescriptorSet pp_blur_a_set;    /* sources: bloom_a,     dummy */
    VkDescriptorSet pp_blur_b_set;    /* sources: bloom_b,     dummy */
    VkDescriptorSet pp_composite_set; /* sources: color_image, bloom_a */

    bool  bloom_enabled;
    float bloom_threshold; /* luminance cutoff, default 0.8 */
    float bloom_strength;  /* bloom additive weight, default 0.5 */
    float bloom_exposure;  /* Reinhard exposure,   default 1.0 */

    /* Screenshot: if path is non-empty, save the next frame's color_image. */
    char screenshot_path[512];

    /* GPU-driven instanced culling + indirect draw. */
    VkBuffer       cull_inst_buf[MAX_FRAMES_IN_FLIGHT];    /* input: instances (HOST_VISIBLE) */
    VkDeviceMemory cull_inst_mem[MAX_FRAMES_IN_FLIGHT];
    void          *cull_inst_mapped[MAX_FRAMES_IN_FLIGHT];

    VkBuffer       cull_batch_buf[MAX_FRAMES_IN_FLIGHT];   /* input: batch AABBs (HOST_VISIBLE) */
    VkDeviceMemory cull_batch_mem[MAX_FRAMES_IN_FLIGHT];
    void          *cull_batch_mapped[MAX_FRAMES_IN_FLIGHT];

    VkBuffer       cull_out_buf[MAX_FRAMES_IN_FLIGHT];     /* output: compact matrices (DEVICE_LOCAL) */
    VkDeviceMemory cull_out_mem[MAX_FRAMES_IN_FLIGHT];

    VkBuffer       cull_indirect_buf[MAX_FRAMES_IN_FLIGHT];/* output: draw args (HOST_VISIBLE+INDIRECT) */
    VkDeviceMemory cull_indirect_mem[MAX_FRAMES_IN_FLIGHT];
    void          *cull_indirect_mapped[MAX_FRAMES_IN_FLIGHT];

    VkDescriptorSetLayout cull_set_layout;
    VkDescriptorPool      cull_pool;
    VkPipelineLayout      cull_layout;
    VkPipeline            cull_pipeline;
    VkDescriptorSet       cull_sets[MAX_FRAMES_IN_FLIGHT];

    /* GPU particle emitters. */
    gpu_emitter_t      gpu_emitters[MAX_GPU_EMITTERS];
    uint32_t           gpu_emitter_count;
    VkDescriptorSetLayout particle_set_layout;
    VkDescriptorPool   particle_pool;
    VkPipelineLayout   particle_layout;
    VkPipeline         particle_pipeline;
    gpu_particle_cmd_t particle_cmds[MAX_GPU_EMITTERS];
    uint32_t           particle_cmd_count;
} g;

/* ----------------------------------------------------------------------- */
/* helpers                                                                  */
/* ----------------------------------------------------------------------- */

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)type;
    (void)user;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        fprintf(stderr, "[validation] %s\n", data->pMessage);
    }
    return VK_FALSE;
}

static bool find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props,
                             uint32_t *out) {
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(g.physical_device, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((type_filter & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & props) == props) {
            *out = i;
            return true;
        }
    }
    return false;
}

static bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags props, VkBuffer *buffer,
                          VkDeviceMemory *memory) {
    VkBufferCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(g.device, &info, NULL, buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.device, *buffer, &req);

    uint32_t type_index;
    if (!find_memory_type(req.memoryTypeBits, props, &type_index)) {
        fprintf(stderr, "[render] no suitable memory type for buffer\n");
        return false;
    }

    VkMemoryAllocateInfo alloc = {0};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type_index;
    VK_CHECK(vkAllocateMemory(g.device, &alloc, NULL, memory));
    VK_CHECK(vkBindBufferMemory(g.device, *buffer, *memory, 0));
    return true;
}

/* Upload host data into a freshly created HOST_VISIBLE|COHERENT buffer. */
static bool create_filled_buffer(const void *data, VkDeviceSize size,
                                 VkBufferUsageFlags usage, VkBuffer *buffer,
                                 VkDeviceMemory *memory) {
    if (!create_buffer(size, usage,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       buffer, memory)) {
        return false;
    }
    void *mapped;
    VK_CHECK(vkMapMemory(g.device, *memory, 0, size, 0, &mapped));
    memcpy(mapped, data, (size_t)size);
    vkUnmapMemory(g.device, *memory);
    return true;
}

/* Allocate and begin a throwaway command buffer for an init-time transfer.
   Pair with end_single_time, which submits and blocks until it completes. */
static VkCommandBuffer begin_single_time(void) {
    VkCommandBufferAllocateInfo alloc = {0};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = g.command_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(g.device, &alloc, &cmd) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    return cmd;
}

static void end_single_time(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(g.graphics_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.graphics_queue);
    vkFreeCommandBuffers(g.device, g.command_pool, 1, &cmd);
}

static bool read_file(const char *path, uint32_t **out_data, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[render] cannot open shader '%s'\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fprintf(stderr, "[render] empty shader '%s'\n", path);
        fclose(f);
        return false;
    }
    uint32_t *buf = malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return false;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "[render] short read on '%s'\n", path);
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);
    *out_data = buf;
    *out_size = (size_t)len;
    return true;
}

static bool create_shader_module(const char *name, VkShaderModule *out) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s.spv", g.shader_dir, name);

    uint32_t *code;
    size_t size;
    if (!read_file(path, &code, &size)) {
        return false;
    }

    VkShaderModuleCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode = code;
    VkResult res = vkCreateShaderModule(g.device, &info, NULL, out);
    free(code);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[render] vkCreateShaderModule(%s) failed: %d\n", name,
                res);
        return false;
    }
    return true;
}

/* ----------------------------------------------------------------------- */
/* instance / device                                                        */
/* ----------------------------------------------------------------------- */

static bool layer_available(const char *name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, NULL);
    VkLayerProperties *layers = malloc(sizeof(*layers) * count);
    vkEnumerateInstanceLayerProperties(&count, layers);
    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(layers[i].layerName, name) == 0) {
            found = true;
            break;
        }
    }
    free(layers);
    return found;
}

static bool create_instance(void) {
    bool validation = layer_available("VK_LAYER_KHRONOS_validation");
    if (!validation) {
        fprintf(stderr, "[render] validation layer unavailable; running "
                        "without it\n");
    } else {
        fprintf(stderr, "[render] validation layer enabled\n");
    }

    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Kiln";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "Kiln";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    const char *extensions[3];
    uint32_t ext_count = 0;
    extensions[ext_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
#ifdef _WIN32
    extensions[ext_count++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#else
    extensions[ext_count++] = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
#endif
    if (validation) {
        extensions[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }

    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};

    VkInstanceCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = ext_count;
    info.ppEnabledExtensionNames = extensions;
    if (validation) {
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = layers;
    }
    VK_CHECK(vkCreateInstance(&info, NULL, &g.instance));

    if (validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create_fn =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                g.instance, "vkCreateDebugUtilsMessengerEXT");
        if (create_fn) {
            VkDebugUtilsMessengerCreateInfoEXT dbg = {0};
            dbg.sType =
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dbg.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dbg.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dbg.pfnUserCallback = debug_callback;
            create_fn(g.instance, &dbg, NULL, &g.debug_messenger);
        }
    }
    return true;
}

static bool create_surface(void) {
    platform_native_handles_t nh = window_get_native_handles(g.window);
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR info = {0};
    info.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = (HINSTANCE)nh.display;
    info.hwnd      = (HWND)(uintptr_t)nh.window;
    VK_CHECK(vkCreateWin32SurfaceKHR(g.instance, &info, NULL, &g.surface));
#else
    VkXlibSurfaceCreateInfoKHR info = {0};
    info.sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    info.dpy    = (Display *)nh.display;
    info.window = (Window)nh.window;
    VK_CHECK(vkCreateXlibSurfaceKHR(g.instance, &info, NULL, &g.surface));
#endif
    return true;
}

static bool pick_physical_device(void) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g.instance, &count, NULL);
    if (count == 0) {
        fprintf(stderr, "[render] no Vulkan devices\n");
        return false;
    }
    VkPhysicalDevice *devices = malloc(sizeof(*devices) * count);
    vkEnumeratePhysicalDevices(g.instance, &count, devices);

    g.physical_device = devices[0];
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            g.physical_device = devices[i];
            break;
        }
    }
    free(devices);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g.physical_device, &props);
    fprintf(stderr, "[render] GPU: %s\n", props.deviceName);

    /* find queue families */
    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical_device, &qcount, NULL);
    VkQueueFamilyProperties *queues = malloc(sizeof(*queues) * qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical_device, &qcount,
                                             queues);
    g.graphics_family = UINT32_MAX;
    g.present_family = UINT32_MAX;
    for (uint32_t i = 0; i < qcount; i++) {
        if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT &&
            g.graphics_family == UINT32_MAX) {
            g.graphics_family = i;
        }
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g.physical_device, i, g.surface,
                                             &present);
        if (present && g.present_family == UINT32_MAX) {
            g.present_family = i;
        }
    }
    free(queues);
    if (g.graphics_family == UINT32_MAX || g.present_family == UINT32_MAX) {
        fprintf(stderr, "[render] missing graphics/present queue\n");
        return false;
    }
    return true;
}

static bool create_device(void) {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_infos[2];
    uint32_t queue_count = 0;

    uint32_t families[2] = {g.graphics_family, g.present_family};
    uint32_t unique = (g.graphics_family == g.present_family) ? 1 : 2;
    for (uint32_t i = 0; i < unique; i++) {
        VkDeviceQueueCreateInfo qi = {0};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = families[i];
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        queue_infos[queue_count++] = qi;
    }

    const char *device_ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkPhysicalDeviceFeatures supported = {0};
    vkGetPhysicalDeviceFeatures(g.physical_device, &supported);
    g.wireframe_supported = (supported.fillModeNonSolid == VK_TRUE);
    g.aniso_supported     = (supported.samplerAnisotropy == VK_TRUE);

    VkPhysicalDeviceProperties phys_props;
    vkGetPhysicalDeviceProperties(g.physical_device, &phys_props);
    g.max_anisotropy = phys_props.limits.maxSamplerAnisotropy;

    VkPhysicalDeviceVulkan13Features features13 = {0};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;

    /* Use PhysicalDeviceFeatures2 so we can enable base features alongside
       the Vulkan 1.3 chain; pEnabledFeatures must be NULL in this case. */
    VkPhysicalDeviceFeatures2 features2 = {0};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features13;
    features2.features.fillModeNonSolid  = supported.fillModeNonSolid;
    features2.features.samplerAnisotropy = supported.samplerAnisotropy;

    VkDeviceCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &features2;
    info.queueCreateInfoCount = queue_count;
    info.pQueueCreateInfos = queue_infos;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = device_ext;
    VK_CHECK(vkCreateDevice(g.physical_device, &info, NULL, &g.device));

    vkGetDeviceQueue(g.device, g.graphics_family, 0, &g.graphics_queue);
    vkGetDeviceQueue(g.device, g.present_family, 0, &g.present_queue);
    return true;
}

/* ----------------------------------------------------------------------- */
/* swapchain + depth                                                        */
/* ----------------------------------------------------------------------- */

static VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR *caps) {
    /* Use platform window size directly. Do NOT clamp to caps min/maxImageExtent:
       on Wine those equal currentExtent, which is stale after an external WM
       resize, so clamping would silently lock the swapchain at the old size. */
    (void)caps;
    uint32_t w, h;
    window_size(g.window, &w, &h);
    return (VkExtent2D){w, h};
}

static VkPresentModeKHR choose_present_mode(bool vsync) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.physical_device, g.surface,
                                             &count, NULL);
    VkPresentModeKHR *modes = malloc(sizeof(*modes) * count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.physical_device, g.surface,
                                             &count, modes);
    VkPresentModeKHR result = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync) {
        for (uint32_t i = 0; i < count; i++) {
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
                result = VK_PRESENT_MODE_MAILBOX_KHR;
                break;
            }
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
                result = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
    }
    free(modes);
    return result;
}

static bool create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physical_device, g.surface,
                                              &caps);
    g.extent = choose_extent(&caps);
#ifdef _WIN32
    {
        uint32_t ww, wh;
        window_size(g.window, &ww, &wh);
        fprintf(stderr, "[kiln] create_swapchain: window_size=%ux%u  "
                "caps.currentExtent=%ux%u  caps.min=%ux%u  caps.max=%ux%u  "
                "using=%ux%u\n",
                ww, wh,
                caps.currentExtent.width, caps.currentExtent.height,
                caps.minImageExtent.width, caps.minImageExtent.height,
                caps.maxImageExtent.width, caps.maxImageExtent.height,
                g.extent.width, g.extent.height);
    }
#endif
    if (g.extent.width == 0 || g.extent.height == 0) {
        return true; /* minimized; defer */
    }

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical_device, g.surface,
                                         &format_count, NULL);
    VkSurfaceFormatKHR *formats = malloc(sizeof(*formats) * format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical_device, g.surface,
                                         &format_count, formats);
    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < format_count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = formats[i];
            break;
        }
    }
    free(formats);
    g.swapchain_format = chosen.format;

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && want > caps.maxImageCount) {
        want = caps.maxImageCount;
    }

    /* Select compositeAlpha from what the surface actually supports. */
    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        const VkCompositeAlphaFlagBitsKHR fallbacks[] = {
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        for (size_t i = 0; i < 3; i++) {
            if (caps.supportedCompositeAlpha & fallbacks[i]) {
                composite_alpha = fallbacks[i];
                fprintf(stderr, "[kiln] OPAQUE compositeAlpha not supported "
                        "(supported=0x%x); using 0x%x\n",
                        caps.supportedCompositeAlpha, (unsigned)composite_alpha);
                break;
            }
        }
    }

    VkSwapchainCreateInfoKHR info = {0};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = g.surface;
    info.minImageCount = want;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = g.extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = composite_alpha;
    info.presentMode = g.present_mode;
    info.clipped = VK_TRUE;

    uint32_t indices[] = {g.graphics_family, g.present_family};
    if (g.graphics_family != g.present_family) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = indices;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    VK_CHECK(vkCreateSwapchainKHR(g.device, &info, NULL, &g.swapchain));

    vkGetSwapchainImagesKHR(g.device, g.swapchain, &g.image_count, NULL);
    g.images = malloc(sizeof(VkImage) * g.image_count);
    vkGetSwapchainImagesKHR(g.device, g.swapchain, &g.image_count, g.images);

    g.image_views = malloc(sizeof(VkImageView) * g.image_count);
    for (uint32_t i = 0; i < g.image_count; i++) {
        VkImageViewCreateInfo view = {0};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = g.images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = g.swapchain_format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(g.device, &view, NULL, &g.image_views[i]));
    }
    return true;
}

static bool create_depth_resources(void) {
    g.depth_format = VK_FORMAT_D32_SFLOAT;

    VkImageCreateInfo image = {0};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = g.depth_format;
    image.extent.width = g.extent.width;
    image.extent.height = g.extent.height;
    image.extent.depth = 1;
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(g.device, &image, NULL, &g.depth_image));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.device, g.depth_image, &req);
    uint32_t type_index;
    if (!find_memory_type(req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index)) {
        return false;
    }
    VkMemoryAllocateInfo alloc = {0};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type_index;
    VK_CHECK(vkAllocateMemory(g.device, &alloc, NULL, &g.depth_memory));
    VK_CHECK(vkBindImageMemory(g.device, g.depth_image, g.depth_memory, 0));

    VkImageViewCreateInfo view = {0};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = g.depth_image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = g.depth_format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(g.device, &view, NULL, &g.depth_view));
    return true;
}

/* ----------------------------------------------------------------------- */
/* HDR offscreen color target + bloom ping-pong images                      */
/* ----------------------------------------------------------------------- */

static bool make_color_image(VkFormat fmt, VkExtent2D ext, VkImageUsageFlags usage,
                             VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ci = {0};
    ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = fmt;
    ci.extent        = (VkExtent3D){ext.width, ext.height, 1};
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ci.usage         = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(g.device, &ci, NULL, img));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.device, *img, &req);
    uint32_t mtype;
    if (!find_memory_type(req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mtype))
        return false;
    VkMemoryAllocateInfo alloc = {0};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = mtype;
    VK_CHECK(vkAllocateMemory(g.device, &alloc, NULL, mem));
    VK_CHECK(vkBindImageMemory(g.device, *img, *mem, 0));

    VkImageViewCreateInfo vci = {0};
    vci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image                           = *img;
    vci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    vci.format                          = fmt;
    vci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount     = 1;
    vci.subresourceRange.layerCount     = 1;
    VK_CHECK(vkCreateImageView(g.device, &vci, NULL, view));
    return true;
}

static bool create_render_targets(void) {
    VkImageUsageFlags col  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImageUsageFlags blm  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT;

    if (!make_color_image(PP_FORMAT, g.extent, col,
                          &g.color_image, &g.color_memory, &g.color_view))
        return false;

    g.bloom_extent.width  = (g.extent.width  > 1) ? g.extent.width  / 2 : 1;
    g.bloom_extent.height = (g.extent.height > 1) ? g.extent.height / 2 : 1;

    return make_color_image(PP_FORMAT, g.bloom_extent, blm,
                            &g.bloom_a_image, &g.bloom_a_memory, &g.bloom_a_view) &&
           make_color_image(PP_FORMAT, g.bloom_extent, blm,
                            &g.bloom_b_image, &g.bloom_b_memory, &g.bloom_b_view);
}

static void destroy_render_targets(void) {
#define DESTROY_IMG(img, mem, view) do { \
    if (view) { vkDestroyImageView(g.device, (view), NULL); \
                vkDestroyImage    (g.device, (img),  NULL); \
                vkFreeMemory      (g.device, (mem),  NULL); \
                (view) = VK_NULL_HANDLE; } } while (0)
    DESTROY_IMG(g.color_image,   g.color_memory,   g.color_view);
    DESTROY_IMG(g.bloom_a_image, g.bloom_a_memory, g.bloom_a_view);
    DESTROY_IMG(g.bloom_b_image, g.bloom_b_memory, g.bloom_b_view);
#undef DESTROY_IMG
}

/* ----------------------------------------------------------------------- */
/* Post-process infrastructure (sampler, layout, pool, pipelines)           */
/* ----------------------------------------------------------------------- */

/* Build a fullscreen-triangle pipeline with the given fragment shader.
   target_fmt is the color attachment format (PP_FORMAT or swapchain). */
static bool build_pp_pipeline(const char *frag, VkFormat target_fmt,
                              VkPipeline *out) {
    VkShaderModule vmod, fmod;
    if (!create_shader_module("fullscreen.vert", &vmod) ||
        !create_shader_module(frag, &fmod))
        return false;

    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod; stages[0].pName = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = {0};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo ras = {0};
    ras.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    ras.polygonMode = VK_POLYGON_MODE_FILL; ras.cullMode = VK_CULL_MODE_NONE;
    ras.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState att = {0};
    att.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend = {0};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1; blend.pAttachments = &att;
    VkDynamicState dyn_st[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {0};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyn_st;
    VkPipelineRenderingCreateInfo rci = {0};
    rci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rci.colorAttachmentCount = 1; rci.pColorAttachmentFormats = &target_fmt;

    VkGraphicsPipelineCreateInfo info = {0};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &rci;
    info.stageCount          = 2; info.pStages = stages;
    info.pVertexInputState   = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState      = &vps;
    info.pRasterizationState = &ras;
    info.pMultisampleState   = &ms;
    info.pDepthStencilState  = &ds;
    info.pColorBlendState    = &blend;
    info.pDynamicState       = &dyn;
    info.layout              = g.pp_layout;

    VkResult res = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE,
                                             1, &info, NULL, out);
    vkDestroyShaderModule(g.device, vmod, NULL);
    vkDestroyShaderModule(g.device, fmod, NULL);
    return res == VK_SUCCESS;
}

static bool create_pp_infra(void) {
    VkSamplerCreateInfo sci = {0};
    sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(g.device, &sci, NULL, &g.pp_sampler));

    VkDescriptorSetLayoutBinding bindings[2] = {0};
    bindings[0].binding        = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags     = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1]                = bindings[0]; bindings[1].binding = 1;
    VkDescriptorSetLayoutCreateInfo dlci = {0};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 2; dlci.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(g.device, &dlci, NULL, &g.pp_set_layout));

    VkDescriptorPoolSize pool_sz = {0};
    pool_sz.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sz.descriptorCount = 8;
    VkDescriptorPoolCreateInfo poolci = {0};
    poolci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolci.maxSets = 4; poolci.poolSizeCount = 1; poolci.pPoolSizes = &pool_sz;
    VK_CHECK(vkCreateDescriptorPool(g.device, &poolci, NULL, &g.pp_pool));

    VkPushConstantRange push = {0};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; push.size = 16;
    VkPipelineLayoutCreateInfo plci = {0};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &g.pp_set_layout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(g.device, &plci, NULL, &g.pp_layout));

    return build_pp_pipeline("bloom_threshold.frag", PP_FORMAT,
                             &g.pp_threshold_pipeline) &&
           build_pp_pipeline("bloom_blur.frag", PP_FORMAT,
                             &g.pp_blur_pipeline) &&
           build_pp_pipeline("bloom_composite.frag", g.swapchain_format,
                             &g.pp_composite_pipeline);
}

static bool create_pp_descriptors(void) {
    VkDescriptorSetLayout layouts[4];
    for (int i = 0; i < 4; i++) layouts[i] = g.pp_set_layout;
    VkDescriptorSetAllocateInfo alloc = {0};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = g.pp_pool;
    alloc.descriptorSetCount = 4;
    alloc.pSetLayouts        = layouts;
    VkDescriptorSet sets[4];
    VK_CHECK(vkAllocateDescriptorSets(g.device, &alloc, sets));
    g.pp_threshold_set = sets[0];
    g.pp_blur_a_set    = sets[1];
    g.pp_blur_b_set    = sets[2];
    g.pp_composite_set = sets[3];

#define MKII(samp, view) (VkDescriptorImageInfo){ \
    (samp), (view), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }
    VkDescriptorImageInfo white   = MKII(g.pp_sampler, g.textures[g.default_texture].view);
    VkDescriptorImageInfo color_i = MKII(g.pp_sampler, g.color_view);
    VkDescriptorImageInfo ba      = MKII(g.pp_sampler, g.bloom_a_view);
    VkDescriptorImageInfo bb      = MKII(g.pp_sampler, g.bloom_b_view);
#undef MKII

#define WIMG(dset, bind, iinfo) do { \
    VkWriteDescriptorSet _w = {0}; \
    _w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; \
    _w.dstSet = (dset); _w.dstBinding = (bind); \
    _w.descriptorCount = 1; \
    _w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; \
    _w.pImageInfo = &(iinfo); \
    vkUpdateDescriptorSets(g.device, 1, &_w, 0, NULL); } while(0)

    WIMG(g.pp_threshold_set, 0, color_i);
    WIMG(g.pp_threshold_set, 1, white);
    WIMG(g.pp_blur_a_set,    0, ba);
    WIMG(g.pp_blur_a_set,    1, white);
    WIMG(g.pp_blur_b_set,    0, bb);
    WIMG(g.pp_blur_b_set,    1, white);
    WIMG(g.pp_composite_set, 0, color_i);
    WIMG(g.pp_composite_set, 1, ba);
#undef WIMG
    return true;
}

static void destroy_pp_descriptors(void) {
    if (g.pp_pool) vkResetDescriptorPool(g.device, g.pp_pool, 0);
}

/* ----------------------------------------------------------------------- */
/* materials: descriptor layout, pool, sampler                              */
/* ----------------------------------------------------------------------- */

/* Each material owns a descriptor set: binding 0 = UBO (base colour),
   binding 1 = albedo sampler, binding 2 = normal map sampler. */
static bool create_material_infra(void) {
    VkDescriptorSetLayoutBinding bindings[3] = {0};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout = {0};
    layout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = 3;
    layout.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(g.device, &layout, NULL,
                                         &g.material_set_layout));

    VkDescriptorPoolSize sizes[2] = {0};
    sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = MAX_MATERIALS;
    sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = MAX_MATERIALS * 2; /* albedo + normal per material */

    VkDescriptorPoolCreateInfo pool = {0};
    pool.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets      = MAX_MATERIALS;
    pool.poolSizeCount = 2;
    pool.pPoolSizes   = sizes;
    VK_CHECK(vkCreateDescriptorPool(g.device, &pool, NULL, &g.descriptor_pool));

    VkFormatProperties fmt_props;
    vkGetPhysicalDeviceFormatProperties(g.physical_device,
                                        VK_FORMAT_R8G8B8A8_SRGB, &fmt_props);
    g.blit_mip_supported =
        (fmt_props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
        (fmt_props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) &&
        (fmt_props.optimalTilingFeatures &
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);

    VkSamplerCreateInfo sampler = {0};
    sampler.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter        = VK_FILTER_LINEAR;
    sampler.minFilter        = VK_FILTER_LINEAR;
    sampler.addressModeU     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.anisotropyEnable = g.aniso_supported ? VK_TRUE : VK_FALSE;
    sampler.maxAnisotropy    = g.max_anisotropy;
    sampler.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.minLod           = 0.0f;
    sampler.maxLod           = 1000.0f; /* VK_LOD_CLAMP_NONE: use all mip levels */
    VK_CHECK(vkCreateSampler(g.device, &sampler, NULL, &g.sampler));
    return true;
}

/* ----------------------------------------------------------------------- */
/* scene UBO: per-frame light direction, colour, ambient                    */
/* ----------------------------------------------------------------------- */

static bool create_scene_infra(void) {
    g.scene_data = (scene_ubo_t){
        .light_dir     = {0.397f, 0.847f, 0.348f, 0.0f},
        .light_color   = {1.0f,   1.0f,   1.0f,   0.0f},
        .ambient_color = {0.16f,  0.18f,  0.24f,  0.0f},
    };

    /* --- cascade shadow map: 3-layer depth array --- */
    VkImageCreateInfo shadow_img = {0};
    shadow_img.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    shadow_img.imageType     = VK_IMAGE_TYPE_2D;
    shadow_img.format        = VK_FORMAT_D32_SFLOAT;
    shadow_img.extent        = (VkExtent3D){SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 1};
    shadow_img.mipLevels     = 1;
    shadow_img.arrayLayers   = 3; /* one per cascade */
    shadow_img.samples       = VK_SAMPLE_COUNT_1_BIT;
    shadow_img.tiling        = VK_IMAGE_TILING_OPTIMAL;
    shadow_img.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT;
    shadow_img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(g.device, &shadow_img, NULL, &g.shadow_image));

    VkMemoryRequirements sreq;
    vkGetImageMemoryRequirements(g.device, g.shadow_image, &sreq);
    uint32_t stype;
    if (!find_memory_type(sreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          &stype))
        return false;
    VkMemoryAllocateInfo salloc = {0};
    salloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    salloc.allocationSize  = sreq.size;
    salloc.memoryTypeIndex = stype;
    VK_CHECK(vkAllocateMemory(g.device, &salloc, NULL, &g.shadow_memory));
    VK_CHECK(vkBindImageMemory(g.device, g.shadow_image, g.shadow_memory, 0));

    /* Sampling view: all 3 layers as a 2D array (for sampler2DArrayShadow). */
    VkImageViewCreateInfo svci = {0};
    svci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    svci.image                           = g.shadow_image;
    svci.viewType                        = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    svci.format                          = VK_FORMAT_D32_SFLOAT;
    svci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
    svci.subresourceRange.levelCount     = 1;
    svci.subresourceRange.layerCount     = 3;
    VK_CHECK(vkCreateImageView(g.device, &svci, NULL, &g.shadow_view));

    /* Per-layer render views (single layer each, for vkCmdBeginRendering). */
    for (uint32_t i = 0; i < 3; i++) {
        VkImageViewCreateInfo lci = svci;
        lci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        lci.subresourceRange.baseArrayLayer = i;
        lci.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(g.device, &lci, NULL,
                                   &g.shadow_layer_view[i]));
    }

    /* --- comparison sampler for sampler2DShadow --- */
    VkSamplerCreateInfo samp = {0};
    samp.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp.magFilter        = VK_FILTER_LINEAR;
    samp.minFilter        = VK_FILTER_LINEAR;
    samp.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samp.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samp.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samp.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; /* lit outside frustum */
    samp.compareEnable    = VK_TRUE;
    samp.compareOp        = VK_COMPARE_OP_LESS_OR_EQUAL;
    samp.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VK_CHECK(vkCreateSampler(g.device, &samp, NULL, &g.shadow_sampler));

    /* --- scene descriptor set layout: binding 0 = UBO, binding 1 = shadow --- */
    VkDescriptorSetLayoutBinding bindings[2] = {0};
    bindings[0].binding        = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags     = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding        = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags     = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci = {0};
    layout_ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 2;
    layout_ci.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(g.device, &layout_ci, NULL,
                                         &g.scene_set_layout));

    VkDescriptorPoolSize pool_sizes[2] = {0};
    pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT;
    pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo pool_ci = {0};
    pool_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets       = MAX_FRAMES_IN_FLIGHT;
    pool_ci.poolSizeCount = 2;
    pool_ci.pPoolSizes    = pool_sizes;
    VK_CHECK(vkCreateDescriptorPool(g.device, &pool_ci, NULL, &g.scene_pool));

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (!create_buffer(sizeof(scene_ubo_t), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &g.scene_ubo_buf[i], &g.scene_ubo_mem[i])) {
            return false;
        }
        VK_CHECK(vkMapMemory(g.device, g.scene_ubo_mem[i], 0,
                             sizeof(scene_ubo_t), 0, &g.scene_ubo_mapped[i]));

        VkDescriptorSetAllocateInfo alloc = {0};
        alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool     = g.scene_pool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &g.scene_set_layout;
        VK_CHECK(vkAllocateDescriptorSets(g.device, &alloc, &g.scene_sets[i]));

        VkDescriptorBufferInfo buf_info = {0};
        buf_info.buffer = g.scene_ubo_buf[i];
        buf_info.range  = sizeof(scene_ubo_t);

        VkDescriptorImageInfo img_info = {0};
        img_info.sampler     = g.shadow_sampler;
        img_info.imageView   = g.shadow_view;
        img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2] = {0};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = g.scene_sets[i];
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo     = &buf_info;
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = g.scene_sets[i];
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &img_info;
        vkUpdateDescriptorSets(g.device, 2, writes, 0, NULL);
    }
    return true;
}

/* ----------------------------------------------------------------------- */
/* pipeline                                                                  */
/* ----------------------------------------------------------------------- */

static bool create_skybox_infra(void) {
    g.active_skybox = RENDER_CUBEMAP_INVALID;

    VkSamplerCreateInfo sci = {0};
    sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod       = 1000.0f;
    VK_CHECK(vkCreateSampler(g.device, &sci, NULL, &g.skybox_sampler));

    VkDescriptorSetLayoutBinding bind = {0};
    bind.binding        = 0;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind.descriptorCount = 1;
    bind.stageFlags     = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dlci = {0};
    dlci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 1;
    dlci.pBindings    = &bind;
    VK_CHECK(vkCreateDescriptorSetLayout(g.device, &dlci, NULL,
                                         &g.skybox_set_layout));

    VkDescriptorPoolSize pool_sz = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    MAX_CUBEMAPS};
    VkDescriptorPoolCreateInfo poolci = {0};
    poolci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolci.maxSets       = MAX_CUBEMAPS;
    poolci.poolSizeCount = 1;
    poolci.pPoolSizes    = &pool_sz;
    VK_CHECK(vkCreateDescriptorPool(g.device, &poolci, NULL, &g.skybox_pool));

    /* Push constant: inv_proj (mat4) + inv_view_rot (mat4) = 128 bytes. */
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, 128};
    VkPipelineLayoutCreateInfo plci = {0};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &g.skybox_set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &push;
    VK_CHECK(vkCreatePipelineLayout(g.device, &plci, NULL, &g.skybox_layout));

    VkShaderModule vmod, fmod;
    if (!create_shader_module("skybox.vert", &vmod) ||
        !create_shader_module("skybox.frag", &fmod))
        return false;

    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod; stages[0].pName = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = {0};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo ras = {0};
    ras.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    ras.polygonMode = VK_POLYGON_MODE_FILL;
    ras.cullMode    = VK_CULL_MODE_NONE;
    ras.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType           = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp  = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState att = {0};
    att.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend = {0};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1; blend.pAttachments = &att;
    VkDynamicState dyn_st[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {0};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyn_st;
    VkPipelineRenderingCreateInfo rci = {0};
    rci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    VkFormat skybox_color_fmt = PP_FORMAT;
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &skybox_color_fmt;
    rci.depthAttachmentFormat = g.depth_format;

    VkGraphicsPipelineCreateInfo info = {0};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &rci;
    info.stageCount          = 2; info.pStages = stages;
    info.pVertexInputState   = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState      = &vps;
    info.pRasterizationState = &ras;
    info.pMultisampleState   = &ms;
    info.pDepthStencilState  = &ds;
    info.pColorBlendState    = &blend;
    info.pDynamicState       = &dyn;
    info.layout              = g.skybox_layout;

    VkResult res = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE,
                                             1, &info, NULL, &g.skybox_pipeline);
    vkDestroyShaderModule(g.device, vmod, NULL);
    vkDestroyShaderModule(g.device, fmod, NULL);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[render] skybox pipeline failed: %d\n", res);
        return false;
    }
    return true;
}

static bool create_shadow_pipeline(void) {
    VkShaderModule vert;
    if (!create_shader_module("shadow.vert", &vert)) return false;

    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mat4_t)};
    VkPipelineLayoutCreateInfo layout_ci = {0};
    layout_ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges    = &push;
    VK_CHECK(vkCreatePipelineLayout(g.device, &layout_ci, NULL, &g.shadow_layout));

    VkPipelineShaderStageCreateInfo stage = {0};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vert;
    stage.pName  = "main";

    /* Only read position (location 0); normal and uv sit in the buffer but
       are not declared in shadow.vert — the stride covers the full vertex. */
    VkVertexInputBindingDescription vbind = {0, sizeof(gpu_vertex_t), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vattr = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                               offsetof(gpu_vertex_t, position)};
    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &vbind;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions    = &vattr;

    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp_state = {0};
    vp_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_state.viewportCount = 1;
    vp_state.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster = {0};
    raster.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode             = VK_POLYGON_MODE_FILL;
    raster.cullMode                = VK_CULL_MODE_BACK_BIT;
    raster.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth               = 1.0f;
    raster.depthBiasEnable         = VK_TRUE;
    raster.depthBiasConstantFactor = 1.25f;
    raster.depthBiasSlopeFactor    = 1.75f;

    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendStateCreateInfo blend = {0};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {0};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkPipelineRenderingCreateInfo rendering = {0};
    rendering.sType                = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    VkGraphicsPipelineCreateInfo info = {0};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &rendering;
    info.stageCount          = 1;
    info.pStages             = &stage;
    info.pVertexInputState   = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState      = &vp_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &ms;
    info.pDepthStencilState  = &ds;
    info.pColorBlendState    = &blend;
    info.pDynamicState       = &dyn;
    info.layout              = g.shadow_layout;

    VkResult res = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1,
                                             &info, NULL, &g.shadow_pipeline);
    vkDestroyShaderModule(g.device, vert, NULL);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[render] shadow pipeline creation failed: %d\n", res);
        return false;
    }
    return true;
}

/* Create one mesh pipeline variant.  The layout must already be in
   g.pipeline_layout.  poly_mode selects fill vs wireframe. */
static bool build_mesh_pipeline(VkPolygonMode poly_mode, VkPipeline *out) {
    VkShaderModule vert, frag;
    if (!create_shader_module("mesh.vert", &vert) ||
        !create_shader_module("mesh.frag", &frag)) {
        return false;
    }

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
    binding.stride    = sizeof(gpu_vertex_t);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[4] = {0};
    attrs[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(gpu_vertex_t, position)};
    attrs[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(gpu_vertex_t, normal)};
    attrs[2] = (VkVertexInputAttributeDescription){2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(gpu_vertex_t, uv)};
    attrs[3] = (VkVertexInputAttributeDescription){3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(gpu_vertex_t, tangent)};

    VkPipelineVertexInputStateCreateInfo vertex_input = {0};
    vertex_input.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 1;
    vertex_input.pVertexBindingDescriptions      = &binding;
    vertex_input.vertexAttributeDescriptionCount = 4;
    vertex_input.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {0};
    input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state = {0};
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster = {0};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = poly_mode;
    raster.cullMode    = VK_CULL_MODE_NONE; /* no culling: viewport Y-flip reverses winding */
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {0};
    multisample.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth = {0};
    depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable  = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend_attach = {0};
    blend_attach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend = {0};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blend_attach;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {0};
    dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates    = dynamic_states;

    VkPipelineRenderingCreateInfo rendering = {0};
    rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount    = 1;
    rendering.pColorAttachmentFormats = &g.swapchain_format;
    rendering.depthAttachmentFormat   = g.depth_format;

    VkGraphicsPipelineCreateInfo info = {0};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &rendering;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState      = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &multisample;
    info.pDepthStencilState  = &depth;
    info.pColorBlendState    = &blend;
    info.pDynamicState       = &dynamic;
    info.layout              = g.pipeline_layout;
    info.renderPass          = VK_NULL_HANDLE; /* dynamic rendering */

    VkResult res = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &info,
                                             NULL, out);
    vkDestroyShaderModule(g.device, vert, NULL);
    vkDestroyShaderModule(g.device, frag, NULL);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[render] mesh pipeline creation failed: %d\n", res);
        return false;
    }
    return true;
}

/* Build one instanced mesh pipeline variant using mesh_inst.vert + mesh.frag.
   Both bindings declared: 0 = vertex (VERTEX_RATE), 1 = instance (INSTANCE_RATE). */
static bool build_inst_pipeline(VkPolygonMode poly_mode, VkPipeline *out) {
    VkShaderModule vert, frag;
    if (!create_shader_module("mesh_inst.vert", &vert) ||
        !create_shader_module("mesh.frag", &frag))
        return false;

    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription vbinds[2] = {0};
    vbinds[0].binding   = 0; vbinds[0].stride = sizeof(gpu_vertex_t);
    vbinds[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vbinds[1].binding   = 1; vbinds[1].stride = sizeof(mat4_t);
    vbinds[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription vattrs[8] = {0};
    vattrs[0] = (VkVertexInputAttributeDescription){0,0,VK_FORMAT_R32G32B32_SFLOAT,   offsetof(gpu_vertex_t,position)};
    vattrs[1] = (VkVertexInputAttributeDescription){1,0,VK_FORMAT_R32G32B32_SFLOAT,   offsetof(gpu_vertex_t,normal)};
    vattrs[2] = (VkVertexInputAttributeDescription){2,0,VK_FORMAT_R32G32_SFLOAT,      offsetof(gpu_vertex_t,uv)};
    vattrs[3] = (VkVertexInputAttributeDescription){3,0,VK_FORMAT_R32G32B32_SFLOAT,   offsetof(gpu_vertex_t,tangent)};
    vattrs[4] = (VkVertexInputAttributeDescription){4,1,VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    vattrs[5] = (VkVertexInputAttributeDescription){5,1,VK_FORMAT_R32G32B32A32_SFLOAT,16};
    vattrs[6] = (VkVertexInputAttributeDescription){6,1,VK_FORMAT_R32G32B32A32_SFLOAT,32};
    vattrs[7] = (VkVertexInputAttributeDescription){7,1,VK_FORMAT_R32G32B32A32_SFLOAT,48};

    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 2;
    vi.pVertexBindingDescriptions      = vbinds;
    vi.vertexAttributeDescriptionCount = 8;
    vi.pVertexAttributeDescriptions    = vattrs;

    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp_s = {0};
    vp_s.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_s.viewportCount = 1;
    vp_s.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster = {0};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = poly_mode;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend_att = {0};
    blend_att.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend = {0};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blend_att;

    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {0};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkPipelineRenderingCreateInfo rendering = {0};
    rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount    = 1;
    rendering.pColorAttachmentFormats = &g.swapchain_format;
    rendering.depthAttachmentFormat   = g.depth_format;

    VkGraphicsPipelineCreateInfo info = {0};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &rendering;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState      = &vp_s;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &ms;
    info.pDepthStencilState  = &ds;
    info.pColorBlendState    = &blend;
    info.pDynamicState       = &dyn;
    info.layout              = g.inst_layout;

    VkResult res = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1,
                                             &info, NULL, out);
    vkDestroyShaderModule(g.device, vert, NULL);
    vkDestroyShaderModule(g.device, frag, NULL);
    return res == VK_SUCCESS;
}

static bool create_inst_pipelines(void) {
    /* Layout: same descriptor sets as mesh pipeline, 64-byte push constant. */
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mat4_t)};
    VkDescriptorSetLayout set_layouts[] = {g.material_set_layout, g.scene_set_layout};
    VkPipelineLayoutCreateInfo lci = {0};
    lci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.setLayoutCount         = 2;
    lci.pSetLayouts            = set_layouts;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &push;
    VK_CHECK(vkCreatePipelineLayout(g.device, &lci, NULL, &g.inst_layout));

    if (!build_inst_pipeline(VK_POLYGON_MODE_FILL, &g.inst_pipeline))
        return false;
    if (g.wireframe_supported &&
        !build_inst_pipeline(VK_POLYGON_MODE_LINE, &g.inst_wireframe_pipeline))
        return false;

    /* Instanced shadow pipeline — reuses shadow_layout (same 64-byte push). */
    VkShaderModule sv;
    if (!create_shader_module("shadow_inst.vert", &sv)) return false;

    VkPipelineShaderStageCreateInfo sstage = {0};
    sstage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    sstage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    sstage.module = sv;
    sstage.pName  = "main";

    VkVertexInputBindingDescription svbinds[2] = {0};
    svbinds[0].binding = 0; svbinds[0].stride = sizeof(gpu_vertex_t);
    svbinds[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    svbinds[1].binding = 1; svbinds[1].stride = sizeof(mat4_t);
    svbinds[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription svattrs[5] = {0};
    svattrs[0] = (VkVertexInputAttributeDescription){0,0,VK_FORMAT_R32G32B32_SFLOAT,   offsetof(gpu_vertex_t,position)};
    svattrs[1] = (VkVertexInputAttributeDescription){4,1,VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    svattrs[2] = (VkVertexInputAttributeDescription){5,1,VK_FORMAT_R32G32B32A32_SFLOAT,16};
    svattrs[3] = (VkVertexInputAttributeDescription){6,1,VK_FORMAT_R32G32B32A32_SFLOAT,32};
    svattrs[4] = (VkVertexInputAttributeDescription){7,1,VK_FORMAT_R32G32B32A32_SFLOAT,48};

    VkPipelineVertexInputStateCreateInfo svi = {0};
    svi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    svi.vertexBindingDescriptionCount   = 2;
    svi.pVertexBindingDescriptions      = svbinds;
    svi.vertexAttributeDescriptionCount = 5;
    svi.pVertexAttributeDescriptions    = svattrs;

    VkPipelineInputAssemblyStateCreateInfo sia = {0};
    sia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    sia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo svp = {0};
    svp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    svp.viewportCount = 1;
    svp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo srast = {0};
    srast.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    srast.polygonMode             = VK_POLYGON_MODE_FILL;
    srast.cullMode                = VK_CULL_MODE_BACK_BIT;
    srast.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    srast.lineWidth               = 1.0f;
    srast.depthBiasEnable         = VK_TRUE;
    srast.depthBiasConstantFactor = 1.25f;
    srast.depthBiasSlopeFactor    = 1.75f;

    VkPipelineMultisampleStateCreateInfo sms = {0};
    sms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    sms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo sds = {0};
    sds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    sds.depthTestEnable  = VK_TRUE;
    sds.depthWriteEnable = VK_TRUE;
    sds.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendStateCreateInfo sblend = {0};
    sblend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

    VkDynamicState sdyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo sdyn = {0};
    sdyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    sdyn.dynamicStateCount = 2;
    sdyn.pDynamicStates    = sdyn_states;

    VkPipelineRenderingCreateInfo srender = {0};
    srender.sType                 = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    srender.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    VkGraphicsPipelineCreateInfo sinfo = {0};
    sinfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    sinfo.pNext               = &srender;
    sinfo.stageCount          = 1;
    sinfo.pStages             = &sstage;
    sinfo.pVertexInputState   = &svi;
    sinfo.pInputAssemblyState = &sia;
    sinfo.pViewportState      = &svp;
    sinfo.pRasterizationState = &srast;
    sinfo.pMultisampleState   = &sms;
    sinfo.pDepthStencilState  = &sds;
    sinfo.pColorBlendState    = &sblend;
    sinfo.pDynamicState       = &sdyn;
    sinfo.layout              = g.shadow_layout;

    VkResult res = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1,
                                             &sinfo, NULL, &g.shadow_inst_pipeline);
    vkDestroyShaderModule(g.device, sv, NULL);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[render] shadow_inst pipeline failed: %d\n", res);
        return false;
    }

    /* Per-frame GPU instance buffer (persistently mapped). */
    VkDeviceSize ibuf_size = (VkDeviceSize)MAX_INST_TOTAL * sizeof(mat4_t);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (!create_buffer(ibuf_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &g.inst_vbuf[i], &g.inst_vmem[i]))
            return false;
        VK_CHECK(vkMapMemory(g.device, g.inst_vmem[i], 0, ibuf_size, 0,
                             &g.inst_vmapped[i]));
    }
    return true;
}

static bool create_cull_pipeline(void) {
    VkDescriptorSetLayoutBinding bindings[4] = {0};
    for (int i = 0; i < 4; i++) {
        bindings[i].binding         = (uint32_t)i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci = {0};
    dslci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 4;
    dslci.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(g.device, &dslci, NULL, &g.cull_set_layout));

    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      MAX_FRAMES_IN_FLIGHT * 4};
    VkDescriptorPoolCreateInfo dpci = {0};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = MAX_FRAMES_IN_FLIGHT;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &pool_size;
    VK_CHECK(vkCreateDescriptorPool(g.device, &dpci, NULL, &g.cull_pool));

    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cull_push_t)};
    VkPipelineLayoutCreateInfo plci = {0};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &g.cull_set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &push;
    VK_CHECK(vkCreatePipelineLayout(g.device, &plci, NULL, &g.cull_layout));

    VkShaderModule cs;
    if (!create_shader_module("cull.comp", &cs)) return false;
    VkPipelineShaderStageCreateInfo stage = {0};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = cs;
    stage.pName  = "main";
    VkComputePipelineCreateInfo cpci = {0};
    cpci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage  = stage;
    cpci.layout = g.cull_layout;
    VkResult res = vkCreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &cpci, NULL,
                                            &g.cull_pipeline);
    vkDestroyShaderModule(g.device, cs, NULL);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[render] cull compute pipeline failed: %d\n", res);
        return false;
    }

    /* Allocate per-frame buffers and descriptor sets. */
    VkDeviceSize inst_size     = (VkDeviceSize)MAX_INST_TOTAL   * sizeof(cull_inst_t);
    VkDeviceSize batch_size    = (VkDeviceSize)MAX_INST_BATCHES * sizeof(cull_batch_t);
    VkDeviceSize out_size      = (VkDeviceSize)MAX_INST_TOTAL   * sizeof(mat4_t);
    VkDeviceSize indirect_size = (VkDeviceSize)MAX_INST_BATCHES * sizeof(VkDrawIndexedIndirectCommand);

    for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
        if (!create_buffer(inst_size,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &g.cull_inst_buf[f], &g.cull_inst_mem[f])) return false;
        VK_CHECK(vkMapMemory(g.device, g.cull_inst_mem[f], 0, VK_WHOLE_SIZE, 0,
                             &g.cull_inst_mapped[f]));

        if (!create_buffer(batch_size,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &g.cull_batch_buf[f], &g.cull_batch_mem[f])) return false;
        VK_CHECK(vkMapMemory(g.device, g.cull_batch_mem[f], 0, VK_WHOLE_SIZE, 0,
                             &g.cull_batch_mapped[f]));

        if (!create_buffer(out_size,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           &g.cull_out_buf[f], &g.cull_out_mem[f])) return false;

        if (!create_buffer(indirect_size,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &g.cull_indirect_buf[f], &g.cull_indirect_mem[f])) return false;
        VK_CHECK(vkMapMemory(g.device, g.cull_indirect_mem[f], 0, VK_WHOLE_SIZE, 0,
                             &g.cull_indirect_mapped[f]));
    }

    VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT] = {g.cull_set_layout, g.cull_set_layout};
    VkDescriptorSetAllocateInfo dsai = {0};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = g.cull_pool;
    dsai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    dsai.pSetLayouts        = layouts;
    VK_CHECK(vkAllocateDescriptorSets(g.device, &dsai, g.cull_sets));

    for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
        VkDescriptorBufferInfo bufs[4] = {
            {g.cull_inst_buf[f],     0, inst_size},
            {g.cull_batch_buf[f],    0, batch_size},
            {g.cull_out_buf[f],      0, out_size},
            {g.cull_indirect_buf[f], 0, indirect_size},
        };
        VkWriteDescriptorSet writes[4] = {0};
        for (int b = 0; b < 4; b++) {
            writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet          = g.cull_sets[f];
            writes[b].dstBinding      = (uint32_t)b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo     = &bufs[b];
        }
        vkUpdateDescriptorSets(g.device, 4, writes, 0, NULL);
    }
    return true;
}

static bool create_particle_pipeline(void) {
    VkDescriptorSetLayoutBinding bindings[4] = {0};
    for (int i = 0; i < 4; i++) {
        bindings[i].binding         = (uint32_t)i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci = {0};
    dslci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 4;
    dslci.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(g.device, &dslci, NULL, &g.particle_set_layout));

    VkDescriptorPoolSize pool_size = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        MAX_GPU_EMITTERS * MAX_FRAMES_IN_FLIGHT * 4
    };
    VkDescriptorPoolCreateInfo dpci = {0};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = MAX_GPU_EMITTERS * MAX_FRAMES_IN_FLIGHT;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &pool_size;
    VK_CHECK(vkCreateDescriptorPool(g.device, &dpci, NULL, &g.particle_pool));

    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(particle_push_t)};
    VkPipelineLayoutCreateInfo plci = {0};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &g.particle_set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &push;
    VK_CHECK(vkCreatePipelineLayout(g.device, &plci, NULL, &g.particle_layout));

    VkShaderModule cs;
    if (!create_shader_module("particles.comp", &cs)) return false;

    VkPipelineShaderStageCreateInfo stage = {0};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = cs;
    stage.pName  = "main";

    VkComputePipelineCreateInfo cpci = {0};
    cpci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage  = stage;
    cpci.layout = g.particle_layout;

    VkResult res = vkCreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &cpci, NULL,
                                            &g.particle_pipeline);
    vkDestroyShaderModule(g.device, cs, NULL);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[render] particle compute pipeline failed: %d\n", res);
        return false;
    }
    return true;
}

static bool create_pipeline(void) {
    VkPushConstantRange push = {0};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.size       = sizeof(mesh_push_t);

    VkDescriptorSetLayout set_layouts[] = {g.material_set_layout, g.scene_set_layout};
    VkPipelineLayoutCreateInfo layout = {0};
    layout.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount         = 2;
    layout.pSetLayouts            = set_layouts;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges    = &push;
    VK_CHECK(vkCreatePipelineLayout(g.device, &layout, NULL, &g.pipeline_layout));

    if (!build_mesh_pipeline(VK_POLYGON_MODE_FILL, &g.pipeline))
        return false;
    if (g.wireframe_supported &&
        !build_mesh_pipeline(VK_POLYGON_MODE_LINE, &g.wireframe_pipeline))
        return false;
    return true;
}

/* ----------------------------------------------------------------------- */
/* text pipeline + buffers                                                  */
/* ----------------------------------------------------------------------- */

static bool create_text_pipeline(void) {
    VkShaderModule vert, frag;
    if (!create_shader_module("text.vert", &vert) ||
        !create_shader_module("text.frag", &frag)) {
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding = {0};
    binding.binding = 0;
    binding.stride = sizeof(text_vertex_t);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2] = {0};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = offsetof(text_vertex_t, pos);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = offsetof(text_vertex_t, color);

    VkPipelineVertexInputStateCreateInfo vertex_input = {0};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {0};
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state = {0};
    viewport_state.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster = {0};
    raster.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {0};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* Overlay: never touch depth, but the attachment is still bound. */
    VkPipelineDepthStencilStateCreateInfo depth = {0};
    depth.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_FALSE;
    depth.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend_attach = {0};
    blend_attach.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend = {0};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attach;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                       VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {0};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkPushConstantRange push = {0};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    push.size = sizeof(float) * 2; /* screen size */

    VkPipelineLayoutCreateInfo layout = {0};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(g.device, &layout, NULL, &g.text_layout));

    /* Overlay renders in the swapchain-only scope (no depth attachment). */
    VkPipelineRenderingCreateInfo rendering = {0};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &g.swapchain_format;

    VkGraphicsPipelineCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = g.text_layout;
    info.renderPass = VK_NULL_HANDLE;

    VkResult res = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &info,
                                             NULL, &g.text_pipeline);
    vkDestroyShaderModule(g.device, vert, NULL);
    vkDestroyShaderModule(g.device, frag, NULL);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[render] text pipeline creation failed: %d\n", res);
        return false;
    }
    return true;
}

static bool create_text_buffers(void) {
    g.text_verts = malloc(sizeof(text_vertex_t) * MAX_TEXT_VERTS);
    if (!g.text_verts) {
        return false;
    }
    VkDeviceSize size = sizeof(text_vertex_t) * MAX_TEXT_VERTS;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (!create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &g.text_buffer[i], &g.text_memory[i])) {
            return false;
        }
        VK_CHECK(vkMapMemory(g.device, g.text_memory[i], 0, size, 0,
                             &g.text_mapped[i]));
    }
    return true;
}

/* ----------------------------------------------------------------------- */
/* commands + sync                                                          */
/* ----------------------------------------------------------------------- */

static bool create_render_finished_semaphores(void) {
    g.render_finished = malloc(sizeof(VkSemaphore) * g.image_count);
    VkSemaphoreCreateInfo sem = {0};
    sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < g.image_count; i++) {
        VK_CHECK(vkCreateSemaphore(g.device, &sem, NULL, &g.render_finished[i]));
    }
    return true;
}

static bool create_commands_and_sync(void) {
    VkCommandPoolCreateInfo pool = {0};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = g.graphics_family;
    VK_CHECK(vkCreateCommandPool(g.device, &pool, NULL, &g.command_pool));

    VkCommandBufferAllocateInfo alloc = {0};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = g.command_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    VK_CHECK(vkAllocateCommandBuffers(g.device, &alloc, g.command_buffers));

    VkSemaphoreCreateInfo sem = {0};
    sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fence = {0};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VK_CHECK(vkCreateSemaphore(g.device, &sem, NULL, &g.image_available[i]));
        VK_CHECK(vkCreateFence(g.device, &fence, NULL, &g.in_flight[i]));
    }
    /* render-finished is per swapchain image so present never waits on a
       semaphore the next frame's submit has already re-signalled. */
    return create_render_finished_semaphores();
}

/* ----------------------------------------------------------------------- */
/* swapchain teardown / recreate                                            */
/* ----------------------------------------------------------------------- */

static void destroy_swapchain(void) {
    destroy_pp_descriptors();
    destroy_render_targets();
    if (g.depth_view) {
        vkDestroyImageView(g.device, g.depth_view, NULL);
        vkDestroyImage(g.device, g.depth_image, NULL);
        vkFreeMemory(g.device, g.depth_memory, NULL);
        g.depth_view = VK_NULL_HANDLE;
    }
    if (g.render_finished) {
        for (uint32_t i = 0; i < g.image_count; i++) {
            vkDestroySemaphore(g.device, g.render_finished[i], NULL);
        }
        free(g.render_finished);
        g.render_finished = NULL;
    }
    if (g.image_views) {
        for (uint32_t i = 0; i < g.image_count; i++) {
            vkDestroyImageView(g.device, g.image_views[i], NULL);
        }
        free(g.image_views);
        g.image_views = NULL;
    }
    free(g.images);
    g.images = NULL;
    if (g.swapchain) {
        vkDestroySwapchainKHR(g.device, g.swapchain, NULL);
        g.swapchain = VK_NULL_HANDLE;
    }
}

static bool recreate_swapchain(void) {
    uint32_t w, h;
    window_size(g.window, &w, &h);
    if (w == 0 || h == 0) {
        return true; /* minimized */
    }
    vkDeviceWaitIdle(g.device);
    destroy_swapchain();
    if (!create_swapchain() || !create_depth_resources() ||
        !create_render_targets() || !create_render_finished_semaphores()) {
        return false;
    }
    if (g.pp_pool) create_pp_descriptors();
    return true;
}

/* ----------------------------------------------------------------------- */
/* frame recording                                                          */
/* ----------------------------------------------------------------------- */

static void image_barrier(VkCommandBuffer cmd, VkImage image,
                          VkImageAspectFlags aspect, VkImageLayout old_layout,
                          VkImageLayout new_layout,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1,
                         &barrier);
}

/* Per-mip-level colour barrier used during mipmap generation. */
static void image_barrier_mip(VkCommandBuffer cmd, VkImage image,
                               uint32_t base_mip, uint32_t level_count,
                               VkImageLayout old_layout, VkImageLayout new_layout,
                               VkAccessFlags src_access, VkAccessFlags dst_access,
                               VkPipelineStageFlags src_stage,
                               VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {0};
    b.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout                       = old_layout;
    b.newLayout                       = new_layout;
    b.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    b.image                           = image;
    b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel   = base_mip;
    b.subresourceRange.levelCount     = level_count;
    b.subresourceRange.layerCount     = 1;
    b.srcAccessMask                   = src_access;
    b.dstAccessMask                   = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

void render_set_camera(mat4_t view, mat4_t proj) {
    g.view = view;
    g.proj = proj;
    /* Camera world position = translation column of inverse(view). */
    mat4_t inv = mat4_inverse(view);
    g.scene_data.view_pos[0] = inv.m[12];
    g.scene_data.view_pos[1] = inv.m[13];
    g.scene_data.view_pos[2] = inv.m[14];
    g.scene_data.view_pos[3] = 1.0f;
    frustum_extract(&g.frustum, mat4_mul(proj, view));
}

void render_mesh(mesh_handle_t mesh, material_handle_t material, mat4_t model) {
    if (g.instance_count >= MAX_INSTANCES || mesh >= g.mesh_count ||
        material >= g.material_count) {
        return;
    }

    /* Frustum cull: transform local AABB corners into world space, refit,
       and reject draws whose world AABB lies entirely outside any plane. */
    const gpu_mesh_t *gm = &g.meshes[mesh];
    vec3_t lmin = gm->bounds_min, lmax = gm->bounds_max;
    vec3_t corners[8] = {
        {lmin.x, lmin.y, lmin.z}, {lmax.x, lmin.y, lmin.z},
        {lmin.x, lmax.y, lmin.z}, {lmax.x, lmax.y, lmin.z},
        {lmin.x, lmin.y, lmax.z}, {lmax.x, lmin.y, lmax.z},
        {lmin.x, lmax.y, lmax.z}, {lmax.x, lmax.y, lmax.z},
    };
    vec3_t wmin = mat4_transform_point(model, corners[0]);
    vec3_t wmax = wmin;
    for (int i = 1; i < 8; i++) {
        vec3_t w = mat4_transform_point(model, corners[i]);
        if (w.x < wmin.x) wmin.x = w.x;
        if (w.y < wmin.y) wmin.y = w.y;
        if (w.z < wmin.z) wmin.z = w.z;
        if (w.x > wmax.x) wmax.x = w.x;
        if (w.y > wmax.y) wmax.y = w.y;
        if (w.z > wmax.z) wmax.z = w.z;
    }
    if (!frustum_intersects_aabb(&g.frustum, wmin, wmax)) {
        return;
    }

    mesh_instance_t *inst = &g.instances[g.instance_count++];
    inst->mesh = mesh;
    inst->material = material;
    inst->mvp = mat4_mul(g.proj, mat4_mul(g.view, model));
    inst->model = model;
}

static void push_text_vertex(float x, float y, float r, float gr, float b) {
    if (g.text_vert_count >= MAX_TEXT_VERTS) {
        return;
    }
    text_vertex_t *v = &g.text_verts[g.text_vert_count++];
    v->pos[0] = x;
    v->pos[1] = y;
    v->color[0] = r;
    v->color[1] = gr;
    v->color[2] = b;
}

void render_text(float x, float y, float scale, float r, float gr, float b,
                 const char *str) {
    float pen_x = x;
    float pen_y = y;
    for (const char *p = str; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n') {
            pen_x = x;
            pen_y += 8.0f * scale;
            continue;
        }
        if (c < 0x20 || c > 0x7F) {
            c = '?';
        }
        const uint8_t *glyph = kiln_font8x8[c - 0x20];
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                if (!(glyph[row] & (0x80 >> col))) {
                    continue;
                }
                float px = pen_x + (float)col * scale;
                float py = pen_y + (float)row * scale;
                push_text_vertex(px, py, r, gr, b);
                push_text_vertex(px + scale, py, r, gr, b);
                push_text_vertex(px + scale, py + scale, r, gr, b);
                push_text_vertex(px, py, r, gr, b);
                push_text_vertex(px + scale, py + scale, r, gr, b);
                push_text_vertex(px, py + scale, r, gr, b);
            }
        }
        pen_x += 6.0f * scale; /* 5px glyph + 1px gap */
    }
}

void render_rect(float x, float y, float w, float h, float r, float g_,
                 float b) {
    push_text_vertex(x, y, r, g_, b);
    push_text_vertex(x + w, y, r, g_, b);
    push_text_vertex(x + w, y + h, r, g_, b);
    push_text_vertex(x, y, r, g_, b);
    push_text_vertex(x + w, y + h, r, g_, b);
    push_text_vertex(x, y + h, r, g_, b);
}

void render_line(float x0, float y0, float x1, float y1, float thickness,
                 float r, float g_, float b) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) {
        return;
    }
    float hw = thickness * 0.5f;
    float nx = -dy / len * hw; /* perpendicular, scaled to half-thickness */
    float ny = dx / len * hw;

    push_text_vertex(x0 + nx, y0 + ny, r, g_, b);
    push_text_vertex(x1 + nx, y1 + ny, r, g_, b);
    push_text_vertex(x1 - nx, y1 - ny, r, g_, b);
    push_text_vertex(x0 + nx, y0 + ny, r, g_, b);
    push_text_vertex(x1 - nx, y1 - ny, r, g_, b);
    push_text_vertex(x0 - nx, y0 - ny, r, g_, b);
}

void render_set_clear_color(float r, float g_, float b) {
    g.clear_color[0] = r;
    g.clear_color[1] = g_;
    g.clear_color[2] = b;
}

void render_set_light(vec3_t dir, vec3_t color, vec3_t ambient) {
    vec3_t n = vec3_normalize(dir);
    g.scene_data.light_dir[0]     = n.x;
    g.scene_data.light_dir[1]     = n.y;
    g.scene_data.light_dir[2]     = n.z;
    g.scene_data.light_color[0]   = color.x;
    g.scene_data.light_color[1]   = color.y;
    g.scene_data.light_color[2]   = color.z;
    g.scene_data.ambient_color[0] = ambient.x;
    g.scene_data.ambient_color[1] = ambient.y;
    g.scene_data.ambient_color[2] = ambient.z;
}

void render_set_bloom(bool enabled, float threshold, float strength, float exposure) {
    g.bloom_enabled   = enabled;
    g.bloom_threshold = threshold;
    g.bloom_strength  = strength;
    g.bloom_exposure  = exposure;
}

void render_save_screenshot(const char *path) {
    strncpy(g.screenshot_path, path, sizeof(g.screenshot_path) - 1);
    g.screenshot_path[sizeof(g.screenshot_path) - 1] = '\0';
}

void render_add_point_light(vec3_t pos, vec3_t color, float radius) {
    if (g.point_light_count >= RENDER_MAX_POINT_LIGHTS) return;
    point_light_ubo_t *pl = &g.scene_data.point_lights[g.point_light_count++];
    pl->pos[0] = pos.x;   pl->pos[1] = pos.y;   pl->pos[2] = pos.z;
    pl->color[0] = color.x; pl->color[1] = color.y; pl->color[2] = color.z;
    pl->params[0] = radius;
}

void render_mesh_instanced(mesh_handle_t mesh, material_handle_t material,
                           const mat4_t *models, uint32_t count) {
    if (count == 0 || mesh >= g.mesh_count || material >= g.material_count)
        return;
    if (g.inst_batch_count >= MAX_INST_BATCHES ||
        g.inst_model_count + count > MAX_INST_TOTAL) {
        fprintf(stderr, "[render] instanced batch overflow\n");
        return;
    }
    memcpy(&g.inst_models[g.inst_model_count], models, sizeof(mat4_t) * count);
    g.inst_batches[g.inst_batch_count++] = (inst_batch_t){
        mesh, material, g.inst_model_count, count
    };
    g.inst_model_count += count;
}

/* ---- GPU particle emitters ------------------------------------------------ */

static void destroy_gpu_emitter_resources(gpu_emitter_t *e) {
    for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
        if (e->emit_mapped[f])    { vkUnmapMemory(g.device, e->emit_mem[f]); e->emit_mapped[f] = NULL; }
        if (e->emit_buf[f])       { vkDestroyBuffer(g.device, e->emit_buf[f], NULL); e->emit_buf[f] = VK_NULL_HANDLE; }
        if (e->emit_mem[f])       { vkFreeMemory(g.device, e->emit_mem[f], NULL); e->emit_mem[f] = VK_NULL_HANDLE; }
        if (e->indirect_mapped[f]){ vkUnmapMemory(g.device, e->indirect_mem[f]); e->indirect_mapped[f] = NULL; }
        if (e->indirect_buf[f])   { vkDestroyBuffer(g.device, e->indirect_buf[f], NULL); e->indirect_buf[f] = VK_NULL_HANDLE; }
        if (e->indirect_mem[f])   { vkFreeMemory(g.device, e->indirect_mem[f], NULL); e->indirect_mem[f] = VK_NULL_HANDLE; }
        if (e->matrix_buf[f])     { vkDestroyBuffer(g.device, e->matrix_buf[f], NULL); e->matrix_buf[f] = VK_NULL_HANDLE; }
        if (e->matrix_mem[f])     { vkFreeMemory(g.device, e->matrix_mem[f], NULL); e->matrix_mem[f] = VK_NULL_HANDLE; }
    }
    if (e->particle_buf) { vkDestroyBuffer(g.device, e->particle_buf, NULL); e->particle_buf = VK_NULL_HANDLE; }
    if (e->particle_mem) { vkFreeMemory(g.device, e->particle_mem, NULL); e->particle_mem = VK_NULL_HANDLE; }
    e->active = false;
}

gpu_emitter_handle_t render_create_gpu_emitter(mesh_handle_t mesh,
                                               material_handle_t mat,
                                               uint32_t capacity,
                                               vec3_t gravity) {
    if (g.gpu_emitter_count >= MAX_GPU_EMITTERS ||
        mesh >= g.mesh_count || mat >= g.material_count || capacity == 0)
        return RENDER_GPU_EMITTER_INVALID;

    uint32_t idx = g.gpu_emitter_count++;
    gpu_emitter_t *e = &g.gpu_emitters[idx];
    memset(e, 0, sizeof(*e));
    e->mesh      = mesh;
    e->material  = mat;
    e->capacity  = capacity;
    e->gravity_y = gravity.y;
    e->active    = true;

    VkDeviceSize ps = (VkDeviceSize)capacity * sizeof(gpu_particle_t);
    VkDeviceSize ms = (VkDeviceSize)capacity * sizeof(mat4_t);
    VkDeviceSize is = sizeof(VkDrawIndexedIndirectCommand);
    VkDeviceSize es = (VkDeviceSize)MAX_EMIT_PER_FRAME * sizeof(gpu_particle_t);

    /* Particle simulation SSBO: device-local, needs TRANSFER_DST for zero-fill. */
    if (!create_buffer(ps,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       &e->particle_buf, &e->particle_mem)) goto fail;
    {
        VkCommandBuffer cmd = begin_single_time();
        vkCmdFillBuffer(cmd, e->particle_buf, 0, VK_WHOLE_SIZE, 0);
        end_single_time(cmd);
    }

    for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
        /* Matrix output: device-local, STORAGE + VERTEX_BUFFER. */
        if (!create_buffer(ms,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           &e->matrix_buf[f], &e->matrix_mem[f])) goto fail;

        /* Indirect args: host-visible so instance_count can be reset via mapped ptr. */
        if (!create_buffer(is,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &e->indirect_buf[f], &e->indirect_mem[f])) goto fail;
        vkMapMemory(g.device, e->indirect_mem[f], 0, VK_WHOLE_SIZE, 0, &e->indirect_mapped[f]);
        VkDrawIndexedIndirectCommand *ic = e->indirect_mapped[f];
        ic->indexCount    = g.meshes[mesh].index_count;
        ic->instanceCount = 0;
        ic->firstIndex    = 0;
        ic->vertexOffset  = 0;
        ic->firstInstance = 0;

        /* Emit staging: host-visible, STORAGE_BUFFER. */
        if (!create_buffer(es,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &e->emit_buf[f], &e->emit_mem[f])) goto fail;
        vkMapMemory(g.device, e->emit_mem[f], 0, VK_WHOLE_SIZE, 0, &e->emit_mapped[f]);
    }

    /* Allocate per-frame compute descriptor sets. */
    VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
    for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) layouts[f] = g.particle_set_layout;
    VkDescriptorSetAllocateInfo dsai = {0};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = g.particle_pool;
    dsai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    dsai.pSetLayouts        = layouts;
    if (vkAllocateDescriptorSets(g.device, &dsai, e->compute_sets) != VK_SUCCESS) goto fail;

    for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
        VkDescriptorBufferInfo bufs[4] = {
            {e->particle_buf,  0, ps},
            {e->matrix_buf[f], 0, ms},
            {e->indirect_buf[f], 0, is},
            {e->emit_buf[f],   0, es},
        };
        VkWriteDescriptorSet writes[4] = {0};
        for (int b = 0; b < 4; b++) {
            writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet          = e->compute_sets[f];
            writes[b].dstBinding      = (uint32_t)b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo     = &bufs[b];
        }
        vkUpdateDescriptorSets(g.device, 4, writes, 0, NULL);
    }
    return idx;

fail:
    destroy_gpu_emitter_resources(e);
    g.gpu_emitter_count--;
    return RENDER_GPU_EMITTER_INVALID;
}

void render_destroy_gpu_emitter(gpu_emitter_handle_t h) {
    if (h >= g.gpu_emitter_count || !g.gpu_emitters[h].active) return;
    vkDeviceWaitIdle(g.device);
    destroy_gpu_emitter_resources(&g.gpu_emitters[h]);
}

void render_gpu_emitter_emit(gpu_emitter_handle_t h,
                             vec3_t pos, vec3_t vel,
                             float life, float scale) {
    if (h >= g.gpu_emitter_count || !g.gpu_emitters[h].active) return;
    gpu_emitter_t *e = &g.gpu_emitters[h];
    if (e->emit_pending_count >= MAX_EMIT_PER_FRAME) return;
    gpu_particle_t *p = &e->emit_pending[e->emit_pending_count++];
    p->pos[0] = pos.x; p->pos[1] = pos.y; p->pos[2] = pos.z;
    p->life     = life;
    p->vel[0] = vel.x; p->vel[1] = vel.y; p->vel[2] = vel.z;
    p->max_life = life;
    p->scale    = scale;
    p->_pad[0]  = p->_pad[1] = p->_pad[2] = 0.0f;
}

void render_gpu_emitter_update(gpu_emitter_handle_t h, float dt) {
    if (h >= g.gpu_emitter_count || !g.gpu_emitters[h].active) return;
    if (g.particle_cmd_count >= MAX_GPU_EMITTERS) return;
    gpu_emitter_t *e = &g.gpu_emitters[h];
    e->pending_dt = dt;
    e->pending    = true;
    g.particle_cmds[g.particle_cmd_count++] = (gpu_particle_cmd_t){h};
}

/* --------------------------------------------------------------------------- */

static void record_command_buffer(VkCommandBuffer cmd, uint32_t image_index,
                                  uint32_t instance_count,
                                  uint32_t inst_batch_count,
                                  uint32_t inst_model_count,
                                  uint32_t text_verts,
                                  uint32_t particle_cmd_count) {
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

    /* ------------------------------------------------------------------ */
    /* Shadow pass: render scene depth from the light's perspective.        */
    /* ------------------------------------------------------------------ */

    /* Compute 3 cascade light view-projections. */
    vec3_t view_pos = {g.scene_data.view_pos[0],
                       g.scene_data.view_pos[1],
                       g.scene_data.view_pos[2]};
    vec3_t ld = {g.scene_data.light_dir[0],
                 g.scene_data.light_dir[1],
                 g.scene_data.light_dir[2]};
    vec3_t world_up  = (fabsf(ld.y) > 0.99f)
                       ? (vec3_t){0.0f, 0.0f, 1.0f}
                       : (vec3_t){0.0f, 1.0f, 0.0f};
    /* Each cascade uses an ortho box sized to its split radius. */
    const float cascade_sz[3] = {25.0f, 60.0f, 220.0f};
    mat4_t cascade_vp[3];
    for (int c = 0; c < 3; c++) {
        float sz   = cascade_sz[c];
        vec3_t eye = vec3_add(view_pos, vec3_scale(ld, sz * 0.8f));
        mat4_t lv  = mat4_look_at(eye, view_pos, world_up);
        mat4_t lp  = mat4_ortho(-sz, sz, -sz, sz, 1.0f, sz * 4.0f);
        cascade_vp[c] = mat4_mul(lp, lv);
        memcpy(g.scene_data.light_vp[c], cascade_vp[c].m,
               sizeof(cascade_vp[c].m));
    }

    /* GPU frustum culling: dispatch before shadow so both shadow and color passes
       use the compacted cull_out_buf instead of the raw inst_vbuf. */
    if (inst_batch_count > 0) {
        /* Make CPU writes (inst + batch inputs, instanceCount reset) visible. */
        VkMemoryBarrier hbar = {VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                NULL,
                                VK_ACCESS_HOST_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &hbar, 0, NULL, 0, NULL);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.cull_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                g.cull_layout, 0, 1, &g.cull_sets[g.frame], 0, NULL);

        cull_push_t cp = {0};
        for (int i = 0; i < 6; i++) {
            cp.planes[i][0] = g.frustum.planes[i].a;
            cp.planes[i][1] = g.frustum.planes[i].b;
            cp.planes[i][2] = g.frustum.planes[i].c;
            cp.planes[i][3] = g.frustum.planes[i].d;
        }
        cp.total_instances = inst_model_count;
        cp.batch_count     = inst_batch_count;
        vkCmdPushConstants(cmd, g.cull_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(cp), &cp);
        vkCmdDispatch(cmd, (inst_model_count + 63) / 64, 1, 1);

        /* cull_out_buf + cull_indirect_buf writes visible to draw pipeline. */
        VkMemoryBarrier cbar = {VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                NULL,
                                VK_ACCESS_SHADER_WRITE_BIT,
                                VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                                VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                             VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                             0, 1, &cbar, 0, NULL, 0, NULL);
    }

    /* Transition entire shadow array to depth-attachment for rendering. */
    {
        VkImageMemoryBarrier bar = {0};
        bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image               = g.shadow_image;
        bar.subresourceRange    = (VkImageSubresourceRange){
            VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 3};
        bar.dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            0, 0, NULL, 0, NULL, 1, &bar);
    }

    /* Render 3 cascades, each into its own layer. */
    VkViewport shadow_vp = {0.0f, 0.0f,
                            (float)SHADOW_MAP_SIZE, (float)SHADOW_MAP_SIZE,
                            0.0f, 1.0f};
    VkRect2D shadow_scissor = {{0, 0}, {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE}};

    for (int c = 0; c < 3; c++) {
        VkRenderingAttachmentInfo shadow_depth = {0};
        shadow_depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        shadow_depth.imageView   = g.shadow_layer_view[c];
        shadow_depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        shadow_depth.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        shadow_depth.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        shadow_depth.clearValue.depthStencil.depth = 1.0f;

        VkRenderingInfo sr = {0};
        sr.sType             = VK_STRUCTURE_TYPE_RENDERING_INFO;
        sr.renderArea.extent = (VkExtent2D){SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
        sr.layerCount        = 1;
        sr.pDepthAttachment  = &shadow_depth;
        vkCmdBeginRendering(cmd, &sr);
        vkCmdSetViewport(cmd, 0, 1, &shadow_vp);
        vkCmdSetScissor(cmd, 0, 1, &shadow_scissor);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          g.shadow_pipeline);
        for (uint32_t i = 0; i < instance_count; i++) {
            const mesh_instance_t *inst = &g.instances[i];
            const gpu_mesh_t *mesh      = &g.meshes[inst->mesh];
            mat4_t lmvp = mat4_mul(cascade_vp[c], inst->model);
            vkCmdPushConstants(cmd, g.shadow_layout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(mat4_t), &lmvp);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertex_buffer, &offset);
            vkCmdBindIndexBuffer(cmd, mesh->index_buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh->index_count, 1, 0, 0, 0);
        }
        if (inst_batch_count > 0) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              g.shadow_inst_pipeline);
            vkCmdPushConstants(cmd, g.shadow_layout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(mat4_t), &cascade_vp[c]);
            for (uint32_t b = 0; b < inst_batch_count; b++) {
                const inst_batch_t *batch = &g.inst_batches[b];
                const gpu_mesh_t   *mesh  = &g.meshes[batch->mesh];
                VkBuffer     vbufs[2] = {mesh->vertex_buffer, g.cull_out_buf[g.frame]};
                VkDeviceSize offs[2]  = {0, 0};
                vkCmdBindVertexBuffers(cmd, 0, 2, vbufs, offs);
                vkCmdBindIndexBuffer(cmd, mesh->index_buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexedIndirect(cmd, g.cull_indirect_buf[g.frame],
                                         b * sizeof(VkDrawIndexedIndirectCommand), 1, 0);
            }
        }
        vkCmdEndRendering(cmd);
    }

    /* Transition entire shadow array to shader-readable. */
    {
        VkImageMemoryBarrier bar = {0};
        bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        bar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image               = g.shadow_image;
        bar.subresourceRange    = (VkImageSubresourceRange){
            VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 3};
        bar.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        bar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, NULL, 0, NULL, 1, &bar);
    }

    /* ------------------------------------------------------------------ */
    /* GPU particle compute: inject + simulate (outside render pass).      */
    /* ------------------------------------------------------------------ */

    for (uint32_t pi = 0; pi < particle_cmd_count; pi++) {
        gpu_emitter_t *e = &g.gpu_emitters[g.particle_cmds[pi].emitter_idx];

        /* Serialize against previous frame's compute on the shared particle_buf. */
        VkBufferMemoryBarrier pbar = {0};
        pbar.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        pbar.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        pbar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        pbar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pbar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pbar.buffer              = e->particle_buf;
        pbar.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, NULL, 1, &pbar, 0, NULL);

        /* Make host write of instance_count=0 visible to the compute shader. */
        VkBufferMemoryBarrier hbar = {0};
        hbar.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        hbar.srcAccessMask       = VK_ACCESS_HOST_WRITE_BIT;
        hbar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        hbar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hbar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hbar.buffer              = e->indirect_buf[g.frame];
        hbar.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, NULL, 1, &hbar, 0, NULL);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.particle_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            g.particle_layout, 0, 1, &e->compute_sets[g.frame], 0, NULL);

        particle_push_t push = {0};
        push.gravity_y = e->gravity_y;
        push.capacity  = e->capacity;

        /* Inject pass: write staged CPU-emitted particles into the ring buffer. */
        uint32_t ec = e->emit_count[g.frame];
        if (ec > 0) {
            push.mode       = 0;
            push.emit_count = ec;
            push.ring_head  = e->emit_ring_head[g.frame];
            push.dt         = 0.0f;
            vkCmdPushConstants(cmd, g.particle_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(push), &push);
            vkCmdDispatch(cmd, (ec + 63) / 64, 1, 1);

            /* Barrier: inject writes to particle_buf → simulate reads it. */
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, NULL, 1, &pbar, 0, NULL);
        }

        /* Simulate pass: advance physics, output alive matrices + indirect count. */
        push.mode       = 1;
        push.dt         = e->pending_dt;
        push.emit_count = 0;
        push.ring_head  = 0;
        vkCmdPushConstants(cmd, g.particle_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);
        vkCmdDispatch(cmd, (e->capacity + 63) / 64, 1, 1);
    }

    /* After all compute: make matrix/indirect writes visible to draw indirect + vertex. */
    if (particle_cmd_count > 0) {
        VkMemoryBarrier mbar = {0};
        mbar.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mbar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mbar.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                             VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            0, 1, &mbar, 0, NULL, 0, NULL);
    }

    /* ------------------------------------------------------------------ */
    /* HDR scene pass  →  g.color_image                                    */
    /* ------------------------------------------------------------------ */

    image_barrier(cmd, g.color_image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    image_barrier(cmd, g.depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
                  VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, 0,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

    VkRenderingAttachmentInfo color = {0};
    color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView   = g.color_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color.float32[0] = g.clear_color[0];
    color.clearValue.color.float32[1] = g.clear_color[1];
    color.clearValue.color.float32[2] = g.clear_color[2];
    color.clearValue.color.float32[3] = 1.0f;

    VkRenderingAttachmentInfo depth = {0};
    depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView   = g.depth_view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil.depth = 1.0f;

    VkRenderingInfo scene_ri = {0};
    scene_ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    scene_ri.renderArea.extent    = g.extent;
    scene_ri.layerCount           = 1;
    scene_ri.colorAttachmentCount = 1;
    scene_ri.pColorAttachments    = &color;
    scene_ri.pDepthAttachment     = &depth;
    vkCmdBeginRendering(cmd, &scene_ri);

    /* Negative-height viewport flips Y for right-handed NDC. */
    VkViewport viewport = {0};
    viewport.x = 0.0f; viewport.y = (float)g.extent.height;
    viewport.width = (float)g.extent.width;
    viewport.height = -(float)g.extent.height;
    viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor = {0};
    scissor.extent = g.extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    /* Skybox: draw before meshes with depth test ≤, no depth write. */
    if (g.active_skybox < g.cubemap_count) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          g.skybox_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                g.skybox_layout, 0, 1,
                                &g.skybox_sets[g.active_skybox], 0, NULL);
        /* Push inv_proj + inv_view_rot (translation stripped). */
        mat4_t inv_proj     = mat4_inverse(g.proj);
        mat4_t view_rot     = g.view;
        view_rot.m[12] = view_rot.m[13] = view_rot.m[14] = 0.0f;
        mat4_t inv_view_rot = mat4_transpose(view_rot); /* orthogonal → transpose = inverse */
        mat4_t sky_push[2]  = {inv_proj, inv_view_rot};
        vkCmdPushConstants(cmd, g.skybox_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, 128, sky_push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    VkPipeline mesh_pipe = (g.wireframe && g.wireframe_pipeline)
                           ? g.wireframe_pipeline : g.pipeline;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipe);

    g.scene_data.light_dir[3] = (float)g.point_light_count;
    memcpy(g.scene_ubo_mapped[g.frame], &g.scene_data, sizeof(g.scene_data));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g.pipeline_layout, 1, 1,
                            &g.scene_sets[g.frame], 0, NULL);

    for (uint32_t i = 0; i < instance_count; i++) {
        const mesh_instance_t *inst = &g.instances[i];
        const gpu_mesh_t *mesh = &g.meshes[inst->mesh];
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                g.pipeline_layout, 0, 1,
                                &g.materials[inst->material].set, 0, NULL);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertex_buffer, &offset);
        vkCmdBindIndexBuffer(cmd, mesh->index_buffer, 0, VK_INDEX_TYPE_UINT32);
        mesh_push_t push = {inst->mvp, inst->model};
        vkCmdPushConstants(cmd, g.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(push), &push);
        vkCmdDrawIndexed(cmd, mesh->index_count, 1, 0, 0, 0);
    }

    if (inst_batch_count > 0) {
        VkPipeline ip = (g.wireframe && g.inst_wireframe_pipeline)
                        ? g.inst_wireframe_pipeline : g.inst_pipeline;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ip);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                g.inst_layout, 1, 1,
                                &g.scene_sets[g.frame], 0, NULL);
        mat4_t vp = mat4_mul(g.proj, g.view);
        vkCmdPushConstants(cmd, g.inst_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(mat4_t), &vp);
        for (uint32_t b = 0; b < inst_batch_count; b++) {
            const inst_batch_t *batch = &g.inst_batches[b];
            const gpu_mesh_t   *mesh  = &g.meshes[batch->mesh];
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    g.inst_layout, 0, 1,
                                    &g.materials[batch->material].set, 0, NULL);
            /* Instance VBO is the compute-culled output; firstInstance in the
               indirect command handles the per-batch base offset. */
            VkBuffer     vbufs[2] = {mesh->vertex_buffer, g.cull_out_buf[g.frame]};
            VkDeviceSize offs[2]  = {0, 0};
            vkCmdBindVertexBuffers(cmd, 0, 2, vbufs, offs);
            vkCmdBindIndexBuffer(cmd, mesh->index_buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(cmd, g.cull_indirect_buf[g.frame],
                                     b * sizeof(VkDrawIndexedIndirectCommand), 1, 0);
        }
    }

    /* GPU particle indirect draws (reuse instanced pipeline). */
    if (particle_cmd_count > 0) {
        VkPipeline ip = (g.wireframe && g.inst_wireframe_pipeline)
                        ? g.inst_wireframe_pipeline : g.inst_pipeline;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ip);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                g.inst_layout, 1, 1,
                                &g.scene_sets[g.frame], 0, NULL);
        mat4_t vp = mat4_mul(g.proj, g.view);
        vkCmdPushConstants(cmd, g.inst_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(mat4_t), &vp);
        for (uint32_t pi = 0; pi < particle_cmd_count; pi++) {
            gpu_emitter_t *e = &g.gpu_emitters[g.particle_cmds[pi].emitter_idx];
            const gpu_mesh_t *mesh = &g.meshes[e->mesh];
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    g.inst_layout, 0, 1,
                                    &g.materials[e->material].set, 0, NULL);
            VkBuffer     vbufs[2] = {mesh->vertex_buffer, e->matrix_buf[g.frame]};
            VkDeviceSize offs[2]  = {0, 0};
            vkCmdBindVertexBuffers(cmd, 0, 2, vbufs, offs);
            vkCmdBindIndexBuffer(cmd, mesh->index_buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(cmd, e->indirect_buf[g.frame], 0, 1,
                                     sizeof(VkDrawIndexedIndirectCommand));
        }
    }

    vkCmdEndRendering(cmd);

    /* ------------------------------------------------------------------ */
    /* Bloom passes  (threshold → blur H → blur V)                         */
    /* ------------------------------------------------------------------ */

    /* color_image: COLOR_ATTACHMENT → SHADER_READ_ONLY for sampling. */
    image_barrier(cmd, g.color_image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    /* Helper: render one fullscreen post-process pass. */
#define PP_PASS(target_img, target_view, target_ext, ds, src_set, push_data, push_sz) do { \
    image_barrier(cmd, (target_img), VK_IMAGE_ASPECT_COLOR_BIT,                 \
                  (ds), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,             \
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,                          \
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,                             \
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);                \
    VkRenderingAttachmentInfo _a = {0};                                          \
    _a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;                      \
    _a.imageView = (target_view); _a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; \
    _a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; _a.storeOp = VK_ATTACHMENT_STORE_OP_STORE; \
    VkRenderingInfo _ri = {0};                                                   \
    _ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;                                \
    _ri.renderArea.extent = (target_ext); _ri.layerCount = 1;                   \
    _ri.colorAttachmentCount = 1; _ri.pColorAttachments = &_a;                  \
    vkCmdBeginRendering(cmd, &_ri);                                              \
    VkViewport _vp = {0,0,(float)(target_ext).width,(float)(target_ext).height,0,1}; \
    vkCmdSetViewport(cmd,0,1,&_vp);                                             \
    VkRect2D _sc = {{0,0},{(target_ext).width,(target_ext).height}};            \
    vkCmdSetScissor(cmd,0,1,&_sc);                                              \
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,               \
                            g.pp_layout, 0, 1, &(src_set), 0, NULL);            \
    vkCmdPushConstants(cmd, g.pp_layout, VK_SHADER_STAGE_FRAGMENT_BIT,          \
                       0, (push_sz), (push_data));                               \
    vkCmdDraw(cmd, 3, 1, 0, 0);                                                 \
    vkCmdEndRendering(cmd);                                                      \
    image_barrier(cmd, (target_img), VK_IMAGE_ASPECT_COLOR_BIT,                 \
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,                      \
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,                      \
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,                          \
                  VK_ACCESS_SHADER_READ_BIT,                                     \
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,                 \
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT); } while(0)

    /* Threshold: color → bloom_a */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      g.pp_threshold_pipeline);
    float thr_push[4] = {g.bloom_threshold, 0, 0, 0};
    PP_PASS(g.bloom_a_image, g.bloom_a_view, g.bloom_extent,
            VK_IMAGE_LAYOUT_UNDEFINED, g.pp_threshold_set, thr_push, 16);

    /* Blur H: bloom_a → bloom_b */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pp_blur_pipeline);
    float blur_h[2] = {1.0f / (float)g.bloom_extent.width, 0.0f};
    PP_PASS(g.bloom_b_image, g.bloom_b_view, g.bloom_extent,
            VK_IMAGE_LAYOUT_UNDEFINED, g.pp_blur_a_set, blur_h, 8);

    /* Blur V: bloom_b → bloom_a (reuse bloom_a as output) */
    image_barrier(cmd, g.bloom_a_image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    float blur_v[2] = {0.0f, 1.0f / (float)g.bloom_extent.height};
    /* Re-use PP_PASS macro but bloom_a starts in COLOR_ATTACHMENT already. */
    {
        VkRenderingAttachmentInfo _a = {0};
        _a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        _a.imageView = g.bloom_a_view;
        _a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        _a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        _a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo _ri = {0};
        _ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        _ri.renderArea.extent = g.bloom_extent; _ri.layerCount = 1;
        _ri.colorAttachmentCount = 1; _ri.pColorAttachments = &_a;
        vkCmdBeginRendering(cmd, &_ri);
        VkViewport _vp = {0,0,(float)g.bloom_extent.width,(float)g.bloom_extent.height,0,1};
        vkCmdSetViewport(cmd,0,1,&_vp);
        VkRect2D _sc = {{0,0},{g.bloom_extent.width,g.bloom_extent.height}};
        vkCmdSetScissor(cmd,0,1,&_sc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                g.pp_layout, 0, 1, &g.pp_blur_b_set, 0, NULL);
        vkCmdPushConstants(cmd, g.pp_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, 8, blur_v);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }
    image_barrier(cmd, g.bloom_a_image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
#undef PP_PASS

    /* ------------------------------------------------------------------ */
    /* Composite + overlay  →  swapchain image                             */
    /* ------------------------------------------------------------------ */

    image_barrier(cmd, g.images[image_index], VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkRenderingAttachmentInfo sw_color = {0};
    sw_color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    sw_color.imageView   = g.image_views[image_index];
    sw_color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sw_color.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    sw_color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo sw_ri = {0};
    sw_ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    sw_ri.renderArea.extent    = g.extent;
    sw_ri.layerCount           = 1;
    sw_ri.colorAttachmentCount = 1;
    sw_ri.pColorAttachments    = &sw_color;
    vkCmdBeginRendering(cmd, &sw_ri);

    VkViewport sw_vp = {0, 0, (float)g.extent.width, (float)g.extent.height, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &sw_vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      g.pp_composite_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g.pp_layout, 0, 1, &g.pp_composite_set, 0, NULL);
    float comp_push[4] = {g.bloom_exposure, g.bloom_strength, 0, 0};
    vkCmdPushConstants(cmd, g.pp_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, 16, comp_push);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    /* Overlay (text/rect) on top of composite, same rendering scope. */
    if (text_verts > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.text_pipeline);
        VkDeviceSize text_offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &g.text_buffer[g.frame], &text_offset);
        float screen[2] = {(float)g.extent.width, (float)g.extent.height};
        vkCmdPushConstants(cmd, g.text_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(screen), screen);
        vkCmdDraw(cmd, text_verts, 1, 0, 0);
    }

    vkCmdEndRendering(cmd);

    image_barrier(cmd, g.images[image_index], VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    vkEndCommandBuffer(cmd);
}

/* ----------------------------------------------------------------------- */
/* public API                                                               */
/* ----------------------------------------------------------------------- */

bool render_init(window_t *window) {
    memset(&g, 0, sizeof(g));
    g.window       = window;
    g.view         = mat4_identity();
    g.proj         = mat4_identity();
    g.present_mode = VK_PRESENT_MODE_FIFO_KHR;
    g.clear_color[0] = 0.02f;
    g.clear_color[1] = 0.02f;
    g.clear_color[2] = 0.05f;
    core_resource_dir(g.shader_dir, sizeof(g.shader_dir), "KILN_SHADER_DIR",
                      "shaders", KILN_SHADER_DIR);

    g.bloom_enabled   = true;
    g.bloom_threshold = 0.8f;
    g.bloom_strength  = 0.5f;
    g.bloom_exposure  = 1.0f;
    /* Default cascade split distances (camera-to-fragment, world units). */
    g.scene_data.cascade_splits[0] = 20.0f;
    g.scene_data.cascade_splits[1] = 50.0f;
    g.scene_data.cascade_splits[2] = 200.0f;

    if (!create_instance() || !create_surface() || !pick_physical_device() ||
        !create_device() || !create_swapchain() || !create_depth_resources() ||
        !create_render_targets() ||
        !create_material_infra() || !create_scene_infra() ||
        !create_skybox_infra() ||
        !create_shadow_pipeline() ||
        !create_pipeline() || !create_inst_pipelines() ||
        !create_cull_pipeline() || !create_particle_pipeline() ||
        !create_text_pipeline() || !create_commands_and_sync()) {
        return false;
    }

    if (!create_text_buffers()) {
        return false;
    }

    g.instances = malloc(sizeof(mesh_instance_t) * MAX_INSTANCES);
    if (!g.instances) return false;

    g.inst_models = malloc(sizeof(mat4_t) * MAX_INST_TOTAL);
    if (!g.inst_models) return false;

    const uint8_t white[4] = {255, 255, 255, 255};
    g.default_texture = render_upload_texture(white, 1, 1);
    if (g.default_texture == RENDER_TEXTURE_INVALID) return false;

    const uint8_t flat_normal[4] = {128, 128, 255, 255};
    g.default_normal_texture = render_upload_texture(flat_normal, 1, 1);
    if (g.default_normal_texture == RENDER_TEXTURE_INVALID) return false;

    if (!create_pp_infra() || !create_pp_descriptors()) return false;

    return true;
}

mesh_handle_t render_upload_mesh(const cpu_mesh_t *mesh) {
    if (g.mesh_count >= MAX_MESHES || mesh->vertex_count == 0 ||
        mesh->index_count == 0) {
        fprintf(stderr, "[render] mesh upload rejected\n");
        return RENDER_MESH_INVALID;
    }

    /* Build a GPU vertex buffer: copy cpu fields, then append per-vertex
       tangents computed from triangle UV deltas (Gram-Schmidt not needed here;
       we average raw tangents and normalize). */
    gpu_vertex_t *gverts = calloc(mesh->vertex_count, sizeof(gpu_vertex_t));
    if (!gverts) return RENDER_MESH_INVALID;

    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        gverts[i].position = mesh->vertices[i].position;
        gverts[i].normal   = mesh->vertices[i].normal;
        gverts[i].uv       = mesh->vertices[i].uv;
    }

    /* Accumulate per-face tangents into each referenced vertex. */
    for (uint32_t i = 0; i + 2 < mesh->index_count; i += 3) {
        uint32_t i0 = mesh->indices[i], i1 = mesh->indices[i+1], i2 = mesh->indices[i+2];
        vec3_t e1  = vec3_sub(mesh->vertices[i1].position, mesh->vertices[i0].position);
        vec3_t e2  = vec3_sub(mesh->vertices[i2].position, mesh->vertices[i0].position);
        float du1  = mesh->vertices[i1].uv.x - mesh->vertices[i0].uv.x;
        float dv1  = mesh->vertices[i1].uv.y - mesh->vertices[i0].uv.y;
        float du2  = mesh->vertices[i2].uv.x - mesh->vertices[i0].uv.x;
        float dv2  = mesh->vertices[i2].uv.y - mesh->vertices[i0].uv.y;
        float denom = du1 * dv2 - du2 * dv1;
        if (fabsf(denom) < 1e-8f) continue;
        float f = 1.0f / denom;
        vec3_t t = {f * (dv2*e1.x - dv1*e2.x),
                    f * (dv2*e1.y - dv1*e2.y),
                    f * (dv2*e1.z - dv1*e2.z)};
        gverts[i0].tangent = vec3_add(gverts[i0].tangent, t);
        gverts[i1].tangent = vec3_add(gverts[i1].tangent, t);
        gverts[i2].tangent = vec3_add(gverts[i2].tangent, t);
    }
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        gverts[i].tangent = (vec3_length(gverts[i].tangent) > 1e-8f)
            ? vec3_normalize(gverts[i].tangent)
            : (vec3_t){1.0f, 0.0f, 0.0f};
    }

    gpu_mesh_t gm = {0};
    gm.index_count = mesh->index_count;
    bool ok = create_filled_buffer(gverts,
                                   sizeof(gpu_vertex_t) * mesh->vertex_count,
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                   &gm.vertex_buffer, &gm.vertex_memory) &&
              create_filled_buffer(mesh->indices,
                                   sizeof(uint32_t) * mesh->index_count,
                                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                   &gm.index_buffer, &gm.index_memory);
    free(gverts);
    if (!ok) return RENDER_MESH_INVALID;

    if (!cpu_mesh_bounds(mesh, &gm.bounds_min, &gm.bounds_max))
        gm.bounds_min = gm.bounds_max = (vec3_t){0, 0, 0};

    g.meshes[g.mesh_count] = gm;
    return g.mesh_count++;
}

static uint32_t calc_mip_levels(uint32_t w, uint32_t h) {
    uint32_t dim = w > h ? w : h, n = 1;
    while (dim > 1) { dim >>= 1; n++; }
    return n;
}

/* Blit-chain mipmap generation.  Mip 0 must already be in
   TRANSFER_DST_OPTIMAL when this is called.  Every mip level is
   transitioned to SHADER_READ_ONLY_OPTIMAL on exit. */
static void generate_mipmaps(VkCommandBuffer cmd, VkImage image,
                              uint32_t w, uint32_t h, uint32_t mip_count) {
    int32_t mw = (int32_t)w, mh = (int32_t)h;
    for (uint32_t i = 1; i < mip_count; i++) {
        int32_t nw = mw > 1 ? mw / 2 : 1;
        int32_t nh = mh > 1 ? mh / 2 : 1;

        image_barrier_mip(cmd, image, i - 1, 1,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        image_barrier_mip(cmd, image, i, 1,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkImageBlit blit = {0};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel   = i - 1;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1]             = (VkOffset3D){mw, mh, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel   = i;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1]             = (VkOffset3D){nw, nh, 1};
        vkCmdBlitImage(cmd,
                       image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        image_barrier_mip(cmd, image, i - 1, 1,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        mw = nw; mh = nh;
    }
    /* Transition the last level out of DST. */
    image_barrier_mip(cmd, image, mip_count - 1, 1,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

texture_handle_t render_upload_texture(const uint8_t *rgba, uint32_t w,
                                       uint32_t h) {
    if (g.texture_count >= MAX_TEXTURES || w == 0 || h == 0) {
        return RENDER_TEXTURE_INVALID;
    }

    VkDeviceSize size = (VkDeviceSize)w * h * 4;
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    if (!create_filled_buffer(rgba, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              &staging, &staging_mem)) {
        return RENDER_TEXTURE_INVALID;
    }

    uint32_t mip_count = (g.blit_mip_supported && w > 1 && h > 1)
                         ? calc_mip_levels(w, h) : 1;

    gpu_texture_t t = {0};
    VkImageCreateInfo image = {0};
    image.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType     = VK_IMAGE_TYPE_2D;
    image.format        = VK_FORMAT_R8G8B8A8_SRGB;
    image.extent.width  = w;
    image.extent.height = h;
    image.extent.depth  = 1;
    image.mipLevels     = mip_count;
    image.arrayLayers   = 1;
    image.samples       = VK_SAMPLE_COUNT_1_BIT;
    image.tiling        = VK_IMAGE_TILING_OPTIMAL;
    image.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | /* blit source for mip gen */
                          VK_IMAGE_USAGE_SAMPLED_BIT;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    texture_handle_t result = RENDER_TEXTURE_INVALID;
    if (vkCreateImage(g.device, &image, NULL, &t.image) == VK_SUCCESS) {
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(g.device, t.image, &req);
        uint32_t type_index;
        if (find_memory_type(req.memoryTypeBits,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             &type_index)) {
            VkMemoryAllocateInfo alloc = {0};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = type_index;
            if (vkAllocateMemory(g.device, &alloc, NULL, &t.memory) ==
                    VK_SUCCESS &&
                vkBindImageMemory(g.device, t.image, t.memory, 0) ==
                    VK_SUCCESS) {

                VkCommandBuffer cmd = begin_single_time();
                image_barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                              VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT);

                VkBufferImageCopy region = {0};
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.layerCount = 1;
                region.imageExtent.width = w;
                region.imageExtent.height = h;
                region.imageExtent.depth = 1;
                vkCmdCopyBufferToImage(cmd, staging, t.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &region);

                if (mip_count > 1) {
                    generate_mipmaps(cmd, t.image, w, h, mip_count);
                } else {
                    image_barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_ACCESS_SHADER_READ_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                }
                end_single_time(cmd);

                VkImageViewCreateInfo view = {0};
                view.sType        = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                view.image        = t.image;
                view.viewType     = VK_IMAGE_VIEW_TYPE_2D;
                view.format       = VK_FORMAT_R8G8B8A8_SRGB;
                view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                view.subresourceRange.levelCount = mip_count;
                view.subresourceRange.layerCount = 1;
                if (vkCreateImageView(g.device, &view, NULL, &t.view) ==
                    VK_SUCCESS) {
                    g.textures[g.texture_count] = t;
                    result = g.texture_count++;
                }
            }
        }
    }

    vkDestroyBuffer(g.device, staging, NULL);
    vkFreeMemory(g.device, staging_mem, NULL);

    if (result == RENDER_TEXTURE_INVALID) {
        if (t.view) {
            vkDestroyImageView(g.device, t.view, NULL);
        }
        if (t.image) {
            vkDestroyImage(g.device, t.image, NULL);
        }
        if (t.memory) {
            vkFreeMemory(g.device, t.memory, NULL);
        }
        fprintf(stderr, "[render] texture upload failed\n");
    }
    return result;
}

material_handle_t render_create_material(vec4_t base_color,
                                         texture_handle_t texture) {
    if (g.material_count >= MAX_MATERIALS) {
        return RENDER_MATERIAL_INVALID;
    }
    if (texture >= g.texture_count) {
        texture = g.default_texture; /* flat colour: sample white */
    }

    gpu_material_t m = {0};
    material_ubo_t data = {{base_color.x, base_color.y, base_color.z,
                            base_color.w}};
    if (!create_filled_buffer(&data, sizeof(data),
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &m.ubo,
                              &m.ubo_memory)) {
        return RENDER_MATERIAL_INVALID;
    }

    VkDescriptorSetAllocateInfo alloc = {0};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = g.descriptor_pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &g.material_set_layout;
    if (vkAllocateDescriptorSets(g.device, &alloc, &m.set) != VK_SUCCESS) {
        vkDestroyBuffer(g.device, m.ubo, NULL);
        vkFreeMemory(g.device, m.ubo_memory, NULL);
        return RENDER_MATERIAL_INVALID;
    }

    VkDescriptorBufferInfo buffer_info = {0};
    buffer_info.buffer = m.ubo;
    buffer_info.range  = sizeof(data);

    VkDescriptorImageInfo albedo_info = {0};
    albedo_info.sampler     = g.sampler;
    albedo_info.imageView   = g.textures[texture].view;
    albedo_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo normal_info = {0};
    normal_info.sampler     = g.sampler;
    normal_info.imageView   = g.textures[g.default_normal_texture].view;
    normal_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[3] = {0};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m.set;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo     = &buffer_info;
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m.set;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo      = &albedo_info;
    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = m.set;
    writes[2].dstBinding      = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo      = &normal_info;
    vkUpdateDescriptorSets(g.device, 3, writes, 0, NULL);

    g.materials[g.material_count] = m;
    return g.material_count++;
}

void render_set_material_normal_map(material_handle_t mat,
                                    texture_handle_t  normal_map) {
    if (mat >= g.material_count) return;
    if (normal_map >= g.texture_count)
        normal_map = g.default_normal_texture;

    VkDescriptorImageInfo info = {0};
    info.sampler     = g.sampler;
    info.imageView   = g.textures[normal_map].view;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write = {0};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = g.materials[mat].set;
    write.dstBinding      = 2;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &info;
    vkUpdateDescriptorSets(g.device, 1, &write, 0, NULL);
}

cubemap_handle_t render_upload_cubemap(const uint8_t *faces[6],
                                       uint32_t w, uint32_t h) {
    if (g.cubemap_count >= MAX_CUBEMAPS || w == 0 || h == 0) {
        fprintf(stderr, "[render] cubemap upload rejected\n");
        return RENDER_CUBEMAP_INVALID;
    }

    VkDeviceSize face_size = (VkDeviceSize)w * h * 4;
    VkDeviceSize total     = face_size * 6;
    VkBuffer     staging; VkDeviceMemory staging_mem;
    if (!create_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &staging, &staging_mem))
        return RENDER_CUBEMAP_INVALID;

    void *mapped;
    vkMapMemory(g.device, staging_mem, 0, total, 0, &mapped);
    for (int i = 0; i < 6; i++)
        memcpy((uint8_t *)mapped + i * face_size, faces[i], (size_t)face_size);
    vkUnmapMemory(g.device, staging_mem);

    VkImageCreateInfo ici = {0};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_R8G8B8A8_SRGB;
    ici.extent        = (VkExtent3D){w, h, 1};
    ici.mipLevels     = 1;
    ici.arrayLayers   = 6;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    uint32_t idx = g.cubemap_count;
    VkImage img; VkDeviceMemory mem;
    if (vkCreateImage(g.device, &ici, NULL, &img) != VK_SUCCESS) goto fail;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.device, img, &req);
    uint32_t mtype;
    if (!find_memory_type(req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mtype))
        goto fail_img;
    VkMemoryAllocateInfo mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size; mai.memoryTypeIndex = mtype;
    if (vkAllocateMemory(g.device, &mai, NULL, &mem) != VK_SUCCESS)
        goto fail_img;
    vkBindImageMemory(g.device, img, mem, 0);

    {
        VkCommandBuffer cmd = begin_single_time();
        /* Transition all 6 layers to TRANSFER_DST */
        VkImageMemoryBarrier bar = {0};
        bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image               = img;
        bar.subresourceRange    = (VkImageSubresourceRange){
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        bar.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &bar);

        VkBufferImageCopy regions[6];
        for (int f = 0; f < 6; f++) {
            regions[f] = (VkBufferImageCopy){
                .bufferOffset = (VkDeviceSize)f * face_size,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)f, 1},
                .imageExtent      = {w, h, 1},
            };
        }
        vkCmdCopyBufferToImage(cmd, staging, img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, regions);

        bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, NULL, 0, NULL, 1, &bar);
        end_single_time(cmd);
    }

    VkImageViewCreateInfo vci = {0};
    vci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image                           = img;
    vci.viewType                        = VK_IMAGE_VIEW_TYPE_CUBE;
    vci.format                          = VK_FORMAT_R8G8B8A8_SRGB;
    vci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount     = 1;
    vci.subresourceRange.layerCount     = 6;
    VkImageView view;
    if (vkCreateImageView(g.device, &vci, NULL, &view) != VK_SUCCESS)
        goto fail_mem;

    /* Allocate and write a descriptor set for this cubemap. */
    VkDescriptorSetAllocateInfo dsai = {0};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = g.skybox_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &g.skybox_set_layout;
    if (vkAllocateDescriptorSets(g.device, &dsai, &g.skybox_sets[idx]) != VK_SUCCESS)
        goto fail_view;

    VkDescriptorImageInfo dii = {g.skybox_sampler, view,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wds = {0};
    wds.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet          = g.skybox_sets[idx];
    wds.dstBinding      = 0;
    wds.descriptorCount = 1;
    wds.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo      = &dii;
    vkUpdateDescriptorSets(g.device, 1, &wds, 0, NULL);

    g.cubemaps[idx].image  = img;
    g.cubemaps[idx].memory = mem;
    g.cubemaps[idx].view   = view;
    vkDestroyBuffer(g.device, staging, NULL);
    vkFreeMemory(g.device, staging_mem, NULL);
    return g.cubemap_count++;

fail_view: vkDestroyImageView(g.device, view, NULL);
fail_mem:  vkFreeMemory(g.device, mem, NULL);
fail_img:  vkDestroyImage(g.device, img, NULL);
fail:
    vkDestroyBuffer(g.device, staging, NULL);
    vkFreeMemory(g.device, staging_mem, NULL);
    return RENDER_CUBEMAP_INVALID;
}

void render_set_skybox(cubemap_handle_t cubemap) {
    g.active_skybox = cubemap;
}


void render_draw(void) {
    /* Snapshot and clear the queues up front so they never accumulate across
       an early-return frame (e.g. resize). */
    uint32_t instance_count = g.instance_count;
    g.instance_count = 0;
    g.point_light_count = 0;
    uint32_t inst_batch_count = g.inst_batch_count;
    uint32_t inst_model_count = g.inst_model_count;
    g.inst_batch_count = 0;
    g.inst_model_count = 0;
    uint32_t text_verts = g.text_vert_count;
    g.text_vert_count = 0;
    uint32_t particle_cmd_count = g.particle_cmd_count;
    g.particle_cmd_count = 0;

    if (g.extent.width == 0 || g.extent.height == 0) {
        recreate_swapchain();
        return;
    }
    /* Proactively recreate when the window size changed (Wine does not
       reliably return VK_SUBOPTIMAL_KHR/OUT_OF_DATE on external resize). */
    {
        uint32_t pw, ph;
        window_size(g.window, &pw, &ph);
        if (pw != g.extent.width || ph != g.extent.height) {
            recreate_swapchain();
            return;
        }
    }
    if (g.vsync_dirty) {
        g.vsync_dirty = false;
        recreate_swapchain();
        return;
    }

    /* Screenshot: device-idle readback from color_image before this frame. */
    if (g.screenshot_path[0] && g.color_view) {
        vkDeviceWaitIdle(g.device);
        uint32_t w = g.extent.width, h = g.extent.height;
        VkDeviceSize sz = (VkDeviceSize)w * h * 8; /* RGBA16F = 8 bytes/px */
        VkBuffer sb; VkDeviceMemory sm;
        if (create_buffer(sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &sb, &sm)) {
            VkCommandBuffer sc = begin_single_time();
            image_barrier(sc, g.color_image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy bic = {0};
            bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bic.imageSubresource.layerCount = 1;
            bic.imageExtent = (VkExtent3D){w, h, 1};
            vkCmdCopyImageToBuffer(sc, g.color_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   sb, 1, &bic);
            image_barrier(sc, g.color_image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            end_single_time(sc);

            void *mapped;
            vkMapMemory(g.device, sm, 0, sz, 0, &mapped);
            FILE *f = fopen(g.screenshot_path, "wb");
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", w, h);
                const uint16_t *px = mapped;
                for (uint32_t i = 0; i < w * h; i++) {
                    /* Half-float channels → uint8 with simple linear clamp */
                    for (int c = 0; c < 3; c++) {
                        uint16_t h16 = px[i * 4 + c];
                        uint32_t exp = (h16 >> 10) & 0x1F;
                        uint32_t man = h16 & 0x3FF;
                        float v = 0.0f;
                        if (exp == 0) v = ldexpf((float)man, -24);
                        else if (exp < 31) v = ldexpf((float)(man | 0x400), (int)exp - 25);
                        uint8_t byte = (uint8_t)(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f + 0.5f);
                        fputc(byte, f);
                    }
                }
                fclose(f);
                fprintf(stderr, "[render] screenshot → %s\n", g.screenshot_path);
            }
            vkUnmapMemory(g.device, sm);
            vkDestroyBuffer(g.device, sb, NULL);
            vkFreeMemory(g.device, sm, NULL);
        }
        g.screenshot_path[0] = '\0';
    }

    vkWaitForFences(g.device, 1, &g.in_flight[g.frame], VK_TRUE, UINT64_MAX);

    /* Fence guarantees frame N-2's GPU work is done.  Safe to write per-frame
       CPU-side buffers (text, particle emit staging, indirect reset). */
    if (text_verts > 0) {
        memcpy(g.text_mapped[g.frame], g.text_verts,
               sizeof(text_vertex_t) * text_verts);
    }

    for (uint32_t pi = 0; pi < particle_cmd_count; pi++) {
        gpu_emitter_t *e = &g.gpu_emitters[g.particle_cmds[pi].emitter_idx];
        uint32_t n = e->emit_pending_count;
        if (n > MAX_EMIT_PER_FRAME) n = MAX_EMIT_PER_FRAME;
        e->emit_ring_head[g.frame] = e->ring_head;
        if (n > 0) {
            memcpy(e->emit_mapped[g.frame], e->emit_pending,
                   sizeof(gpu_particle_t) * n);
            e->ring_head = (e->ring_head + n) % e->capacity;
            e->emit_pending_count = 0;
        }
        e->emit_count[g.frame] = n;
        /* Reset instance_count to 0 so the simulate pass starts fresh. */
        ((VkDrawIndexedIndirectCommand *)e->indirect_mapped[g.frame])->instanceCount = 0;
    }

    /* Build cull input buffers for this frame's instanced batches. */
    if (inst_batch_count > 0) {
        cull_inst_t  *ci  = g.cull_inst_mapped[g.frame];
        cull_batch_t *cb  = g.cull_batch_mapped[g.frame];
        VkDrawIndexedIndirectCommand *ic = g.cull_indirect_mapped[g.frame];
        uint32_t write = 0;
        for (uint32_t b = 0; b < inst_batch_count; b++) {
            const inst_batch_t *batch = &g.inst_batches[b];
            const gpu_mesh_t   *mesh  = &g.meshes[batch->mesh];
            /* Per-batch descriptor. */
            cb[b].bounds_min[0] = mesh->bounds_min.x;
            cb[b].bounds_min[1] = mesh->bounds_min.y;
            cb[b].bounds_min[2] = mesh->bounds_min.z;
            cb[b].first         = batch->first;
            cb[b].bounds_max[0] = mesh->bounds_max.x;
            cb[b].bounds_max[1] = mesh->bounds_max.y;
            cb[b].bounds_max[2] = mesh->bounds_max.z;
            cb[b].count         = batch->count;
            /* Indirect args: CPU sets everything except instanceCount. */
            ic[b].indexCount    = mesh->index_count;
            ic[b].instanceCount = 0;   /* ← compute fills this */
            ic[b].firstIndex    = 0;
            ic[b].vertexOffset  = 0;
            ic[b].firstInstance = batch->first; /* VBO reads cull_out_buf[first..] */
            /* Per-instance data. */
            for (uint32_t i = 0; i < batch->count; i++, write++) {
                ci[write].model     = g.inst_models[batch->first + i];
                ci[write].batch_idx = b;
                ci[write]._pad[0] = ci[write]._pad[1] = ci[write]._pad[2] = 0;
            }
        }
        (void)write;
    }

    uint32_t image_index;
    VkResult acquire = vkAcquireNextImageKHR(
        g.device, g.swapchain, UINT64_MAX, g.image_available[g.frame],
        VK_NULL_HANDLE, &image_index);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "[render] acquire failed: %d\n", acquire);
        return;
    }

    vkResetFences(g.device, 1, &g.in_flight[g.frame]);

    VkCommandBuffer cmd = g.command_buffers[g.frame];
    vkResetCommandBuffer(cmd, 0);
    record_command_buffer(cmd, image_index, instance_count,
                          inst_batch_count, inst_model_count, text_verts,
                          particle_cmd_count);

    VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &g.image_available[g.frame];
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &g.render_finished[image_index];
    vkQueueSubmit(g.graphics_queue, 1, &submit, g.in_flight[g.frame]);

    VkPresentInfoKHR present = {0};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &g.render_finished[image_index];
    present.swapchainCount = 1;
    present.pSwapchains = &g.swapchain;
    present.pImageIndices = &image_index;
    VkResult result = vkQueuePresentKHR(g.present_queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain();
    } else if (result != VK_SUCCESS) {
        fprintf(stderr, "[render] present failed: %d\n", result);
    }

    g.frame = (g.frame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void render_set_wireframe(bool enabled) {
    g.wireframe = enabled && g.wireframe_supported;
}

bool render_get_wireframe(void) { return g.wireframe; }

void render_set_vsync(bool enabled) {
    VkPresentModeKHR mode = choose_present_mode(enabled);
    if (mode == g.present_mode) return;
    g.present_mode = mode;
    g.vsync_dirty  = true;
}

bool render_get_vsync(void) {
    return g.present_mode == VK_PRESENT_MODE_FIFO_KHR ||
           g.present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
}

void render_shutdown(void) {
    if (!g.device) {
        return;
    }
    vkDeviceWaitIdle(g.device);

    for (uint32_t i = 0; i < g.mesh_count; i++) {
        vkDestroyBuffer(g.device, g.meshes[i].index_buffer, NULL);
        vkFreeMemory(g.device, g.meshes[i].index_memory, NULL);
        vkDestroyBuffer(g.device, g.meshes[i].vertex_buffer, NULL);
        vkFreeMemory(g.device, g.meshes[i].vertex_memory, NULL);
    }

    /* Material descriptor sets are freed wholesale with the pool. */
    for (uint32_t i = 0; i < g.material_count; i++) {
        vkDestroyBuffer(g.device, g.materials[i].ubo, NULL);
        vkFreeMemory(g.device, g.materials[i].ubo_memory, NULL);
    }
    for (uint32_t i = 0; i < g.texture_count; i++) {
        vkDestroyImageView(g.device, g.textures[i].view, NULL);
        vkDestroyImage(g.device, g.textures[i].image, NULL);
        vkFreeMemory(g.device, g.textures[i].memory, NULL);
    }
    vkDestroySampler(g.device, g.sampler, NULL);
    vkDestroyDescriptorPool(g.device, g.descriptor_pool, NULL);
    vkDestroyDescriptorSetLayout(g.device, g.material_set_layout, NULL);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroyBuffer(g.device, g.scene_ubo_buf[i], NULL);
        vkFreeMemory(g.device, g.scene_ubo_mem[i], NULL);
    }
    vkDestroyDescriptorPool(g.device, g.scene_pool, NULL);
    vkDestroyDescriptorSetLayout(g.device, g.scene_set_layout, NULL);

    vkDestroyPipeline(g.device, g.skybox_pipeline, NULL);
    vkDestroyPipelineLayout(g.device, g.skybox_layout, NULL);
    vkDestroyDescriptorPool(g.device, g.skybox_pool, NULL);
    vkDestroyDescriptorSetLayout(g.device, g.skybox_set_layout, NULL);
    vkDestroySampler(g.device, g.skybox_sampler, NULL);
    for (uint32_t i = 0; i < g.cubemap_count; i++) {
        vkDestroyImageView(g.device, g.cubemaps[i].view, NULL);
        vkDestroyImage(g.device, g.cubemaps[i].image, NULL);
        vkFreeMemory(g.device, g.cubemaps[i].memory, NULL);
    }

    vkDestroyPipeline(g.device, g.pp_composite_pipeline, NULL);
    vkDestroyPipeline(g.device, g.pp_blur_pipeline, NULL);
    vkDestroyPipeline(g.device, g.pp_threshold_pipeline, NULL);
    vkDestroyPipelineLayout(g.device, g.pp_layout, NULL);
    vkDestroyDescriptorPool(g.device, g.pp_pool, NULL);
    vkDestroyDescriptorSetLayout(g.device, g.pp_set_layout, NULL);
    vkDestroySampler(g.device, g.pp_sampler, NULL);

    vkDestroyPipeline(g.device, g.shadow_pipeline, NULL);
    vkDestroyPipelineLayout(g.device, g.shadow_layout, NULL);
    vkDestroySampler(g.device, g.shadow_sampler, NULL);
    for (uint32_t i = 0; i < 3; i++)
        if (g.shadow_layer_view[i])
            vkDestroyImageView(g.device, g.shadow_layer_view[i], NULL);
    vkDestroyImageView(g.device, g.shadow_view, NULL);
    vkDestroyImage(g.device, g.shadow_image, NULL);
    vkFreeMemory(g.device, g.shadow_memory, NULL);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroyBuffer(g.device, g.text_buffer[i], NULL);
        vkFreeMemory(g.device, g.text_memory[i], NULL);
    }
    free(g.text_verts);
    free(g.instances);
    free(g.inst_models);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroyBuffer(g.device, g.inst_vbuf[i], NULL);
        vkFreeMemory(g.device, g.inst_vmem[i], NULL);
    }
    if (g.inst_wireframe_pipeline)
        vkDestroyPipeline(g.device, g.inst_wireframe_pipeline, NULL);
    vkDestroyPipeline(g.device, g.inst_pipeline, NULL);
    vkDestroyPipeline(g.device, g.shadow_inst_pipeline, NULL);
    vkDestroyPipelineLayout(g.device, g.inst_layout, NULL);

    /* GPU-driven cull infrastructure. */
    for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
        if (g.cull_inst_mapped[f])     { vkUnmapMemory(g.device, g.cull_inst_mem[f]); }
        if (g.cull_inst_buf[f])        { vkDestroyBuffer(g.device, g.cull_inst_buf[f], NULL); }
        if (g.cull_inst_mem[f])        { vkFreeMemory(g.device, g.cull_inst_mem[f], NULL); }
        if (g.cull_batch_mapped[f])    { vkUnmapMemory(g.device, g.cull_batch_mem[f]); }
        if (g.cull_batch_buf[f])       { vkDestroyBuffer(g.device, g.cull_batch_buf[f], NULL); }
        if (g.cull_batch_mem[f])       { vkFreeMemory(g.device, g.cull_batch_mem[f], NULL); }
        if (g.cull_out_buf[f])         { vkDestroyBuffer(g.device, g.cull_out_buf[f], NULL); }
        if (g.cull_out_mem[f])         { vkFreeMemory(g.device, g.cull_out_mem[f], NULL); }
        if (g.cull_indirect_mapped[f]) { vkUnmapMemory(g.device, g.cull_indirect_mem[f]); }
        if (g.cull_indirect_buf[f])    { vkDestroyBuffer(g.device, g.cull_indirect_buf[f], NULL); }
        if (g.cull_indirect_mem[f])    { vkFreeMemory(g.device, g.cull_indirect_mem[f], NULL); }
    }
    if (g.cull_pipeline)    vkDestroyPipeline(g.device, g.cull_pipeline, NULL);
    if (g.cull_layout)      vkDestroyPipelineLayout(g.device, g.cull_layout, NULL);
    if (g.cull_pool)        vkDestroyDescriptorPool(g.device, g.cull_pool, NULL);
    if (g.cull_set_layout)  vkDestroyDescriptorSetLayout(g.device, g.cull_set_layout, NULL);

    /* GPU particle emitters. */
    for (uint32_t i = 0; i < g.gpu_emitter_count; i++)
        if (g.gpu_emitters[i].active)
            destroy_gpu_emitter_resources(&g.gpu_emitters[i]);
    if (g.particle_pipeline) vkDestroyPipeline(g.device, g.particle_pipeline, NULL);
    if (g.particle_layout)   vkDestroyPipelineLayout(g.device, g.particle_layout, NULL);
    if (g.particle_pool)     vkDestroyDescriptorPool(g.device, g.particle_pool, NULL);
    if (g.particle_set_layout) vkDestroyDescriptorSetLayout(g.device, g.particle_set_layout, NULL);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(g.device, g.image_available[i], NULL);
        vkDestroyFence(g.device, g.in_flight[i], NULL);
    }
    vkDestroyCommandPool(g.device, g.command_pool, NULL);

    vkDestroyPipeline(g.device, g.text_pipeline, NULL);
    vkDestroyPipelineLayout(g.device, g.text_layout, NULL);
    if (g.wireframe_pipeline)
        vkDestroyPipeline(g.device, g.wireframe_pipeline, NULL);
    vkDestroyPipeline(g.device, g.pipeline, NULL);
    vkDestroyPipelineLayout(g.device, g.pipeline_layout, NULL);

    destroy_swapchain();

    vkDestroyDevice(g.device, NULL);
    if (g.debug_messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy_fn =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                g.instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy_fn) {
            destroy_fn(g.instance, g.debug_messenger, NULL);
        }
    }
    vkDestroySurfaceKHR(g.instance, g.surface, NULL);
    vkDestroyInstance(g.instance, NULL);
    memset(&g, 0, sizeof(g));
}
