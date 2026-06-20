#include "gui.h"
#include <string.h>
#include <ctype.h>

#define GUI_FONT_W 5
#define GUI_FONT_H 7

static void gui_set_color(Gui *gui, float r, float g, float b, float a) {
    int color_loc = renderer_uniform_location(gui->program, "uColor");
    int alpha_loc = renderer_uniform_location(gui->program, "uAlpha");
    renderer_uniform_vec3(color_loc, r, g, b);
    renderer_uniform_float(alpha_loc, a);
}

static float gui_ndc_x(const Gui *gui, float x) {
    return (2.0f * x / (float)gui->width) - 1.0f;
}

static float gui_ndc_y(const Gui *gui, float y) {
    return 1.0f - (2.0f * y / (float)gui->height);
}

static void gui_draw_triangles(Gui *gui, const float *verts, int vertex_count, float r, float g, float b, float a) {
    gui_set_color(gui, r, g, b, a);
    renderer_bind_vao(gui->vao);
    renderer_bind_buffer(R_BUF_ARRAY, gui->vbo);
    renderer_buffer_data(R_BUF_ARRAY, (size_t)vertex_count * 2 * sizeof(float), verts, R_USAGE_DYNAMIC);
    renderer_draw_arrays(R_PRIM_TRIANGLES, 0, vertex_count);
}

static void gui_draw_lines(Gui *gui, const float *verts, int vertex_count, float r, float g, float b, float a) {
    gui_set_color(gui, r, g, b, a);
    renderer_bind_vao(gui->vao);
    renderer_bind_buffer(R_BUF_ARRAY, gui->vbo);
    renderer_buffer_data(R_BUF_ARRAY, (size_t)vertex_count * 2 * sizeof(float), verts, R_USAGE_DYNAMIC);
    renderer_draw_arrays(R_PRIM_LINES, 0, vertex_count);
}

static void gui_fill_rect(Gui *gui, float x, float y, float w, float h, float r, float g, float b, float a) {
    float x0 = gui_ndc_x(gui, x);
    float y0 = gui_ndc_y(gui, y);
    float x1 = gui_ndc_x(gui, x + w);
    float y1 = gui_ndc_y(gui, y + h);
    float verts[] = {
        x0, y0,  x1, y1,  x1, y0,
        x0, y0,  x0, y1,  x1, y1,
    };
    gui_draw_triangles(gui, verts, 6, r, g, b, a);
}

static void gui_border_rect(Gui *gui, float x, float y, float w, float h, float r, float g, float b, float a) {
    float x0 = gui_ndc_x(gui, x);
    float y0 = gui_ndc_y(gui, y);
    float x1 = gui_ndc_x(gui, x + w);
    float y1 = gui_ndc_y(gui, y + h);
    float verts[] = {
        x0, y0,  x1, y0,
        x1, y0,  x1, y1,
        x1, y1,  x0, y1,
        x0, y1,  x0, y0,
    };
    gui_draw_lines(gui, verts, 8, r, g, b, a);
}

static bool gui_point_in_rect(const Gui *gui, float x, float y, float w, float h) {
    return gui->mouse_x >= x && gui->mouse_x <= x + w && gui->mouse_y >= y && gui->mouse_y <= y + h;
}

static unsigned char font_row(char c, int row) {
    static const unsigned char blank[7] = {0,0,0,0,0,0,0};
    static const unsigned char glyphs[][8] = {
        {'0', 14,17,19,21,25,17,14}, {'1', 4,12,4,4,4,4,14}, {'2', 14,17,1,2,4,8,31},
        {'3', 30,1,1,14,1,1,30}, {'4', 2,6,10,18,31,2,2}, {'5', 31,16,30,1,1,17,14},
        {'6', 6,8,16,30,17,17,14}, {'7', 31,1,2,4,8,8,8}, {'8', 14,17,17,14,17,17,14},
        {'9', 14,17,17,15,1,2,12}, {'A', 14,17,17,31,17,17,17}, {'B', 30,17,17,30,17,17,30},
        {'C', 14,17,16,16,16,17,14}, {'D', 30,17,17,17,17,17,30}, {'E', 31,16,16,30,16,16,31},
        {'F', 31,16,16,30,16,16,16}, {'G', 14,17,16,23,17,17,14}, {'H', 17,17,17,31,17,17,17},
        {'I', 14,4,4,4,4,4,14}, {'J', 1,1,1,1,17,17,14}, {'K', 17,18,20,24,20,18,17},
        {'L', 16,16,16,16,16,16,31}, {'M', 17,27,21,21,17,17,17}, {'N', 17,25,21,19,17,17,17},
        {'O', 14,17,17,17,17,17,14}, {'P', 30,17,17,30,16,16,16}, {'Q', 14,17,17,17,21,18,13},
        {'R', 30,17,17,30,20,18,17}, {'S', 15,16,16,14,1,1,30}, {'T', 31,4,4,4,4,4,4},
        {'U', 17,17,17,17,17,17,14}, {'V', 17,17,17,17,17,10,4}, {'W', 17,17,17,21,21,21,10},
        {'X', 17,17,10,4,10,17,17}, {'Y', 17,17,10,4,4,4,4}, {'Z', 31,1,2,4,8,16,31},
        {'-', 0,0,0,31,0,0,0}, {'.', 0,0,0,0,0,12,12}, {':', 0,12,12,0,12,12,0},
        {'/', 1,1,2,4,8,16,16}, {'_', 0,0,0,0,0,0,31},
    };
    if (c >= 'a' && c <= 'z') c = (char)toupper((unsigned char)c);
    if (c == ' ') return blank[row];
    for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++) {
        if (glyphs[i][0] == (unsigned char)c) return glyphs[i][row + 1];
    }
    return row == 0 || row == 6 ? 31 : 17;
}

void gui_init(Gui *gui, R_Program program) {
    memset(gui, 0, sizeof(*gui));
    gui->program = program;
    gui->vao = renderer_create_vao();
    gui->vbo = renderer_create_buffer();
    renderer_bind_vao(gui->vao);
    renderer_bind_buffer(R_BUF_ARRAY, gui->vbo);
    renderer_attrib_pointer(0, 2, R_TYPE_FLOAT, false, 2 * sizeof(float), 0);
    renderer_enable_attrib(0);
    renderer_bind_vao(R_INVALID_HANDLE);
}

void gui_shutdown(Gui *gui) {
    if (gui->vbo != R_INVALID_HANDLE) renderer_destroy_buffer(gui->vbo);
    if (gui->vao != R_INVALID_HANDLE) renderer_destroy_vao(gui->vao);
    gui->vbo = R_INVALID_HANDLE;
    gui->vao = R_INVALID_HANDLE;
}

void gui_begin_frame(Gui *gui, int width, int height, int mouse_x, int mouse_y, bool mouse_down) {
    bool was_down = gui->mouse_down;
    gui->width = width > 0 ? width : 1;
    gui->height = height > 0 ? height : 1;
    gui->mouse_x = (float)mouse_x;
    gui->mouse_y = (float)mouse_y;
    gui->mouse_down = mouse_down;
    gui->mouse_pressed = mouse_down && !was_down;
}

void gui_write_text(Gui *gui, float x, float y, const char *text, float scale, float r, float g, float b) {
    if (!text || scale <= 0.0f) return;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n') {
            y += (GUI_FONT_H + 2) * scale;
            x = 0.0f;
            continue;
        }
        for (int row = 0; row < GUI_FONT_H; row++) {
            unsigned char bits = font_row((char)c, row);
            for (int col = 0; col < GUI_FONT_W; col++) {
                if (bits & (1u << (GUI_FONT_W - 1 - col))) {
                    gui_fill_rect(gui, x + col * scale, y + row * scale, scale, scale, r, g, b, 1.0f);
                }
            }
        }
        x += (GUI_FONT_W + 1) * scale;
    }
}

bool gui_create_button(Gui *gui, float x, float y, float w, float h, const char *text, GuiCallback callback, void *userdata) {
    bool hover = gui_point_in_rect(gui, x, y, w, h);
    bool clicked = hover && gui->mouse_pressed;
    float fill = hover ? 0.18f : 0.10f;
    float border = hover ? 0.85f : 0.55f;

    gui_fill_rect(gui, x, y, w, h, fill, fill, fill, 0.9f);
    renderer_line_width(2.0f);
    gui_border_rect(gui, x, y, w, h, border, border, border, 1.0f);
    renderer_line_width(1.0f);

    if (text) {
        float scale = 3.0f;
        float text_w = (float)strlen(text) * (GUI_FONT_W + 1) * scale - scale;
        float text_h = GUI_FONT_H * scale;
        gui_write_text(gui, x + (w - text_w) * 0.5f, y + (h - text_h) * 0.5f, text, scale, 0.9f, 0.9f, 0.9f);
    }

    if (clicked && callback) callback(userdata);
    return clicked;
}
