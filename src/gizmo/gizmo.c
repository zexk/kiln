#include "gizmo.h"

#include "render.h"

#include <math.h>

#define GIZMO_PICK_PX 12.0f      /* cursor-to-axis distance to grab a handle */
#define GIZMO_SCREEN_FRAC 0.16f  /* axis length as a fraction of eye distance */
#define ROTATE_SENS 0.01f        /* radians per projected pixel */
#define SCALE_SENS 0.01f         /* fractional scale per projected pixel */

static const vec3_t AXIS_DIR[3] = {
    {1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 1.0f},
};

/* Dim / bright colour per axis (X red, Y green, Z blue). */
static const float AXIS_COL[3][3] = {
    {0.90f, 0.25f, 0.25f},
    {0.30f, 0.85f, 0.30f},
    {0.35f, 0.55f, 0.95f},
};

void gizmo_init(gizmo_t *g) {
    g->mode = GIZMO_MOVE;
    g->active_axis = -1;
    g->dragging = false;
    g->prev_down = false;
    g->last_x = 0.0f;
    g->last_y = 0.0f;
}


/* Distance from point p to segment ab, in 2D. */
static float seg_dist(float px, float py, float ax, float ay, float bx,
                      float by) {
    float dx = bx - ax;
    float dy = by - ay;
    float len2 = dx * dx + dy * dy;
    float t = (len2 > 1e-6f) ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    float cx = ax + t * dx;
    float cy = ay + t * dy;
    dx = px - cx;
    dy = py - cy;
    return sqrtf(dx * dx + dy * dy);
}

static void set_component(vec3_t *v, int axis, float value) {
    if (axis == 0) {
        v->x = value;
    } else if (axis == 1) {
        v->y = value;
    } else {
        v->z = value;
    }
}

static float get_component(const vec3_t *v, int axis) {
    return axis == 0 ? v->x : (axis == 1 ? v->y : v->z);
}

bool gizmo_update(gizmo_t *g, const camera_t *cam, float screen_w,
                  float screen_h, const gizmo_input_t *in, vec3_t *pos,
                  quat_t *rot, vec3_t *scale) {
    float aspect = screen_h > 0.0f ? screen_w / screen_h : 1.0f;

    /* Constant on-screen size: axis length scales with distance to the eye. */
    float length = vec3_distance(camera_eye(cam), *pos) * GIZMO_SCREEN_FRAC;

    float ox, oy;
    if (!camera_project(cam, aspect, screen_w, screen_h, *pos, &ox, &oy)) {
        return false; /* pivot behind camera */
    }

    float tip_x[3], tip_y[3];
    bool tip_ok[3];
    for (int i = 0; i < 3; i++) {
        vec3_t tip = vec3_add(*pos, vec3_scale(AXIS_DIR[i], length));
        tip_ok[i] = camera_project(cam, aspect, screen_w, screen_h, tip,
                                   &tip_x[i], &tip_y[i]);
    }

    bool down = in->mouse_down;
    bool went_down = down && !g->prev_down;
    bool went_up = !down && g->prev_down;
    g->prev_down = down;

    /* Hover test (skipped while already dragging). */
    int hover = -1;
    if (in->pointer_valid && !g->dragging) {
        float best = GIZMO_PICK_PX;
        for (int i = 0; i < 3; i++) {
            if (!tip_ok[i]) {
                continue;
            }
            float d =
                seg_dist(in->mouse_x, in->mouse_y, ox, oy, tip_x[i], tip_y[i]);
            if (d < best) {
                best = d;
                hover = i;
            }
        }
    }

    if (!g->dragging && went_down && hover >= 0 && in->pointer_valid) {
        g->dragging = true;
        g->active_axis = hover;
        g->last_x = in->mouse_x;
        g->last_y = in->mouse_y;
    }

    if (g->dragging) {
        int a = g->active_axis;
        float dx = in->mouse_x - g->last_x;
        float dy = in->mouse_y - g->last_y;
        g->last_x = in->mouse_x;
        g->last_y = in->mouse_y;

        /* Only apply movement when the active tip is in front of the camera.
           If it clips behind the near plane mid-drag, tip_x/y are
           uninitialized — freeze until the tip re-emerges, then release. */
        if (tip_ok[a]) {
            float sdx = tip_x[a] - ox;
            float sdy = tip_y[a] - oy;
            float slen = sqrtf(sdx * sdx + sdy * sdy);
            float pixels = (slen > 1e-3f) ? (dx * sdx + dy * sdy) / slen : 0.0f;

            if (g->mode == GIZMO_MOVE) {
                float world_per_px = (slen > 1e-3f) ? length / slen : 0.0f;
                *pos = vec3_add(*pos,
                                vec3_scale(AXIS_DIR[a], pixels * world_per_px));
            } else if (g->mode == GIZMO_ROTATE) {
                quat_t q = quat_from_axis_angle(AXIS_DIR[a], pixels * ROTATE_SENS);
                *rot = quat_normalize(quat_mul(q, *rot));
            } else { /* GIZMO_SCALE */
                float s = get_component(scale, a) * (1.0f + pixels * SCALE_SENS);
                set_component(scale, a, s < 0.02f ? 0.02f : s);
            }
        }

        if (went_up) {
            g->dragging = false;
            g->active_axis = -1;
        }
    }

    /* Draw: a line + tip handle per axis, brighter/thicker when hot. */
    for (int i = 0; i < 3; i++) {
        if (!tip_ok[i]) {
            continue;
        }
        bool hot = (g->dragging && g->active_axis == i) ||
                   (!g->dragging && hover == i);
        float thick = hot ? 5.0f : 3.0f;
        float k = hot ? 1.0f : 0.85f;
        float r = AXIS_COL[i][0] * k;
        float gr = AXIS_COL[i][1] * k;
        float b = AXIS_COL[i][2] * k;
        render_line(ox, oy, tip_x[i], tip_y[i], thick, r, gr, b);
        float hs = hot ? 6.0f : 4.0f;
        render_rect(tip_x[i] - hs, tip_y[i] - hs, hs * 2.0f, hs * 2.0f, r, gr,
                    b);
    }

    return g->dragging || hover >= 0;
}
