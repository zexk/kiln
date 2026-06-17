#pragma once

#include <stdbool.h>

#include "platform.h"

/* Vulkan renderer. Owns the device, swapchain and a single pipeline that
   draws a spinning cube. The window must outlive the renderer. */
bool render_init(window_t *window);

/* Queue a line of debug text, positioned in screen pixels with a top-left
   origin and +y pointing down. Each glyph is scale*8 px tall. Drawn on top of
   the frame with no depth test. Call before render_draw; the queue is cleared
   every frame. Lowercase letters render as uppercase (see font8x8.h). */
void render_text(float x, float y, float scale, float r, float g, float b,
                 const char *str);

/* Acquire, record and present one frame. Recreates the swapchain
   transparently when the window is resized. */
void render_draw(void);

void render_shutdown(void);
