#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "linalg.h"
#include "platform.h"

/* Editor-style orbit camera: the eye sits on a sphere around `target`, posed by
   yaw/pitch and pulled in/out by `distance`. Driven entirely by mouse input —
   LMB drag orbits, RMB drag pans the target, the wheel dollies. The camera is
   render-agnostic: it just produces view/projection matrices. */
typedef struct {
    vec3_t target; /* point the camera looks at and orbits around */
    float yaw;     /* radians, around world +Y */
    float pitch;   /* radians, clamped just shy of straight up/down */
    float distance;

    float fov;  /* vertical field of view, radians */
    float near; /* near plane */
    float far;  /* far plane */

    /* drag state */
    bool orbiting; /* left button held */
    bool panning;  /* right button held */
} camera_t;

void camera_init(camera_t *cam);

/* Feed every window event here; non-input events are ignored. Mouse motion is
   consumed as relative deltas, so it behaves identically whether the cursor is
   free or captured (CURSOR_DISABLED). */
void camera_handle_event(camera_t *cam, const event_t *ev);

/* True while a drag is in progress — the app uses this to capture the cursor. */
bool camera_is_navigating(const camera_t *cam);

vec3_t camera_eye(const camera_t *cam);
mat4_t camera_view(const camera_t *cam);
mat4_t camera_proj(const camera_t *cam, float aspect);

/* Build a world-space ray through a screen pixel (top-left origin, +y down — the
   same convention the renderer/gizmo use). The ray starts on the near plane and
   points into the scene; `out_dir` is unit length. Used for mouse picking. */
void camera_ray(const camera_t *cam, float aspect, float screen_x,
                float screen_y, float screen_w, float screen_h,
                vec3_t *out_origin, vec3_t *out_dir);
