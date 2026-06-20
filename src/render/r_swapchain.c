#include "render_internal.h"

bool create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.physical_device, g_vk.surface, &caps);

    g_vk.swap_extent = caps.currentExtent;
    if (g_vk.swap_extent.width == UINT32_MAX) {
        g_vk.swap_extent.width  = (uint32_t)g_vk.width;
        g_vk.swap_extent.height = (uint32_t)g_vk.height;
    }

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.physical_device, g_vk.surface, &fmt_count, NULL);
    VkSurfaceFormatKHR *fmts = malloc(sizeof(VkSurfaceFormatKHR) * fmt_count);
    if (!fmts) { fprintf(stderr, "OOM\n"); return false; }
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.physical_device, g_vk.surface, &fmt_count, fmts);

    g_vk.swap_format = fmts[0].format;
    for (uint32_t i = 0; i < fmt_count; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_SRGB) {
            g_vk.swap_format = fmts[i].format;
            break;
        }
    }
    free(fmts);

    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.physical_device, g_vk.surface, &mode_count, NULL);
    VkPresentModeKHR *modes = malloc(sizeof(VkPresentModeKHR) * mode_count);
    if (!modes) { fprintf(stderr, "OOM\n"); return false; }
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.physical_device, g_vk.surface, &mode_count, modes);

    VkPresentModeKHR desired = g_vk.present_mode;
    bool found = false;
    for (uint32_t i = 0; i < mode_count; i++) {
        if (modes[i] == desired) { found = true; break; }
    }
    if (!found) desired = VK_PRESENT_MODE_FIFO_KHR;
    free(modes);

    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && img_count > caps.maxImageCount)
        img_count = caps.maxImageCount;
    g_vk.swap_image_count = img_count;

    VkSwapchainCreateInfoKHR ci = {0};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = g_vk.surface;
    ci.minImageCount    = img_count;
    ci.imageFormat      = g_vk.swap_format;
    ci.imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent      = g_vk.swap_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = desired;
    ci.clipped          = VK_TRUE;

    if (vkCreateSwapchainKHR(g_vk.device, &ci, NULL, &g_vk.swapchain) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create swapchain\n");
        return false;
    }

    vkGetSwapchainImagesKHR(g_vk.device, g_vk.swapchain, &g_vk.swap_image_count, NULL);
    g_vk.swap_images = malloc(sizeof(VkImage) * g_vk.swap_image_count);
    if (!g_vk.swap_images) { fprintf(stderr, "OOM\n"); return false; }
    vkGetSwapchainImagesKHR(g_vk.device, g_vk.swapchain, &g_vk.swap_image_count, g_vk.swap_images);
    return true;
}

bool create_swap_image_views(void) {
    g_vk.swap_views = malloc(sizeof(VkImageView) * g_vk.swap_image_count);
    if (!g_vk.swap_views) { fprintf(stderr, "OOM\n"); return false; }

    for (uint32_t i = 0; i < g_vk.swap_image_count; i++) {
        VkImageViewCreateInfo ci = {0};
        ci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image                           = g_vk.swap_images[i];
        ci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        ci.format                          = g_vk.swap_format;
        ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.levelCount     = 1;
        ci.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(g_vk.device, &ci, NULL, &g_vk.swap_views[i]) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create swap image view %u\n", i);
            return false;
        }
    }
    return true;
}

bool create_depth_buffer(void) {
    VkImageCreateInfo ii = {0};
    ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.extent.width  = g_vk.swap_extent.width;
    ii.extent.height = g_vk.swap_extent.height;
    ii.extent.depth  = 1;
    ii.mipLevels     = 1;
    ii.arrayLayers   = 1;
    ii.format        = VK_FORMAT_D32_SFLOAT;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(g_vk.device, &ii, NULL, &g_vk.depth_image) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create depth image\n");
        return false;
    }

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(g_vk.device, g_vk.depth_image, &reqs);

    VkMemoryAllocateInfo ai = {0};
    ai.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = reqs.size;
    uint32_t mt = find_memory_type(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) { vkDestroyImage(g_vk.device, g_vk.depth_image, NULL); return false; }
    ai.memoryTypeIndex = mt;

    if (vkAllocateMemory(g_vk.device, &ai, NULL, &g_vk.depth_memory) != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate depth memory\n");
        vkDestroyImage(g_vk.device, g_vk.depth_image, NULL);
        return false;
    }
    vkBindImageMemory(g_vk.device, g_vk.depth_image, g_vk.depth_memory, 0);

    VkImageViewCreateInfo vi = {0};
    vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image                           = g_vk.depth_image;
    vi.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    vi.format                          = VK_FORMAT_D32_SFLOAT;
    vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.levelCount     = 1;
    vi.subresourceRange.layerCount     = 1;
    if (vkCreateImageView(g_vk.device, &vi, NULL, &g_vk.depth_view) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create depth image view\n");
        vkFreeMemory(g_vk.device, g_vk.depth_memory, NULL);
        vkDestroyImage(g_vk.device, g_vk.depth_image, NULL);
        return false;
    }
    return true;
}

void cleanup_swapchain(void) {
    vkDeviceWaitIdle(g_vk.device);

    if (g_vk.depth_view)   { vkDestroyImageView(g_vk.device, g_vk.depth_view, NULL);   g_vk.depth_view   = VK_NULL_HANDLE; }
    if (g_vk.depth_image)  { vkDestroyImage(g_vk.device, g_vk.depth_image, NULL);       g_vk.depth_image  = VK_NULL_HANDLE; }
    if (g_vk.depth_memory) { vkFreeMemory(g_vk.device, g_vk.depth_memory, NULL);        g_vk.depth_memory = VK_NULL_HANDLE; }

    if (g_vk.swap_views) {
        for (uint32_t i = 0; i < g_vk.swap_image_count; i++)
            if (g_vk.swap_views[i]) vkDestroyImageView(g_vk.device, g_vk.swap_views[i], NULL);
        free(g_vk.swap_views);
        g_vk.swap_views = NULL;
    }
    if (g_vk.swapchain) { vkDestroySwapchainKHR(g_vk.device, g_vk.swapchain, NULL); g_vk.swapchain = VK_NULL_HANDLE; }
    if (g_vk.swap_images) { free(g_vk.swap_images); g_vk.swap_images = NULL; }
}

bool recreate_swapchain(void) {
    vkDeviceWaitIdle(g_vk.device);
    cleanup_swapchain();
    if (!create_swapchain())        return false;
    if (!create_swap_image_views()) return false;
    if (!create_depth_buffer())     return false;
    return true;
}
