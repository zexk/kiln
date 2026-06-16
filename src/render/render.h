#pragma once

#include <stdbool.h>

#include "platform.h"

/* Vulkan renderer. Owns the device, swapchain and a single pipeline that
   draws a spinning cube. The window must outlive the renderer. */
bool render_init(window_t *window);

/* Acquire, record and present one frame. Recreates the swapchain
   transparently when the window is resized. */
void render_draw(void);

void render_shutdown(void);
