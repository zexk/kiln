#pragma once

#include <stdbool.h>

/* A tiny immediate-mode debug UI, drawn through the renderer's 2D overlay
   primitives (render_rect/render_text). One context per frame: feed input to
   ui_begin, emit a panel of widgets, ui_end, then read ui_wants_mouse to decide
   whether the camera should ignore the mouse this frame.

   The UI is render- and ECS-agnostic: callers pass plain pointers to the values
   being tampered with, and widgets mutate them in place. */

typedef struct {
    float mouse_x; /* screen pixels, top-left origin */
    float mouse_y;
    bool mouse_down;    /* left button currently held */
    bool pointer_valid; /* false when the cursor is captured/elsewhere — then
                           the UI registers no hover or clicks (see camera) */
} ui_input_t;

#define UI_MAX_PANELS 8

typedef struct {
    ui_input_t in;
    bool prev_down;
    bool went_down;
    bool went_up;
    float screen_w;
    float screen_h;

    /* interaction state, persists across frames */
    int active_id; /* widget being dragged/held, 0 = none */
    int hot_id;    /* widget under the pointer this frame */
    int id_counter;
    bool pointer_in_panel; /* pointer over any panel this frame */

    /* current panel layout */
    bool in_panel;
    int panel_index;
    float panel_x, panel_y, panel_w;
    float cursor_y;

    float panel_height[UI_MAX_PANELS]; /* cached so the bg can be drawn first */
} ui_t;

void ui_init(ui_t *ui);
void ui_begin(ui_t *ui, const ui_input_t *in, float screen_w, float screen_h);
void ui_end(ui_t *ui);

/* True if the UI consumed the mouse this frame (pointer over a panel, or a
   widget is being dragged). The app suppresses camera input when set. */
bool ui_wants_mouse(const ui_t *ui);

void ui_panel_begin(ui_t *ui, float x, float y, float w);
void ui_panel_end(ui_t *ui);

void ui_text(ui_t *ui, const char *fmt, ...);
bool ui_button(ui_t *ui, const char *label);
bool ui_checkbox(ui_t *ui, const char *label, bool *value);
bool ui_slider_float(ui_t *ui, const char *label, float *value, float min,
                     float max);
