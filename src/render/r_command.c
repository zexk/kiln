#include "render_internal.h"

bool create_command_pool(void) {
    VkCommandPoolCreateInfo ci = {0};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = g_vk.graphics_family;
    if (vkCreateCommandPool(g_vk.device, &ci, NULL, &g_vk.cmd_pool) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create command pool\n");
        return false;
    }
    return true;
}

bool create_command_buffers(void) {
    g_vk.cmd_buffers = malloc(sizeof(VkCommandBuffer) * MAX_FRAMES_IN_FLIGHT);
    if (!g_vk.cmd_buffers) { fprintf(stderr, "OOM\n"); return false; }

    VkCommandBufferAllocateInfo ai = {0};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = g_vk.cmd_pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    if (vkAllocateCommandBuffers(g_vk.device, &ai, g_vk.cmd_buffers) != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate command buffers\n");
        free(g_vk.cmd_buffers);
        return false;
    }
    return true;
}

bool create_sync_objects(void) {
    g_vk.image_avail_sems = malloc(sizeof(VkSemaphore) * MAX_FRAMES_IN_FLIGHT);
    g_vk.render_done_sems = malloc(sizeof(VkSemaphore) * MAX_FRAMES_IN_FLIGHT);
    g_vk.in_flight_fences = malloc(sizeof(VkFence)     * MAX_FRAMES_IN_FLIGHT);
    if (!g_vk.image_avail_sems || !g_vk.render_done_sems || !g_vk.in_flight_fences) {
        fprintf(stderr, "OOM\n");
        free(g_vk.image_avail_sems);
        free(g_vk.render_done_sems);
        free(g_vk.in_flight_fences);
        return false;
    }

    VkSemaphoreCreateInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fi = {0};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(g_vk.device, &si, NULL, &g_vk.image_avail_sems[i]) != VK_SUCCESS ||
            vkCreateSemaphore(g_vk.device, &si, NULL, &g_vk.render_done_sems[i]) != VK_SUCCESS ||
            vkCreateFence    (g_vk.device, &fi, NULL, &g_vk.in_flight_fences[i])  != VK_SUCCESS) {
            fprintf(stderr, "Failed to create sync object %d\n", i);
            for (int j = 0; j < i; j++) {
                vkDestroyFence    (g_vk.device, g_vk.in_flight_fences[j], NULL);
                vkDestroySemaphore(g_vk.device, g_vk.image_avail_sems[j], NULL);
                vkDestroySemaphore(g_vk.device, g_vk.render_done_sems[j], NULL);
            }
            return false;
        }
    }
    return true;
}
