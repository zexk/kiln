#include "fps_camera.h"
#include <math.h>

#define PITCH_LIMIT (KLN_PI * 0.5f - 0.001f)

static void recompute_front(fps_camera_t *cam) {
    cam->front = vec3_normalize((vec3_t){
        cosf(cam->yaw) * cosf(cam->pitch),
        sinf(cam->pitch),
        sinf(cam->yaw) * cosf(cam->pitch),
    });
}

void fps_camera_init(fps_camera_t *cam) {
    cam->yaw         = -KLN_PI * 0.5f; /* look along -Z initially */
    cam->pitch       = 0.0f;
    cam->sensitivity = 0.002f;         /* ~0.1 deg/px */
    cam->up          = (vec3_t){0.0f, 1.0f, 0.0f};
    recompute_front(cam);
}

void fps_camera_rotate(fps_camera_t *cam, float dx, float dy) {
    cam->yaw   += dx * cam->sensitivity;
    cam->pitch -= dy * cam->sensitivity; /* screen +y is down, pitch +y is up */
    if (cam->pitch >  PITCH_LIMIT) cam->pitch =  PITCH_LIMIT;
    if (cam->pitch < -PITCH_LIMIT) cam->pitch = -PITCH_LIMIT;
    recompute_front(cam);
}

mat4_t fps_camera_view(const fps_camera_t *cam, vec3_t eye) {
    return mat4_look_at(eye, vec3_add(eye, cam->front), cam->up);
}

void fps_camera_basis(const fps_camera_t *cam, vec3_t *right, vec3_t *up_out) {
    *right  = vec3_normalize(vec3_cross(cam->front, cam->up));
    *up_out = vec3_cross(*right, cam->front);
}
