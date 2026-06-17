#pragma once

#include <stdbool.h>

#include "camera.h"
#include "linalg.h"

/* A crude screen-space transform manipulator for one selected object. Projects
   the object's three world axes to the screen, draws them via the renderer's 2D
   overlay, and drag-edits the supplied transform along the grabbed axis. Mode
   chooses what a drag does. Render- and ECS-agnostic: the caller passes plain
   pointers to the transform components being edited. */

typedef enum {
    GIZMO_MOVE,
    GIZMO_ROTATE,
    GIZMO_SCALE,
} gizmo_mode_t;

typedef struct {
    gizmo_mode_t mode;
    int active_axis; /* axis being dragged, -1 = none */
    bool dragging;
    bool prev_down;
    float last_x, last_y; /* pointer position at the previous drag step */
} gizmo_t;

typedef struct {
    float mouse_x, mouse_y;
    bool mouse_down;
    bool pointer_valid; /* false when another layer owns the mouse */
} gizmo_input_t;

void gizmo_init(gizmo_t *g);

/* Draw the gizmo for the object at *pos and, if a handle is grabbed, edit the
   transform in place (translation, rotation, or per-axis scale by mode).
   Returns true if the gizmo is hovering or dragging a handle — the caller then
   keeps the mouse away from the camera. */
bool gizmo_update(gizmo_t *g, const camera_t *cam, float screen_w,
                  float screen_h, const gizmo_input_t *in, vec3_t *pos,
                  quat_t *rot, vec3_t *scale);
