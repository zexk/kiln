#pragma once
#include "linalg.h"

/* First-person / free-look camera: yaw/pitch state + front vector.
   Pure math — no world, physics, or ECS dependency.  The caller is
   responsible for feeding mouse deltas and supplying a position. */
typedef struct {
    float yaw;         /* radians; 0 = looking along +X, grows CCW */
    float pitch;       /* radians; clamped just shy of ±π/2 */
    float sensitivity; /* radians per pixel of mouse delta */
    vec3_t front;      /* unit look direction, kept in sync with yaw/pitch */
    vec3_t up;         /* world up; (0,1,0) in the default init */
} fps_camera_t;

void   fps_camera_init(fps_camera_t *cam);

/* Apply a mouse delta (pixels).  Positive dx rotates right, positive dy
   looks down (screen-space convention matches CURSOR_DISABLED deltas). */
void   fps_camera_rotate(fps_camera_t *cam, float dx, float dy);

/* View matrix for the given world-space eye position. */
mat4_t fps_camera_view(const fps_camera_t *cam, vec3_t eye);

/* Build a right-handed orthonormal basis for the camera.
   `right` = cross(front, up), `up_out` = cross(right, front).
   Useful for WASD movement: forward = front, strafe = right. */
void   fps_camera_basis(const fps_camera_t *cam,
                        vec3_t *right, vec3_t *up_out);
