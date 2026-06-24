#include "render_internal.h"

#ifdef ENABLE_VALIDATION
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT        type,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *user_data) {
    (void)type; (void)user_data;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        fprintf(stderr, "[Vulkan] %s\n", data->pMessage);
    return VK_FALSE;
}
#endif

bool create_instance(void) {
    VkApplicationInfo app = {0};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "Kiln";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName        = "KilnEngine";
    app.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion         = VK_API_VERSION_1_3;

    const char *exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef _WIN32
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#else
        VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
#endif
#ifdef ENABLE_VALIDATION
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#endif
    };

#ifdef ENABLE_VALIDATION
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, NULL);
    bool val_available = false;
    if (layer_count > 0) {
        VkLayerProperties *layers = malloc(sizeof(VkLayerProperties) * layer_count);
        vkEnumerateInstanceLayerProperties(&layer_count, layers);
        for (uint32_t i = 0; i < layer_count; i++) {
            if (strcmp(layers[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                val_available = true;
                break;
            }
        }
        free(layers);
    }
    if (!val_available)
        fprintf(stderr, "Warning: VK_LAYER_KHRONOS_validation not available\n");
#endif

    VkInstanceCreateInfo ci = {0};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = sizeof(exts) / sizeof(exts[0]);
    ci.ppEnabledExtensionNames = exts;
#ifdef ENABLE_VALIDATION
    if (val_available) {
        const char *val_layers[] = {"VK_LAYER_KHRONOS_validation"};
        ci.enabledLayerCount   = 1;
        ci.ppEnabledLayerNames = val_layers;
    }
#endif

    if (vkCreateInstance(&ci, NULL, &g_vk.instance) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create Vulkan instance\n");
        return false;
    }

#ifdef ENABLE_VALIDATION
    if (val_available) {
        PFN_vkCreateDebugUtilsMessengerEXT fn =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                g_vk.instance, "vkCreateDebugUtilsMessengerEXT");
        if (fn) {
            VkDebugUtilsMessengerCreateInfoEXT di = {0};
            di.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            di.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            di.pfnUserCallback = debug_callback;
            fn(g_vk.instance, &di, NULL, &g_debug_messenger);
        }
    }
#endif
    return true;
}

bool select_physical_device(void) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g_vk.instance, &count, NULL);
    if (count == 0) { fprintf(stderr, "No Vulkan-compatible devices found\n"); return false; }

    VkPhysicalDevice *devs = malloc(sizeof(VkPhysicalDevice) * count);
    if (!devs) { fprintf(stderr, "OOM\n"); return false; }
    vkEnumeratePhysicalDevices(g_vk.instance, &count, devs);

    g_vk.physical_device = devs[0];
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            g_vk.physical_device = devs[i];
            printf("Using GPU: %s\n", props.deviceName);
            break;
        }
    }
    free(devs);
    return true;
}

bool find_queue_families(void) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.physical_device, &count, NULL);
    VkQueueFamilyProperties *qs = malloc(sizeof(VkQueueFamilyProperties) * count);
    if (!qs) { fprintf(stderr, "OOM\n"); return false; }
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.physical_device, &count, qs);

    g_vk.graphics_family = UINT32_MAX;
    g_vk.compute_family  = UINT32_MAX;
    g_vk.present_family  = UINT32_MAX;

    for (uint32_t i = 0; i < count; i++) {
        if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && g_vk.graphics_family == UINT32_MAX)
            g_vk.graphics_family = i;
        if ((qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && g_vk.compute_family == UINT32_MAX)
            g_vk.compute_family = i;
    }
    for (uint32_t i = 0; i < count; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.physical_device, i, g_vk.surface, &present);
        if (present) { g_vk.present_family = i; break; }
    }
    free(qs);

    if (g_vk.graphics_family == UINT32_MAX) { fprintf(stderr, "No graphics queue\n"); return false; }
    if (g_vk.present_family  == UINT32_MAX) { fprintf(stderr, "No present queue\n");  return false; }
    return true;
}

bool create_device(void) {
    float prio = 1.0f;

    uint32_t families[3];
    uint32_t n = 0;
    families[n++] = g_vk.graphics_family;
    if (g_vk.compute_family != g_vk.graphics_family && g_vk.compute_family != UINT32_MAX)
        families[n++] = g_vk.compute_family;
    if (g_vk.present_family != g_vk.graphics_family &&
        g_vk.present_family != g_vk.compute_family  &&
        g_vk.present_family != UINT32_MAX)
        families[n++] = g_vk.present_family;

    VkDeviceQueueCreateInfo qcis[3];
    for (uint32_t i = 0; i < n; i++) {
        qcis[i].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qcis[i].pNext            = NULL;
        qcis[i].flags            = 0;
        qcis[i].queueFamilyIndex = families[i];
        qcis[i].queueCount       = 1;
        qcis[i].pQueuePriorities = &prio;
    }

    const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkPhysicalDeviceVulkan13Features vk13 = {0};
    vk13.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vk13.dynamicRendering  = VK_TRUE;
    vk13.synchronization2  = VK_TRUE;

    VkDeviceCreateInfo ci = {0};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext                   = &vk13;
    ci.queueCreateInfoCount    = n;
    ci.pQueueCreateInfos       = qcis;
    ci.enabledExtensionCount   = 1;
    ci.ppEnabledExtensionNames = dev_exts;

    if (vkCreateDevice(g_vk.physical_device, &ci, NULL, &g_vk.device) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create logical device\n");
        return false;
    }

    vkGetDeviceQueue(g_vk.device, g_vk.graphics_family, 0, &g_vk.graphics_queue);
    vkGetDeviceQueue(g_vk.device, g_vk.present_family,  0, &g_vk.present_queue);
    if (g_vk.compute_family != UINT32_MAX)
        vkGetDeviceQueue(g_vk.device, g_vk.compute_family, 0, &g_vk.compute_queue);
    else {
        g_vk.compute_queue  = g_vk.graphics_queue;
        g_vk.compute_family = g_vk.graphics_family;
    }

    VkPipelineCacheCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    vkCreatePipelineCache(g_vk.device, &pci, NULL, &g_pipeline_cache);
    return true;
}

bool create_surface(void) {
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR ci = {0};
    ci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.hinstance = (HINSTANCE)g_vk.native_handles.display;
    ci.hwnd      = (HWND)(uintptr_t)g_vk.native_handles.window;
    if (vkCreateWin32SurfaceKHR(g_vk.instance, &ci, NULL, &g_vk.surface) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create Win32 surface\n");
        return false;
    }
#else
    VkXlibSurfaceCreateInfoKHR ci = {0};
    ci.sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    ci.dpy    = (Display *)g_vk.native_handles.display;
    ci.window = (Window)g_vk.native_handles.window;
    if (vkCreateXlibSurfaceKHR(g_vk.instance, &ci, NULL, &g_vk.surface) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create X11 surface\n");
        return false;
    }
#endif
    return true;
}

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
