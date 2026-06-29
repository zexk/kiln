#pragma once

#include <stdbool.h>

/* A tiny immediate-mode debug UI.  Rendering is fully backend-agnostic:
   callers supply a ui_draw_t vtable at ui_begin time that routes rect and text
   primitives to whatever renderer is in use (render.h for the editor,
   renderer.h for the game, etc.).

   One context per frame: feed input + draw vtable to ui_begin, emit a panel of
   widgets, ui_end, then read ui_wants_mouse to decide whether the camera should
   ignore the mouse this frame.  Widgets mutate plain pointers in place. */

/* Drawing backend: two primitives are enough for all widgets.
   `ud` is the userdata pointer stored in ui_draw_t; pass NULL if unused. */
typedef struct {
    void (*rect)(void *ud, float x, float y, float w, float h,
                 float r, float g, float b, float a);
    void (*text)(void *ud, float x, float y, float scale,
                 float r, float g, float b, const char *s);
    void *userdata;
} ui_draw_t;

typedef struct {
    float mouse_x; /* screen pixels, top-left origin */
    float mouse_y;
    bool mouse_down;    /* left button currently held */
    bool pointer_valid; /* false when the cursor is captured/elsewhere — then
                           the UI registers no hover or clicks (see camera) */
    /* First key pressed this frame; 0 (KEY_UNKNOWN) if none.  Used by
       ui_keybind to capture the new key while in rebind-listening mode. */
    int key_pressed;
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

    /* Persistent state for ui_drag_float: the reference mouse-x and value
       captured when the drag started, kept across frames while active. */
    float drag_ref_x;
    float drag_ref_val;

    /* Persistent state for ui_keybind: id of the widget currently waiting
       for a key press; 0 means no widget is in rebind-listening mode. */
    int keybind_listening;

    ui_draw_t draw; /* set by ui_begin each frame */
} ui_t;

void ui_init(ui_t *ui);
void ui_begin(ui_t *ui, const ui_input_t *in, float screen_w, float screen_h,
              const ui_draw_t *draw);
void ui_end(ui_t *ui);

/* True if the UI consumed the mouse this frame (pointer over a panel, or a
   widget is being dragged). The app suppresses camera input when set. */
bool ui_wants_mouse(const ui_t *ui);

void ui_panel_begin(ui_t *ui, float x, float y, float w);
void ui_panel_end(ui_t *ui);

void ui_text(ui_t *ui, const char *fmt, ...);
void ui_separator(ui_t *ui);
bool ui_button(ui_t *ui, const char *label);
bool ui_checkbox(ui_t *ui, const char *label, bool *value);
bool ui_slider_float(ui_t *ui, const char *label, float *value, float min,
                     float max);
bool ui_slider_int(ui_t *ui, const char *label, int *value, int min, int max);
void ui_progress(ui_t *ui, const char *label, float value, float max);
bool ui_input_int(ui_t *ui, const char *label, int *value, int step);

/* A left-aligned, click-to-select row.  `selected` tints the background blue
   even when not hovered, indicating the active selection.  Returns true the
   frame the row is clicked. */
bool ui_selectable(ui_t *ui, const char *label, bool selected);

/* A numeric drag field: click and drag left/right to change `*value` by
   `speed` units per pixel.  Returns true every frame the value changes. */
bool ui_drag_float(ui_t *ui, const char *label, float *value, float speed);

/* Collapsible section header: draws a toggle button "v LABEL" / "> LABEL".
   Flips *open on click and returns the new state.  Caller gates content:
     if (ui_collapsible(ui, "My Section", &open)) { ... widgets ... }
   When closed, omitted widgets don't advance the cursor — correct behaviour. */
bool ui_collapsible(ui_t *ui, const char *label, bool *open);

/* Key-binding widget: shows "label: key_name". Click to enter rebind mode
   (shows "label: [press key...]"), then press any key to set *key to the new
   value.  key_name is typically key_code_to_name(*key) from settings.h.
   Returns true the frame *key changes. */
bool ui_keybind(ui_t *ui, const char *label, int *key, const char *key_name);

/* Rolling bar chart of `count` float samples stored in a circular buffer.
   `head` is the index of the next write slot (oldest sample = head % count).
   Bars are coloured green → yellow → red as values approach `max`.
   Pass `target > 0` to draw a horizontal reference line at that value. */
void ui_graph(ui_t *ui, const char *label,
              const float *samples, int count, int head,
              float max, float target);
