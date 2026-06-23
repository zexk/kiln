#include "ui_gl.h"
#include "font8x8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef KILN_UI_SHADER_DIR
#define KILN_UI_SHADER_DIR "."
#endif

/* kiln_font8x8 glyphs occupy a 5-wide cell in the high bits (bit 7 leftmost,
   bit 3 the fifth column) across 8 rows.  One column of spacing gives a 6 px
   advance. */
#define UI_GL_FONT_W   5
#define UI_GL_FONT_H   8
#define UI_GL_ADVANCE  (UI_GL_FONT_W + 1)

static void ui_gl_set_color(ui_gl_t *g, float r, float gg, float b, float a) {
    int color_loc = renderer_uniform_location(g->program, "uColor");
    int alpha_loc = renderer_uniform_location(g->program, "uAlpha");
    renderer_uniform_vec3(color_loc, r, gg, b);
    renderer_uniform_float(alpha_loc, a);
}

static float ui_gl_ndc_x(const ui_gl_t *g, float x) {
    return (2.0f * x / (float)g->width) - 1.0f;
}

static float ui_gl_ndc_y(const ui_gl_t *g, float y) {
    return 1.0f - (2.0f * y / (float)g->height);
}

static void ui_gl_draw_triangles(ui_gl_t *g, const float *verts, int vertex_count,
                                 float r, float gg, float b, float a) {
    /* Each draw gets its own slice of the pre-allocated VBO so that all draws
       issued in a frame remain valid when the command buffer executes on the
       GPU.  Without this, every call overwrites offset 0 and all-but-last draws
       read stale data. */
    size_t byte_off  = (size_t)g->batch_floats * sizeof(float);
    size_t byte_size = (size_t)(vertex_count * 2) * sizeof(float);
    if (byte_off + byte_size > UI_GL_VBO_BYTES) return; /* overflow guard */

    ui_gl_set_color(g, r, gg, b, a);
    renderer_bind_vao(g->vao);
    renderer_bind_buffer(R_BUF_ARRAY, g->vbo);
    renderer_buffer_sub_data(R_BUF_ARRAY, byte_off, byte_size, verts);
    renderer_draw_arrays(R_PRIM_TRIANGLES, g->batch_floats / 2, vertex_count);
    g->batch_floats += vertex_count * 2;
}

static void ui_gl_fill_rect(ui_gl_t *g, float x, float y, float w, float h,
                            float r, float gg, float b, float a) {
    float x0 = ui_gl_ndc_x(g, x);
    float y0 = ui_gl_ndc_y(g, y);
    float x1 = ui_gl_ndc_x(g, x + w);
    float y1 = ui_gl_ndc_y(g, y + h);
    float verts[] = {
        x0, y0,  x1, y1,  x1, y0,
        x0, y0,  x0, y1,  x1, y1,
    };
    ui_gl_draw_triangles(g, verts, 6, r, gg, b, a);
}

static void ui_gl_border_rect(ui_gl_t *g, float x, float y, float w, float h,
                              float r, float gg, float b, float a) {
    float t = 2.0f;
    ui_gl_fill_rect(g, x,       y,       w,       t,       r, gg, b, a); /* top    */
    ui_gl_fill_rect(g, x,       y+h-t,   w,       t,       r, gg, b, a); /* bottom */
    ui_gl_fill_rect(g, x,       y+t,     t,       h-2*t,   r, gg, b, a); /* left   */
    ui_gl_fill_rect(g, x+w-t,   y+t,     t,       h-2*t,   r, gg, b, a); /* right  */
}

static bool ui_gl_point_in_rect(const ui_gl_t *g, float x, float y, float w, float h) {
    return g->mouse_x >= x && g->mouse_x <= x + w &&
           g->mouse_y >= y && g->mouse_y <= y + h;
}

bool ui_gl_init(ui_gl_t *g) {
    memset(g, 0, sizeof(*g));

    const char *dir = getenv("KILN_UI_SHADER_DIR");
    if (!dir || !dir[0]) dir = KILN_UI_SHADER_DIR;

    char vert[512], frag[512];
    snprintf(vert, sizeof(vert), "%s/hud.vert", dir);
    snprintf(frag, sizeof(frag), "%s/hud.frag", dir);

    g->program = renderer_create_program(vert, frag);
    if (g->program == R_INVALID_HANDLE) return false;

    g->vao = renderer_create_vao();
    g->vbo = renderer_create_buffer();
    renderer_bind_vao(g->vao);
    renderer_bind_buffer(R_BUF_ARRAY, g->vbo);
    renderer_buffer_data(R_BUF_ARRAY, UI_GL_VBO_BYTES, NULL, R_USAGE_DYNAMIC);
    renderer_attrib_pointer(0, 2, R_TYPE_FLOAT, false, 2 * sizeof(float), 0);
    renderer_enable_attrib(0);
    renderer_bind_vao(R_INVALID_HANDLE);
    return true;
}

void ui_gl_shutdown(ui_gl_t *g) {
    if (g->vbo != R_INVALID_HANDLE) renderer_destroy_buffer(g->vbo);
    if (g->vao != R_INVALID_HANDLE) renderer_destroy_vao(g->vao);
    if (g->program != R_INVALID_HANDLE) renderer_destroy_program(g->program);
    g->vbo = R_INVALID_HANDLE;
    g->vao = R_INVALID_HANDLE;
    g->program = R_INVALID_HANDLE;
}

void ui_gl_begin(ui_gl_t *g, int width, int height,
                 int mouse_x, int mouse_y, bool mouse_down) {
    bool was_down = g->mouse_down;
    g->width  = width  > 0 ? width  : 1;
    g->height = height > 0 ? height : 1;
    g->mouse_x = (float)mouse_x;
    g->mouse_y = (float)mouse_y;
    g->mouse_down = mouse_down;
    g->mouse_pressed = mouse_down && !was_down;
    g->batch_floats = 0; /* rewind the VBO linear allocator each frame */
}

void ui_gl_rect(ui_gl_t *g, float x, float y, float w, float h,
                float r, float gg, float b) {
    ui_gl_fill_rect(g, x, y, w, h, r, gg, b, 1.0f);
}

void ui_gl_text(ui_gl_t *g, float x, float y, const char *text, float scale,
                float r, float gg, float b) {
    if (!text || scale <= 0.0f) return;
    float start_x = x;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n') {
            y += (UI_GL_FONT_H + 1) * scale;
            x = start_x;
            continue;
        }
        if (c < 0x20 || c > 0x7f) c = '?';
        const uint8_t *glyph = kiln_font8x8[c - 0x20];
        for (int row = 0; row < UI_GL_FONT_H; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < UI_GL_FONT_W; col++) {
                if (bits & (1u << (7 - col))) {
                    ui_gl_fill_rect(g, x + col * scale, y + row * scale,
                                    scale, scale, r, gg, b, 1.0f);
                }
            }
        }
        x += UI_GL_ADVANCE * scale;
    }
}

float ui_gl_text_width(const char *text, float scale) {
    int n = text ? (int)strlen(text) : 0;
    return n > 0 ? (float)(n * UI_GL_ADVANCE - 1) * scale : 0.0f;
}

bool ui_gl_button(ui_gl_t *g, float x, float y, float w, float h,
                  const char *label) {
    bool hover   = ui_gl_point_in_rect(g, x, y, w, h);
    bool clicked = hover && g->mouse_pressed;
    float fill   = hover ? 0.18f : 0.10f;
    float border = hover ? 0.85f : 0.55f;

    ui_gl_fill_rect(g, x, y, w, h, fill, fill, fill, 0.9f);
    renderer_line_width(2.0f);
    ui_gl_border_rect(g, x, y, w, h, border, border, border, 1.0f);
    renderer_line_width(1.0f);

    if (label) {
        float scale  = 3.0f;
        float text_w = ui_gl_text_width(label, scale);
        float text_h = UI_GL_FONT_H * scale;
        ui_gl_text(g, x + (w - text_w) * 0.5f, y + (h - text_h) * 0.5f,
                   label, scale, 0.9f, 0.9f, 0.9f);
    }
    return clicked;
}

/* ── ui_draw_t adapter ──────────────────────────────────────────────────── */

static void ui_gl_draw_rect(void *ud, float x, float y, float w, float h,
                            float r, float g, float b) {
    ui_gl_rect((ui_gl_t *)ud, x, y, w, h, r, g, b);
}
static void ui_gl_draw_text(void *ud, float x, float y, float scale,
                            float r, float g, float b, const char *s) {
    ui_gl_text((ui_gl_t *)ud, x, y, s, scale, r, g, b);
}

ui_draw_t ui_gl_draw(ui_gl_t *g) {
    return (ui_draw_t){ui_gl_draw_rect, ui_gl_draw_text, g};
}
