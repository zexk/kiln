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
#define SHADOW_MAP_SIZE 2048

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

/* Per-frame scene uniform.  All vec4 for trivial std140; light_vp is a full
   mat4 (64 bytes) appended after the four vec4s, total 128 bytes. */
typedef struct {
    float light_dir[4];     /* xyz: unit direction toward the key light */
    float light_color[4];   /* xyz: RGB intensity of the key light */
    float ambient_color[4]; /* xyz: ambient fill RGB */
    float view_pos[4];      /* xyz: camera world-space position */
    float light_vp[16];     /* mat4: light view-projection for shadow mapping */
} scene_ubo_t;

/* One queued draw: which mesh + material, plus the matrices the shader needs. */
typedef struct {
    mesh_handle_t mesh;
    material_handle_t material;
    mat4_t mvp;   /* proj * view * model, for clip-space position */
    mat4_t model; /* for transforming the normal into world space */
} mesh_instance_t;

/* Vertex-stage push constants for the mesh pipeline (128 bytes = the minimum
   guaranteed maxPushConstantsSize, so this stays portable). */
typedef struct {
    mat4_t mvp;
    mat4_t model;
} mesh_push_t;

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
    VkImageView      shadow_view;
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
    texture_handle_t default_texture; /* 1x1 white, for untextured materials */
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

    VkPresentModeKHR present_mode; /* desired mode; applied on next swapchain create */
    bool vsync_dirty;              /* triggers swapchain recreation in render_draw */
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
    if (caps->currentExtent.width != UINT32_MAX) {
        return caps->currentExtent;
    }
    uint32_t w, h;
    window_size(g.window, &w, &h);
    VkExtent2D e = {w, h};
    if (e.width < caps->minImageExtent.width) {
        e.width = caps->minImageExtent.width;
    }
    if (e.height < caps->minImageExtent.height) {
        e.height = caps->minImageExtent.height;
    }
    if (e.width > caps->maxImageExtent.width) {
        e.width = caps->maxImageExtent.width;
    }
    if (e.height > caps->maxImageExtent.height) {
        e.height = caps->maxImageExtent.height;
    }
    return e;
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
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
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
/* materials: descriptor layout, pool, sampler                              */
/* ----------------------------------------------------------------------- */

/* Each material owns a descriptor set with this layout: binding 0 a uniform
   buffer (base colour), binding 1 a combined image sampler (albedo). Created
   before the pipeline, which references the set layout. */
static bool create_material_infra(void) {
    VkDescriptorSetLayoutBinding bindings[2] = {0};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout = {0};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = 2;
    layout.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(g.device, &layout, NULL,
                                         &g.material_set_layout));

    VkDescriptorPoolSize sizes[2] = {0};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = MAX_MATERIALS;
    sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = MAX_MATERIALS;

    VkDescriptorPoolCreateInfo pool = {0};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = MAX_MATERIALS;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
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

    /* --- shadow map image + view --- */
    VkImageCreateInfo shadow_img = {0};
    shadow_img.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    shadow_img.imageType     = VK_IMAGE_TYPE_2D;
    shadow_img.format        = VK_FORMAT_D32_SFLOAT;
    shadow_img.extent        = (VkExtent3D){SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 1};
    shadow_img.mipLevels     = 1;
    shadow_img.arrayLayers   = 1;
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
                          &stype)) {
        return false;
    }
    VkMemoryAllocateInfo salloc = {0};
    salloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    salloc.allocationSize  = sreq.size;
    salloc.memoryTypeIndex = stype;
    VK_CHECK(vkAllocateMemory(g.device, &salloc, NULL, &g.shadow_memory));
    VK_CHECK(vkBindImageMemory(g.device, g.shadow_image, g.shadow_memory, 0));

    VkImageViewCreateInfo svci = {0};
    svci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    svci.image                           = g.shadow_image;
    svci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    svci.format                          = VK_FORMAT_D32_SFLOAT;
    svci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
    svci.subresourceRange.levelCount     = 1;
    svci.subresourceRange.layerCount     = 1;
    VK_CHECK(vkCreateImageView(g.device, &svci, NULL, &g.shadow_view));

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
    VkVertexInputBindingDescription vbind = {0, sizeof(mesh_vertex_t), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vattr = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                               offsetof(mesh_vertex_t, position)};
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
    binding.stride    = sizeof(mesh_vertex_t);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3] = {0};
    attrs[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(mesh_vertex_t, position)};
    attrs[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(mesh_vertex_t, normal)};
    attrs[2] = (VkVertexInputAttributeDescription){2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(mesh_vertex_t, uv)};

    VkPipelineVertexInputStateCreateInfo vertex_input = {0};
    vertex_input.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 1;
    vertex_input.pVertexBindingDescriptions      = &binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
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

    /* Must declare the same attachment formats as the cube pipeline even
       though depth is unused, or the formats mismatch the render scope. */
    VkPipelineRenderingCreateInfo rendering = {0};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &g.swapchain_format;
    rendering.depthAttachmentFormat = g.depth_format;

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
        !create_render_finished_semaphores()) {
        return false;
    }
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

static void record_command_buffer(VkCommandBuffer cmd, uint32_t image_index,
                                  uint32_t instance_count,
                                  uint32_t text_verts) {
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

    /* ------------------------------------------------------------------ */
    /* Shadow pass: render scene depth from the light's perspective.        */
    /* ------------------------------------------------------------------ */

    /* Compute light view-projection (ortho, centered at camera position). */
    vec3_t view_pos  = {g.scene_data.view_pos[0],
                        g.scene_data.view_pos[1],
                        g.scene_data.view_pos[2]};
    vec3_t ld = {g.scene_data.light_dir[0],
                 g.scene_data.light_dir[1],
                 g.scene_data.light_dir[2]};
    vec3_t light_eye = vec3_add(view_pos, vec3_scale(ld, 50.0f));
    vec3_t world_up  = (fabsf(ld.y) > 0.99f)
                       ? (vec3_t){0.0f, 0.0f, 1.0f}
                       : (vec3_t){0.0f, 1.0f, 0.0f};
    mat4_t light_view = mat4_look_at(light_eye, view_pos, world_up);
    float  sz         = 40.0f;
    mat4_t light_proj = mat4_ortho(-sz, sz, -sz, sz, 1.0f, 200.0f);
    mat4_t light_vp   = mat4_mul(light_proj, light_view);
    memcpy(g.scene_data.light_vp, light_vp.m, sizeof(light_vp.m));

    image_barrier(cmd, g.shadow_image, VK_IMAGE_ASPECT_DEPTH_BIT,
                  VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, 0,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

    VkRenderingAttachmentInfo shadow_depth = {0};
    shadow_depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    shadow_depth.imageView   = g.shadow_view;
    shadow_depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    shadow_depth.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    shadow_depth.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    shadow_depth.clearValue.depthStencil.depth = 1.0f;

    VkRenderingInfo shadow_pass = {0};
    shadow_pass.sType             = VK_STRUCTURE_TYPE_RENDERING_INFO;
    shadow_pass.renderArea.extent = (VkExtent2D){SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
    shadow_pass.layerCount        = 1;
    shadow_pass.pDepthAttachment  = &shadow_depth;
    vkCmdBeginRendering(cmd, &shadow_pass);

    VkViewport shadow_vp = {0.0f, 0.0f,
                            (float)SHADOW_MAP_SIZE, (float)SHADOW_MAP_SIZE,
                            0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &shadow_vp);
    VkRect2D shadow_scissor = {{0, 0}, {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE}};
    vkCmdSetScissor(cmd, 0, 1, &shadow_scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.shadow_pipeline);

    for (uint32_t i = 0; i < instance_count; i++) {
        const mesh_instance_t *inst = &g.instances[i];
        const gpu_mesh_t *mesh      = &g.meshes[inst->mesh];
        mat4_t light_mvp = mat4_mul(light_vp, inst->model);
        vkCmdPushConstants(cmd, g.shadow_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(mat4_t), &light_mvp);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertex_buffer, &offset);
        vkCmdBindIndexBuffer(cmd, mesh->index_buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, mesh->index_count, 1, 0, 0, 0);
    }
    vkCmdEndRendering(cmd);

    /* Transition shadow map to shader-readable before the color pass. */
    image_barrier(cmd, g.shadow_image, VK_IMAGE_ASPECT_DEPTH_BIT,
                  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    /* ------------------------------------------------------------------ */
    /* Color pass                                                           */
    /* ------------------------------------------------------------------ */

    image_barrier(cmd, g.images[image_index], VK_IMAGE_ASPECT_COLOR_BIT,
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
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = g.image_views[image_index];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color.float32[0] = g.clear_color[0];
    color.clearValue.color.float32[1] = g.clear_color[1];
    color.clearValue.color.float32[2] = g.clear_color[2];
    color.clearValue.color.float32[3] = 1.0f;

    VkRenderingAttachmentInfo depth = {0};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = g.depth_view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil.depth = 1.0f;

    VkRenderingInfo rendering = {0};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = g.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &rendering);

    /* Negative-height viewport flips Y so the linalg projection (which leaves
       Y unflipped, per its contract) renders right-side-up under Vulkan. */
    VkViewport viewport = {0};
    viewport.x = 0.0f;
    viewport.y = (float)g.extent.height;
    viewport.width = (float)g.extent.width;
    viewport.height = -(float)g.extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {0};
    scissor.extent = g.extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkPipeline mesh_pipe = (g.wireframe && g.wireframe_pipeline)
                           ? g.wireframe_pipeline : g.pipeline;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipe);

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
        vkCmdPushConstants(cmd, g.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(push), &push);
        vkCmdDrawIndexed(cmd, mesh->index_count, 1, 0, 0, 0);
    }

    /* Debug text overlay: a positive-height viewport (top-left origin, +y
       down) so it draws upright regardless of the cube's flipped viewport. */
    if (text_verts > 0) {
        VkViewport text_viewport = {0};
        text_viewport.x = 0.0f;
        text_viewport.y = 0.0f;
        text_viewport.width = (float)g.extent.width;
        text_viewport.height = (float)g.extent.height;
        text_viewport.minDepth = 0.0f;
        text_viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &text_viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          g.text_pipeline);
        VkDeviceSize text_offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &g.text_buffer[g.frame],
                               &text_offset);
        float screen[2] = {(float)g.extent.width, (float)g.extent.height};
        vkCmdPushConstants(cmd, g.text_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(screen), screen);
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

    if (!create_instance() || !create_surface() || !pick_physical_device() ||
        !create_device() || !create_swapchain() || !create_depth_resources() ||
        !create_material_infra() || !create_scene_infra() ||
        !create_shadow_pipeline() ||
        !create_pipeline() || !create_text_pipeline() ||
        !create_commands_and_sync()) {
        return false;
    }

    if (!create_text_buffers()) {
        return false;
    }

    g.instances = malloc(sizeof(mesh_instance_t) * MAX_INSTANCES);
    if (!g.instances) {
        return false;
    }

    /* Built-in 1x1 white texture, substituted for untextured materials so the
       shader always has something to sample. Needs the command pool, hence
       after create_commands_and_sync. */
    const uint8_t white[4] = {255, 255, 255, 255};
    g.default_texture = render_upload_texture(white, 1, 1);
    if (g.default_texture == RENDER_TEXTURE_INVALID) {
        return false;
    }
    return true;
}

mesh_handle_t render_upload_mesh(const cpu_mesh_t *mesh) {
    if (g.mesh_count >= MAX_MESHES || mesh->vertex_count == 0 ||
        mesh->index_count == 0) {
        fprintf(stderr, "[render] mesh upload rejected\n");
        return RENDER_MESH_INVALID;
    }

    gpu_mesh_t gm = {0};
    gm.index_count = mesh->index_count;
    if (!create_filled_buffer(mesh->vertices,
                              sizeof(mesh_vertex_t) * mesh->vertex_count,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              &gm.vertex_buffer, &gm.vertex_memory) ||
        !create_filled_buffer(mesh->indices,
                              sizeof(uint32_t) * mesh->index_count,
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &gm.index_buffer,
                              &gm.index_memory)) {
        return RENDER_MESH_INVALID;
    }

    if (!cpu_mesh_bounds(mesh, &gm.bounds_min, &gm.bounds_max)) {
        /* degenerate mesh: treat as a single point at the origin */
        gm.bounds_min = gm.bounds_max = (vec3_t){0, 0, 0};
    }

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
    buffer_info.range = sizeof(data);

    VkDescriptorImageInfo image_info = {0};
    image_info.sampler = g.sampler;
    image_info.imageView = g.textures[texture].view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2] = {0};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m.set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &buffer_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m.set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &image_info;
    vkUpdateDescriptorSets(g.device, 2, writes, 0, NULL);

    g.materials[g.material_count] = m;
    return g.material_count++;
}

void render_draw(void) {
    /* Snapshot and clear the queues up front so they never accumulate across
       an early-return frame (e.g. resize). */
    uint32_t instance_count = g.instance_count;
    g.instance_count = 0;
    uint32_t text_verts = g.text_vert_count;
    g.text_vert_count = 0;

    if (g.extent.width == 0 || g.extent.height == 0) {
        recreate_swapchain();
        return;
    }
    if (g.vsync_dirty) {
        g.vsync_dirty = false;
        recreate_swapchain();
        return;
    }

    vkWaitForFences(g.device, 1, &g.in_flight[g.frame], VK_TRUE, UINT64_MAX);

    /* Safe to write this frame's text buffer now the fence guarantees the GPU
       has finished its previous use. */
    if (text_verts > 0) {
        memcpy(g.text_mapped[g.frame], g.text_verts,
               sizeof(text_vertex_t) * text_verts);
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
    record_command_buffer(cmd, image_index, instance_count, text_verts);

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

    vkDestroyPipeline(g.device, g.shadow_pipeline, NULL);
    vkDestroyPipelineLayout(g.device, g.shadow_layout, NULL);
    vkDestroySampler(g.device, g.shadow_sampler, NULL);
    vkDestroyImageView(g.device, g.shadow_view, NULL);
    vkDestroyImage(g.device, g.shadow_image, NULL);
    vkFreeMemory(g.device, g.shadow_memory, NULL);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroyBuffer(g.device, g.text_buffer[i], NULL);
        vkFreeMemory(g.device, g.text_memory[i], NULL);
    }
    free(g.text_verts);
    free(g.instances);

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
