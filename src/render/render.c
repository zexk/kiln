#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <X11/Xlib.h>

#include "render.h"
#include "linalg.h"
#include "font8x8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES_IN_FLIGHT 2

/* Debug-text vertex budget per frame (each lit font pixel costs 6 verts). */
#define MAX_TEXT_VERTS (64 * 1024)

/* Per-frame cube instance budget; each costs one indexed draw + push constant. */
#define MAX_CUBES 4096

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
    float pos[3];
    float color[3];
} vertex_t;

typedef struct {
    float pos[2]; /* screen pixels */
    float color[3];
} text_vertex_t;

/* Unit cube centred on the origin; vertex colour derived from corner. */
static const vertex_t k_cube_vertices[] = {
    {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 0.0f}},
    {{-0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}},
    {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 1.0f}},
    {{0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}},
    {{-0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 1.0f}},
};

static const uint16_t k_cube_indices[] = {
    0, 1, 2, 2, 3, 0, /* -Z */
    4, 5, 6, 6, 7, 4, /* +Z */
    0, 3, 7, 7, 4, 0, /* -X */
    1, 2, 6, 6, 5, 1, /* +X */
    0, 1, 5, 5, 4, 0, /* -Y */
    3, 2, 6, 6, 7, 3, /* +Y */
};

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

    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;

    VkCommandPool command_pool;
    VkCommandBuffer command_buffers[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore image_available[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore *render_finished; /* one per swapchain image */
    VkFence in_flight[MAX_FRAMES_IN_FLIGHT];
    uint32_t frame;

    VkBuffer vertex_buffer;
    VkDeviceMemory vertex_memory;
    VkBuffer index_buffer;
    VkDeviceMemory index_memory;

    /* Debug text: a screen-space 2D pipeline with per-frame-in-flight vertex
       buffers (persistently mapped) fed from a CPU staging array. */
    VkPipelineLayout text_layout;
    VkPipeline text_pipeline;
    VkBuffer text_buffer[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory text_memory[MAX_FRAMES_IN_FLIGHT];
    void *text_mapped[MAX_FRAMES_IN_FLIGHT];
    text_vertex_t *text_verts; /* CPU staging, capacity MAX_TEXT_VERTS */
    uint32_t text_vert_count;

    /* Cube instances queued this frame; drained in record_command_buffer. The
       camera's view*proj is baked into each instance's mvp at queue time. */
    mat4_t view;
    mat4_t proj;
    mat4_t *cube_mvps; /* capacity MAX_CUBES */
    uint32_t cube_count;
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
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.spv", KILN_SHADER_DIR, name);

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
    extensions[ext_count++] = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
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
    VkXlibSurfaceCreateInfoKHR info = {0};
    info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    info.dpy = (Display *)window_x11_display(g.window);
    info.window = (Window)window_x11_window(g.window);
    VK_CHECK(vkCreateXlibSurfaceKHR(g.instance, &info, NULL, &g.surface));
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

    VkPhysicalDeviceVulkan13Features features13 = {0};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;

    VkDeviceCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &features13;
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
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; /* vsync, always supported */
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
/* pipeline                                                                  */
/* ----------------------------------------------------------------------- */

static bool create_pipeline(void) {
    VkShaderModule vert, frag;
    if (!create_shader_module("cube.vert", &vert) ||
        !create_shader_module("cube.frag", &frag)) {
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
    binding.stride = sizeof(vertex_t);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2] = {0};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = offsetof(vertex_t, pos);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = offsetof(vertex_t, color);

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
    raster.cullMode = VK_CULL_MODE_NONE; /* v1: no culling, see render notes */
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {0};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth = {0};
    depth.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;

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
    push.size = sizeof(mat4_t);

    VkPipelineLayoutCreateInfo layout = {0};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    VK_CHECK(
        vkCreatePipelineLayout(g.device, &layout, NULL, &g.pipeline_layout));

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
    info.layout = g.pipeline_layout;
    info.renderPass = VK_NULL_HANDLE; /* dynamic rendering */

    VkResult res = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &info,
                                             NULL, &g.pipeline);
    vkDestroyShaderModule(g.device, vert, NULL);
    vkDestroyShaderModule(g.device, frag, NULL);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[render] pipeline creation failed: %d\n", res);
        return false;
    }
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

void render_set_camera(mat4_t view, mat4_t proj) {
    g.view = view;
    g.proj = proj;
}

void render_cube(mat4_t model) {
    if (g.cube_count >= MAX_CUBES) {
        return;
    }
    /* Bake the full mvp now so recording is a flat memcpy-and-draw loop. */
    g.cube_mvps[g.cube_count++] = mat4_mul(g.proj, mat4_mul(g.view, model));
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
        if (c >= 'a' && c <= 'z') {
            c -= 32; /* fold to uppercase */
        }
        if (c < 0x20 || c > 0x5F) {
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

static void record_command_buffer(VkCommandBuffer cmd, uint32_t image_index,
                                  uint32_t cube_count, uint32_t text_verts) {
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

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
    color.clearValue.color.float32[0] = 0.02f;
    color.clearValue.color.float32[1] = 0.02f;
    color.clearValue.color.float32[2] = 0.05f;
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

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g.vertex_buffer, &offset);
    vkCmdBindIndexBuffer(cmd, g.index_buffer, 0, VK_INDEX_TYPE_UINT16);

    uint32_t index_count = sizeof(k_cube_indices) / sizeof(k_cube_indices[0]);
    for (uint32_t i = 0; i < cube_count; i++) {
        vkCmdPushConstants(cmd, g.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(mat4_t), &g.cube_mvps[i]);
        vkCmdDrawIndexed(cmd, index_count, 1, 0, 0, 0);
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
    g.window = window;
    g.view = mat4_identity();
    g.proj = mat4_identity();

    if (!create_instance() || !create_surface() || !pick_physical_device() ||
        !create_device() || !create_swapchain() || !create_depth_resources() ||
        !create_pipeline() || !create_text_pipeline() ||
        !create_commands_and_sync()) {
        return false;
    }

    if (!create_filled_buffer(k_cube_vertices, sizeof(k_cube_vertices),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              &g.vertex_buffer, &g.vertex_memory) ||
        !create_filled_buffer(k_cube_indices, sizeof(k_cube_indices),
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &g.index_buffer,
                              &g.index_memory) ||
        !create_text_buffers()) {
        return false;
    }

    g.cube_mvps = malloc(sizeof(mat4_t) * MAX_CUBES);
    if (!g.cube_mvps) {
        return false;
    }
    return true;
}

void render_draw(void) {
    /* Snapshot and clear the queues up front so they never accumulate across
       an early-return frame (e.g. resize). */
    uint32_t cube_count = g.cube_count;
    g.cube_count = 0;
    uint32_t text_verts = g.text_vert_count;
    g.text_vert_count = 0;

    if (g.extent.width == 0 || g.extent.height == 0) {
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
    record_command_buffer(cmd, image_index, cube_count, text_verts);

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

void render_shutdown(void) {
    if (!g.device) {
        return;
    }
    vkDeviceWaitIdle(g.device);

    vkDestroyBuffer(g.device, g.index_buffer, NULL);
    vkFreeMemory(g.device, g.index_memory, NULL);
    vkDestroyBuffer(g.device, g.vertex_buffer, NULL);
    vkFreeMemory(g.device, g.vertex_memory, NULL);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroyBuffer(g.device, g.text_buffer[i], NULL);
        vkFreeMemory(g.device, g.text_memory[i], NULL);
    }
    free(g.text_verts);
    free(g.cube_mvps);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(g.device, g.image_available[i], NULL);
        vkDestroyFence(g.device, g.in_flight[i], NULL);
    }
    vkDestroyCommandPool(g.device, g.command_pool, NULL);

    vkDestroyPipeline(g.device, g.text_pipeline, NULL);
    vkDestroyPipelineLayout(g.device, g.text_layout, NULL);
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
