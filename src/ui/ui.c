#include "ui.h"

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
              float screen_h, const ui_draw_t *draw) {
    ui->draw = *draw;
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
    ui->draw.rect(ui->draw.userdata, x, y, w, h, COL_PANEL[0], COL_PANEL[1], COL_PANEL[2]);

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

static void draw_label(ui_t *ui, float x, float y, float row_h, const char *s) {
    float ty = y + (row_h - UI_GLYPH_H) * 0.5f;
    ui->draw.text(ui->draw.userdata, x, ty, UI_SCALE, COL_TEXT[0], COL_TEXT[1], COL_TEXT[2], s);
}

void ui_text(ui_t *ui, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    float x, y, w, h;
    next_row(ui, &x, &y, &w, &h);
    draw_label(ui, x, y, h, buf);
}

bool ui_button(ui_t *ui, const char *label) {
    int id = ++ui->id_counter;
    float x, y, w, h;
    next_row(ui, &x, &y, &w, &h);

    bool over = widget_over(ui, x, y, w, h);
    if (over) ui->hot_id = id;
    bool clicked = false;
    if (ui->active_id == id) {
        if (ui->went_up) {
            if (over) clicked = true;
            ui->active_id = 0;
        }
    } else if (over && ui->went_down) {
        ui->active_id = id;
    }

    const float *c = (ui->active_id == id) ? COL_ACTIVE
                     : over                ? COL_HOT
                                           : COL_WIDGET;
    ui->draw.rect(ui->draw.userdata, x, y, w, h, c[0], c[1], c[2]);
    draw_label(ui, x + (w - text_width(label)) * 0.5f, y, h, label);
    return clicked;
}

bool ui_checkbox(ui_t *ui, const char *label, bool *value) {
    int id = ++ui->id_counter;
    float x, y, w, h;
    next_row(ui, &x, &y, &w, &h);

    bool over = widget_over(ui, x, y, w, h);
    if (over) ui->hot_id = id;
    bool changed = false;
    if (ui->active_id == id) {
        if (ui->went_up) {
            if (over) { *value = !*value; changed = true; }
            ui->active_id = 0;
        }
    } else if (over && ui->went_down) {
        ui->active_id = id;
    }

    float box = h - 6.0f;
    float bx = x + 2.0f;
    float by = y + 3.0f;
    const float *c = over ? COL_HOT : COL_WIDGET;
    ui->draw.rect(ui->draw.userdata, bx, by, box, box, c[0], c[1], c[2]);
    if (*value) {
        float in = 4.0f;
        ui->draw.rect(ui->draw.userdata, bx + in, by + in, box - 2.0f * in, box - 2.0f * in,
                      COL_CHECK[0], COL_CHECK[1], COL_CHECK[2]);
    }
    draw_label(ui, bx + box + 8.0f, y, h, label);
    return changed;
}

void ui_separator(ui_t *ui) {
    float x = ui->panel_x + UI_PAD;
    float y = ui->cursor_y + 4.0f;
    float w = ui->panel_w - 2.0f * UI_PAD;
    ui->draw.rect(ui->draw.userdata, x, y, w, 1.0f, 0.35f, 0.35f, 0.40f);
    ui->cursor_y += 10.0f;
}

bool ui_slider_float(ui_t *ui, const char *label, float *value, float min,
                     float max) {
    int id = ++ui->id_counter;
    float x, y, w, h;
    next_row(ui, &x, &y, &w, &h);

    bool over = widget_over(ui, x, y, w, h);
    if (over) ui->hot_id = id;
    if (over && ui->went_down) ui->active_id = id;
    bool changed = false;
    if (ui->active_id == id) {
        float t = (ui->in.mouse_x - x) / w;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        float nv = min + t * (max - min);
        if (nv != *value) { *value = nv; changed = true; }
        if (ui->went_up) ui->active_id = 0;
    }

    float t = (max > min) ? (*value - min) / (max - min) : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float *track = (over || ui->active_id == id) ? COL_HOT : COL_WIDGET;
    ui->draw.rect(ui->draw.userdata, x, y, w, h, track[0], track[1], track[2]);
    ui->draw.rect(ui->draw.userdata, x, y, w * t, h, COL_FILL[0], COL_FILL[1], COL_FILL[2]);

    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %.2f", label, (double)*value);
    draw_label(ui, x + 6.0f, y, h, buf);
    return changed;
}

bool ui_slider_int(ui_t *ui, const char *label, int *value, int min, int max) {
    int id = ++ui->id_counter;
    float x, y, w, h;
    next_row(ui, &x, &y, &w, &h);

    bool over = widget_over(ui, x, y, w, h);
    if (over) ui->hot_id = id;
    if (over && ui->went_down) ui->active_id = id;
    bool changed = false;
    if (ui->active_id == id) {
        float t = (ui->in.mouse_x - x) / w;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        int nv = min + (int)(t * (float)(max - min) + 0.5f);
        if (nv < min) nv = min;
        if (nv > max) nv = max;
        if (nv != *value) { *value = nv; changed = true; }
        if (ui->went_up) ui->active_id = 0;
    }

    float t = (max > min) ? (float)(*value - min) / (float)(max - min) : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float *track = (over || ui->active_id == id) ? COL_HOT : COL_WIDGET;
    ui->draw.rect(ui->draw.userdata, x, y, w, h, track[0], track[1], track[2]);
    ui->draw.rect(ui->draw.userdata, x, y, w * t, h, COL_FILL[0], COL_FILL[1], COL_FILL[2]);

    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %d", label, *value);
    draw_label(ui, x + 6.0f, y, h, buf);
    return changed;
}

void ui_progress(ui_t *ui, const char *label, float value, float max) {
    float x, y, w, h;
    next_row(ui, &x, &y, &w, &h);

    float t = (max > 0.0f) ? value / max : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    ui->draw.rect(ui->draw.userdata, x, y, w, h, COL_WIDGET[0], COL_WIDGET[1], COL_WIDGET[2]);
    ui->draw.rect(ui->draw.userdata, x, y, w * t, h, COL_FILL[0], COL_FILL[1], COL_FILL[2]);

    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %.0f/%.0f", label, (double)value, (double)max);
    draw_label(ui, x + 6.0f, y, h, buf);
}

#define UI_GRAPH_H (2.0f * UI_ROW + UI_GAP)

void ui_graph(ui_t *ui, const char *label,
              const float *samples, int count, int head,
              float max, float target) {
    if (count <= 0 || max <= 0.0f) return;

    float x  = ui->panel_x + UI_PAD;
    float y  = ui->cursor_y;
    float w  = ui->panel_w - 2.0f * UI_PAD;
    float gh = UI_GRAPH_H;
    ui->cursor_y += gh + UI_GAP;

    ui->draw.rect(ui->draw.userdata, x, y, w, gh,
                  COL_WIDGET[0], COL_WIDGET[1], COL_WIDGET[2]);

    float bw = w / (float)count;
    for (int i = 0; i < count; i++) {
        float v = samples[(head + i) % count];
        float t = v / max;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        if (t == 0.0f) continue;

        float bh = t * gh;
        float cr = t < 0.5f ? t * 2.0f : 1.0f;
        float cg = t < 0.5f ? 1.0f : 1.0f - (t - 0.5f) * 2.0f;
        ui->draw.rect(ui->draw.userdata,
                      x + (float)i * bw, y + gh - bh, bw, bh, cr, cg, 0.0f);
    }

    if (target > 0.0f && target < max) {
        float ly = y + gh * (1.0f - target / max);
        ui->draw.rect(ui->draw.userdata, x, ly, w, 1.0f, 0.55f, 0.55f, 0.60f);
    }

    float latest = samples[(head + count - 1) % count];
    char buf[64];
    snprintf(buf, sizeof(buf), "%s: %.1f", label, (double)latest);
    ui->draw.text(ui->draw.userdata, x + 3.0f, y + 3.0f, UI_SCALE,
                  COL_TEXT[0], COL_TEXT[1], COL_TEXT[2], buf);
}

bool ui_selectable(ui_t *ui, const char *label, bool selected) {
    int id = ++ui->id_counter;
    float x, y, w, h;
    next_row(ui, &x, &y, &w, &h);

    bool over = widget_over(ui, x, y, w, h);
    if (over) ui->hot_id = id;
    bool clicked = false;
    if (ui->active_id == id) {
        if (ui->went_up) {
            if (over) clicked = true;
            ui->active_id = 0;
        }
    } else if (over && ui->went_down) {
        ui->active_id = id;
    }

    const float *c = (ui->active_id == id && over) ? COL_ACTIVE
                     : selected                     ? COL_FILL
                     : over                         ? COL_HOT
                                                    : COL_WIDGET;
    ui->draw.rect(ui->draw.userdata, x, y, w, h, c[0], c[1], c[2]);
    draw_label(ui, x + 6.0f, y, h, label);
    return clicked;
}

bool ui_drag_float(ui_t *ui, const char *label, float *value, float speed) {
    int id = ++ui->id_counter;
    float x, y, w, h;
    next_row(ui, &x, &y, &w, &h);

    bool over = widget_over(ui, x, y, w, h);
    if (over) ui->hot_id = id;
    bool changed = false;
    if (ui->active_id == id) {
        float nv = ui->drag_ref_val + (ui->in.mouse_x - ui->drag_ref_x) * speed;
        if (nv != *value) { *value = nv; changed = true; }
        if (ui->went_up) ui->active_id = 0;
    } else if (over && ui->went_down) {
        ui->active_id   = id;
        ui->drag_ref_x   = ui->in.mouse_x;
        ui->drag_ref_val = *value;
    }

    const float *c = (ui->active_id == id) ? COL_ACTIVE : over ? COL_HOT : COL_WIDGET;
    ui->draw.rect(ui->draw.userdata, x, y, w, h, c[0], c[1], c[2]);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %.3f", label, (double)*value);
    draw_label(ui, x + 6.0f, y, h, buf);
    return changed;
}

bool ui_input_int(ui_t *ui, const char *label, int *value, int step) {
    int id_m = ++ui->id_counter;
    int id_p = ++ui->id_counter;
    float x, y, w, h;
    next_row(ui, &x, &y, &w, &h);

    float bw  = h;
    float mx  = x + w - 2.0f * bw - 4.0f;
    float px  = x + w - bw;
    float fw  = mx - x;

    ui->draw.rect(ui->draw.userdata, x, y, fw, h, COL_WIDGET[0], COL_WIDGET[1], COL_WIDGET[2]);

    bool over_m = widget_over(ui, mx, y, bw, h);
    if (over_m) ui->hot_id = id_m;
    bool clicked = false;
    if (ui->active_id == id_m) {
        if (ui->went_up) {
            if (over_m) { *value -= step; clicked = true; }
            ui->active_id = 0;
        }
    } else if (over_m && ui->went_down) {
        ui->active_id = id_m;
    }
    const float *cm = (ui->active_id == id_m) ? COL_ACTIVE : over_m ? COL_HOT : COL_WIDGET;
    ui->draw.rect(ui->draw.userdata, mx, y, bw, h, cm[0], cm[1], cm[2]);
    draw_label(ui, mx + (bw - UI_GLYPH_W) * 0.5f, y, h, "-");

    bool over_p = widget_over(ui, px, y, bw, h);
    if (over_p) ui->hot_id = id_p;
    if (ui->active_id == id_p) {
        if (ui->went_up) {
            if (over_p) { *value += step; clicked = true; }
            ui->active_id = 0;
        }
    } else if (over_p && ui->went_down) {
        ui->active_id = id_p;
    }
    const float *cp = (ui->active_id == id_p) ? COL_ACTIVE : over_p ? COL_HOT : COL_WIDGET;
    ui->draw.rect(ui->draw.userdata, px, y, bw, h, cp[0], cp[1], cp[2]);
    draw_label(ui, px + (bw - UI_GLYPH_W) * 0.5f, y, h, "+");

    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %d", label, *value);
    draw_label(ui, x + 6.0f, y, h, buf);
    return clicked;
}
