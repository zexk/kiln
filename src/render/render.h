#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "linalg.h"
#include "mesh.h"
#include "platform.h"

/* Opaque handles to GPU-resident resources. The *_INVALID values are never
   returned by a successful create/upload. */
typedef uint32_t mesh_handle_t;
typedef uint32_t texture_handle_t;
typedef uint32_t material_handle_t;
#define RENDER_MESH_INVALID UINT32_MAX
#define RENDER_TEXTURE_INVALID UINT32_MAX
#define RENDER_MATERIAL_INVALID UINT32_MAX

/* Vulkan renderer. Owns the device, swapchain and a lit mesh pipeline. The
   window must outlive the renderer.

   Per-frame usage: set the camera, queue one render_mesh per visible object,
   optionally queue debug text, then call render_draw to record and present.
   The queues are cleared every frame. The renderer is deliberately
   ECS-agnostic: callers translate their scene into camera + model matrices. */
bool render_init(window_t *window);

/* Upload a CPU mesh to the GPU and return a handle for render_mesh. The CPU
   mesh may be freed afterwards. Returns RENDER_MESH_INVALID on failure. */
mesh_handle_t render_upload_mesh(const cpu_mesh_t *mesh);

/* Upload tightly-packed RGBA8 pixels (w*h*4 bytes) as a sampled 2D texture.
   Returns RENDER_TEXTURE_INVALID on failure. */
texture_handle_t render_upload_texture(const uint8_t *rgba, uint32_t w,
                                       uint32_t h);

/* Create a material: a base colour modulated by an albedo texture. Pass
   RENDER_TEXTURE_INVALID for `texture` to get a flat colour (a built-in white
   texture is substituted). Returns RENDER_MATERIAL_INVALID on failure. */
material_handle_t render_create_material(vec4_t base_color,
                                         texture_handle_t texture);

/* Set the view and projection used for this frame's queued meshes. Call once
   per frame before render_mesh. Persists until overwritten. */
void render_set_camera(mat4_t view, mat4_t proj);

/* Queue a mesh drawn with the given material and model matrix. Call before
   render_draw; the queue is cleared every frame. */
void render_mesh(mesh_handle_t mesh, material_handle_t material, mat4_t model);

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
