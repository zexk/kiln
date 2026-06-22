#pragma once
#include "renderer.h"
#include <stdbool.h>

typedef void (*GuiCallback)(void *userdata);

/* Pre-allocated VBO capacity for one frame's worth of GUI geometry.
   Each 2D rect = 6 vertices × 2 floats = 12 floats.  256 KB ≈ 5460 rects. */
#define GUI_VBO_BYTES (256 * 1024)

typedef struct {
    R_Program program;
    R_VAO     vao;
    R_Buffer  vbo;
    int       width;
    int       height;
    float     mouse_x;
    float     mouse_y;
    bool      mouse_down;
    bool      mouse_pressed;
    int       batch_floats; /* floats consumed this frame; reset each gui_begin_frame */
} Gui;

void gui_init(Gui *gui, R_Program program);
void gui_shutdown(Gui *gui);

/* Filled screen-space rectangle, pixels, top-left origin. */
void gui_rect(Gui *gui, float x, float y, float w, float h, float r, float g, float b);
void gui_begin_frame(Gui *gui, int width, int height, int mouse_x, int mouse_y, bool mouse_down);
bool gui_create_button(Gui *gui, float x, float y, float w, float h, const char *text, GuiCallback callback, void *userdata);
void gui_write_text(Gui *gui, float x, float y, const char *text, float scale, float r, float g, float b);
/* Pixel width of a string at the given scale (useful for centering). */
float gui_text_width(const char *text, float scale);
