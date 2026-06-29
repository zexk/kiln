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
typedef uint32_t cubemap_handle_t;
#define RENDER_MESH_INVALID     UINT32_MAX
#define RENDER_TEXTURE_INVALID  UINT32_MAX
#define RENDER_MATERIAL_INVALID UINT32_MAX
#define RENDER_CUBEMAP_INVALID  UINT32_MAX

/* Renderer: manages GPU resources and records one frame per render_draw call.
   The implementation is selected at build time via KILN_RENDERER (default:
   vulkan). The window must outlive the renderer.

   Per-frame usage: set the camera, queue one render_mesh per visible object,
   optionally queue debug text/rects/lines, then call render_draw to present.
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

/* Attach a normal map to an existing material for tangent-space normal mapping.
   Pass RENDER_TEXTURE_INVALID to revert to the built-in flat normal (0,0,1). */
void render_set_material_normal_map(material_handle_t mat,
                                    texture_handle_t normal_map);

/* Set the view and projection used for this frame's queued meshes. Call once
   per frame before render_mesh. Persists until overwritten. */
void render_set_camera(mat4_t view, mat4_t proj);

/* Queue a mesh drawn with the given material and model matrix. Call before
   render_draw; the queue is cleared every frame. */
void render_mesh(mesh_handle_t mesh, material_handle_t material, mat4_t model);

/* Draw `count` instances of a mesh in a single GPU call. Each entry in
   `models` is a model matrix. Both shadow and colour passes are instanced.
   The queue is cleared every frame. Up to RENDER_MAX_INST_TOTAL matrices
   and RENDER_MAX_INST_BATCHES separate render_mesh_instanced calls per frame. */
#define RENDER_MAX_INST_TOTAL   16384
#define RENDER_MAX_INST_BATCHES    64
void render_mesh_instanced(mesh_handle_t mesh, material_handle_t material,
                           const mat4_t *models, uint32_t count);

/* Queue a line of debug text, positioned in screen pixels with a top-left
   origin and +y pointing down. Each glyph is scale*8 px tall. Drawn on top of
   the frame with no depth test. Call before render_draw; the queue is cleared
   every frame. */
void render_text(float x, float y, float scale, float r, float g, float b,
                 const char *str);

/* Queue a filled screen-space rectangle (same overlay stream as render_text,
   painter-ordered: submit backgrounds before the text drawn over them). Opaque.
   Call before render_draw. */
void render_rect(float x, float y, float w, float h, float r, float g, float b);

/* Queue a thick screen-space line (a quad) from (x0,y0) to (x1,y1). Same
   overlay stream as render_rect. */
void render_line(float x0, float y0, float x1, float y1, float thickness,
                 float r, float g, float b);

/* Set the framebuffer clear colour (default a dark blue-grey). Persists. */
void render_set_clear_color(float r, float g, float b);

/* Set the directional light for the next frame. `dir` need not be unit length.
   `color` is the key-light RGB (multiply by intensity before passing in).
   `ambient` is the ambient fill RGB. Both persist until overwritten. */
void render_set_light(vec3_t dir, vec3_t color, vec3_t ambient);

/* Queue a point light for this frame. Up to RENDER_MAX_POINT_LIGHTS per frame;
   extras are silently dropped. The queue is cleared each frame like render_mesh.
   `radius` controls the attenuation falloff distance. */
#define RENDER_MAX_POINT_LIGHTS 8
void render_add_point_light(vec3_t pos, vec3_t color, float radius);

/* Acquire, record and present one frame. Recreates the swapchain
   transparently when the window is resized. */
void render_draw(void);

void render_shutdown(void);

/* Toggle vsync. When enabled, selects FIFO (waits for every vblank).
   When disabled, prefers MAILBOX (no tearing, low latency) and falls
   back to IMMEDIATE if MAILBOX is unsupported. Takes effect on the
   next rendered frame; safe to call between render_draw calls. */
/* Toggle wireframe rendering (VK_POLYGON_MODE_LINE). Falls back to fill if the
   device does not support fillModeNonSolid. */
void render_set_wireframe(bool enabled);
bool render_get_wireframe(void);

void render_set_vsync(bool enabled);
bool render_get_vsync(void);

/* Upload a cubemap from 6 RGBA8 face images (order: +X,-X,+Y,-Y,+Z,-Z).
   All faces must be w×h pixels. Returns RENDER_CUBEMAP_INVALID on failure. */
cubemap_handle_t render_upload_cubemap(const uint8_t *faces[6], uint32_t w, uint32_t h);

/* Set the skybox drawn behind all geometry this frame (pass RENDER_CUBEMAP_INVALID
   to disable). Persists until overwritten. */
void render_set_skybox(cubemap_handle_t cubemap);

/* Bloom post-processing. Default: enabled, threshold=0.8, strength=0.5,
   exposure=1.0 (Reinhard tone-mapping + gamma applied in composite). */
void render_set_bloom(bool enabled, float threshold, float strength, float exposure);

/* Write the next frame's HDR color buffer to a PPM file at `path`.
   Blocks only on that one frame; safe to call between render_draw calls. */
void render_save_screenshot(const char *path);

/* Register a hook called each frame after the HDR composite pass while the
   swapchain image is still in COLOR_ATTACHMENT_OPTIMAL layout.  The callback
   records thin-renderer draw calls into the active command buffer obtained via
   render_get_overlay_cmd().  Pass fn=NULL to unregister. */
void render_set_overlay_hook(void (*fn)(void *ud), void *ud);

/* GPU particle emitter: particle state lives in device-local SSBOs; a compute
   shader advances physics and writes indirect draw arguments each frame.
   The draw reuses the existing instanced pipeline so lighting and shadows apply.

   Usage:
     handle = render_create_gpu_emitter(mesh, mat, 4096, (vec3_t){0,-9.8f,0});
     // each frame:
     render_gpu_emitter_emit(handle, pos, vel, life, scale);  // 0-N times
     render_gpu_emitter_update(handle, dt);                   // once per frame
*/
typedef uint32_t gpu_emitter_handle_t;
#define RENDER_GPU_EMITTER_INVALID UINT32_MAX

gpu_emitter_handle_t render_create_gpu_emitter(mesh_handle_t mesh,
                                               material_handle_t mat,
                                               uint32_t capacity,
                                               vec3_t gravity);
void render_destroy_gpu_emitter(gpu_emitter_handle_t h);

void render_gpu_emitter_emit(gpu_emitter_handle_t h,
                             vec3_t pos, vec3_t vel,
                             float life, float scale);

/* Advance the simulation by dt seconds and queue the emitter for this frame's
   draw.  Call once per frame between render_set_camera and render_draw. */
void render_gpu_emitter_update(gpu_emitter_handle_t h, float dt);
