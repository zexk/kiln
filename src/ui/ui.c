#include "ui.h"

#include "render.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Visual metrics (screen pixels). */
#define UI_SCALE 2.0f
#define UI_GLYPH_W (6.0f * UI_SCALE) /* advance per character */
#define UI_GLYPH_H (8.0f * UI_SCALE)
#define UI_PAD 8.0f
#define UI_GAP 5.0f
#define UI_ROW 22.0f

/* Palette (opaque). */
static const float COL_PANEL[3] = {0.10f, 0.10f, 0.13f};
static const float COL_WIDGET[3] = {0.24f, 0.24f, 0.30f};
static const float COL_HOT[3] = {0.34f, 0.34f, 0.42f};
static const float COL_ACTIVE[3] = {0.20f, 0.45f, 0.72f};
static const float COL_FILL[3] = {0.28f, 0.52f, 0.82f};
static const float COL_TEXT[3] = {0.90f, 0.90f, 0.92f};
static const float COL_CHECK[3] = {0.45f, 0.85f, 0.55f};

static bool point_in(float px, float py, float x, float y, float w, float h) {
    return px >= x && px <= x + w && py >= y && py <= y + h;
}

static float text_width(const char *s) {
    return (float)strlen(s) * UI_GLYPH_W;
}

void ui_init(ui_t *ui) {
    memset(ui, 0, sizeof(*ui));
}

void ui_begin(ui_t *ui, const ui_input_t *in, float screen_w,
              float screen_h) {
    ui->in = *in;
    ui->went_down = in->mouse_down && !ui->prev_down;
    ui->went_up = !in->mouse_down && ui->prev_down;
    ui->prev_down = in->mouse_down;
    ui->screen_w = screen_w;
    ui->screen_h = screen_h;

    ui->hot_id = 0;
    ui->id_counter = 0;
    ui->pointer_in_panel = false;
    ui->panel_index = 0;
}

void ui_end(ui_t *ui) {
    /* Safety net: if the button was released this frame but the active widget
       was not emitted (e.g. its panel collapsed), no widget cleared active_id.
       Do it here — after every widget has had a chance to observe went_up, so
       button/checkbox clicks aren't swallowed before they register. */
    if (ui->active_id != 0 && !ui->in.mouse_down) {
        ui->active_id = 0;
    }
}

bool ui_wants_mouse(const ui_t *ui) {
    return ui->pointer_in_panel || ui->active_id != 0;
}

void ui_panel_begin(ui_t *ui, float x, float y, float w) {
    ui->in_panel = true;
    ui->panel_x = x;
    ui->panel_y = y;
    ui->panel_w = w;
    ui->cursor_y = y + UI_PAD;

    float h = ui->panel_height[ui->panel_index]; /* from last frame */
    if (h <= 0.0f) {
        h = UI_ROW; /* first frame: self-corrects next frame */
    }
    render_rect(x, y, w, h, COL_PANEL[0], COL_PANEL[1], COL_PANEL[2]);

    if (ui->in.pointer_valid &&
        point_in(ui->in.mouse_x, ui->in.mouse_y, x, y, w, h)) {
        ui->pointer_in_panel = true;
    }
}

void ui_panel_end(ui_t *ui) {
    if (ui->panel_index < UI_MAX_PANELS) {
        ui->panel_height[ui->panel_index] = ui->cursor_y - ui->panel_y;
    }
    ui->panel_index++;
    ui->in_panel = false;
}

/* Claim the next row rectangle inside the current panel and advance the cursor. */
static void next_row(ui_t *ui, float *x, float *y, float *w, float *h) {
    *x = ui->panel_x + UI_PAD;
    *y = ui->cursor_y;
    *w = ui->panel_w - 2.0f * UI_PAD;
    *h = UI_ROW;
    ui->cursor_y += UI_ROW + UI_GAP;
}

static bool widget_over(ui_t *ui, float x, float y, float w, float h) {
    return ui->in.pointer_valid &&
           point_in(ui->in.mouse_x, ui->in.mouse_y, x, y, w, h);
}

static void draw_label(float x, float y, float row_h, const char *s) {
    float ty = y + (row_h - UI_GLYPH_H) * 0.5f;
    render_text(x, ty, UI_SCALE, COL_TEXT[0], COL_TEXT[1], COL_TEXT[2], s);
}

void ui_text(ui_t *ui, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    float x, y, w, h;
    next_row(ui, &x, &y, &w, &h);
    draw_label(x, y, h, buf);
}

bool ui_button(ui_t *ui, const char *label) {
    int id = ++ui->id_counter;
    float x, y, w, h;
    next_row(ui, &x, &y, &w, &h);

    bool over = widget_over(ui, x, y, w, h);
    if (over) {
        ui->hot_id = id;
    }
    bool clicked = false;
    if (ui->active_id == id) {
        if (ui->went_up) {
            if (over) {
                clicked = true;
            }
            ui->active_id = 0;
        }
    } else if (over && ui->went_down) {
        ui->active_id = id;
    }

    const float *c = (ui->active_id == id) ? COL_ACTIVE
                     : over               ? COL_HOT
                                          : COL_WIDGET;
    render_rect(x, y, w, h, c[0], c[1], c[2]);
    draw_label(x + (w - text_width(label)) * 0.5f, y, h, label);
    return clicked;
}

bool ui_checkbox(ui_t *ui, const char *label, bool *value) {
    int id = ++ui->id_counter;
    float x, y, w, h;
    next_row(ui, &x, &y, &w, &h);

    bool over = widget_over(ui, x, y, w, h); /* whole row toggles */
    if (over) {
        ui->hot_id = id;
    }
    bool changed = false;
    if (ui->active_id == id) {
        if (ui->went_up) {
            if (over) {
                *value = !*value;
                changed = true;
            }
            ui->active_id = 0;
        }
    } else if (over && ui->went_down) {
        ui->active_id = id;
    }

    float box = h - 6.0f;
    float bx = x + 2.0f;
    float by = y + 3.0f;
    const float *c = over ? COL_HOT : COL_WIDGET;
    render_rect(bx, by, box, box, c[0], c[1], c[2]);
    if (*value) {
        float in = 4.0f;
        render_rect(bx + in, by + in, box - 2.0f * in, box - 2.0f * in,
                    COL_CHECK[0], COL_CHECK[1], COL_CHECK[2]);
    }
    draw_label(bx + box + 8.0f, y, h, label);
    return changed;
}

bool ui_slider_float(ui_t *ui, const char *label, float *value, float min,
                     float max) {
    int id = ++ui->id_counter;
    float x, y, w, h;
    next_row(ui, &x, &y, &w, &h);

    bool over = widget_over(ui, x, y, w, h);
    if (over) {
        ui->hot_id = id;
    }
    if (over && ui->went_down) {
        ui->active_id = id;
    }
    bool changed = false;
    if (ui->active_id == id) {
        float t = (ui->in.mouse_x - x) / w;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        float nv = min + t * (max - min);
        if (nv != *value) {
            *value = nv;
            changed = true;
        }
        if (ui->went_up) {
            ui->active_id = 0;
        }
    }

    float t = (max > min) ? (*value - min) / (max - min) : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float *track = over || ui->active_id == id ? COL_HOT : COL_WIDGET;
    render_rect(x, y, w, h, track[0], track[1], track[2]);
    render_rect(x, y, w * t, h, COL_FILL[0], COL_FILL[1], COL_FILL[2]);

    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %.2f", label, (double)*value);
    draw_label(x + 6.0f, y, h, buf);
    return changed;
}
