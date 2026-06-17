#pragma once

#include <stdbool.h>

#include "linalg.h"
#include "platform.h"

/* Vulkan renderer. Owns the device, swapchain and a single pipeline that
   draws unit cubes. The window must outlive the renderer.

   Per-frame usage: set the camera, queue one render_cube per visible object,
   optionally queue debug text, then call render_draw to record and present.
   Both queues are cleared every frame. The renderer is deliberately
   ECS-agnostic: callers translate their scene into camera + model matrices. */
bool render_init(window_t *window);

/* Set the view and projection used for this frame's queued cubes. Call once
   per frame before render_cube. Persists until overwritten. */
void render_set_camera(mat4_t view, mat4_t proj);

/* Queue a unit cube (centred on the origin, vertex-coloured by corner) drawn
   with the given model matrix. Call before render_draw; the queue is cleared
   every frame. */
void render_cube(mat4_t model);

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
