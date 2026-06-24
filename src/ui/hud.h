#pragma once

#include "renderer.h"
#include "ui.h"
#include <stdbool.h>

/* Renderer-backed 2D primitives for HUDs and immediate-mode UI, built on the
   low-level abstract renderer (renderer.h).  This is the default drawing
   backend a game wires into kiln_ui's ui_draw_t vtable, plus a handful of
   standalone primitives (filled rects, bitmap text, buttons) for HUD elements
   that don't need the widget layer.

   Geometry for one frame is batched into a single dynamic vertex buffer using
   the baked kiln_font8x8 glyph table; text is drawn as one filled quad per lit
   font pixel, so no texture atlas is required.  A context owns its own shader
   program, VAO and VBO — call hud_init once after the renderer is up. */

/* Pre-allocated per-frame vertex capacity.  Each 2D rect = 6 verts x 2 floats;
   256 KB ~= 5460 rects, enough for dense debug text. */
#define HUD_VBO_BYTES (256 * 1024)

typedef struct {
    R_Program program;
    R_VAO     vao;
    R_Buffer  vbo;
    int       width;
    int       height;
    float     mouse_x;
    float     mouse_y;
    bool      mouse_down;
    bool      mouse_pressed; /* true the frame the button transitions to down */
    int       batch_floats;  /* floats consumed this frame; reset by hud_begin */
} hud_t;

/* Create the shader program, VAO and VBO.  Returns false if the program fails
   to build (e.g. shaders not found).  The compiled shaders are located via
   $KILN_UI_SHADER_DIR, falling back to the dir baked in at build time. */
bool hud_init(hud_t *g);
void hud_shutdown(hud_t *g);

/* Begin a frame: cache the framebuffer size and pointer state, rewind the
   per-frame vertex allocator.  Pass mouse coords in screen pixels (top-left
   origin); they drive hud_button hover/click. */
void hud_begin(hud_t *g, int width, int height,
               int mouse_x, int mouse_y, bool mouse_down);

/* Filled screen-space rectangle (pixels, top-left origin). */
void hud_rect(hud_t *g, float x, float y, float w, float h,
              float r, float g_, float b);

/* Bitmap text at (x, y); `scale` is the pixel size of one font cell pixel. */
void hud_text(hud_t *g, float x, float y, const char *text, float scale,
              float r, float g_, float b);

/* Pixel width of `text` at `scale` (for centering). */
float hud_text_width(const char *text, float scale);

/* A bordered, auto-labelled button.  Returns true the frame it is clicked. */
bool hud_button(hud_t *g, float x, float y, float w, float h,
                const char *label);

/* A ui_draw_t vtable bound to `g`, for feeding kiln_ui's ui_begin. */
ui_draw_t hud_draw(hud_t *g);
