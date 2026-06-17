#include "camera.h"

#include <math.h>

#define ORBIT_SENSITIVITY 0.006f      /* radians per pixel */
#define PAN_SENSITIVITY 0.0015f       /* world units per pixel, per unit dist */
#define ZOOM_FACTOR 0.9f              /* distance multiplier per wheel notch */
#define PITCH_LIMIT 1.55334f          /* ~89 degrees */
#define MIN_DISTANCE 0.5f
#define MAX_DISTANCE 500.0f

static const vec3_t WORLD_UP = {0.0f, 1.0f, 0.0f};

void camera_init(camera_t *cam) {
    cam->target = (vec3_t){0.0f, 0.0f, 0.0f};
    cam->yaw = kln_radians(45.0f);
    cam->pitch = kln_radians(30.0f);
    cam->distance = 14.0f;
    cam->fov = kln_radians(60.0f);
    cam->near = 0.1f;
    cam->far = 200.0f;
    cam->orbiting = false;
    cam->panning = false;
}

/* Unit vector pointing from the target out to the eye. */
static vec3_t orbit_offset(const camera_t *cam) {
    float cp = cosf(cam->pitch);
    return (vec3_t){cp * sinf(cam->yaw), sinf(cam->pitch), cp * cosf(cam->yaw)};
}

vec3_t camera_eye(const camera_t *cam) {
    return vec3_add(cam->target, vec3_scale(orbit_offset(cam), cam->distance));
}

mat4_t camera_view(const camera_t *cam) {
    return mat4_look_at(camera_eye(cam), cam->target, WORLD_UP);
}

mat4_t camera_proj(const camera_t *cam, float aspect) {
    return mat4_perspective(cam->fov, aspect, cam->near, cam->far);
}

void camera_ray(const camera_t *cam, float aspect, float screen_x,
                float screen_y, float screen_w, float screen_h,
                vec3_t *out_origin, vec3_t *out_dir) {
    /* Invert the same view-projection the renderer draws with. Pixel -> NDC uses
       the renderer's screen flip (+y down), so it round-trips the gizmo's
       world->screen project(). Vulkan clip depth is [0,1]: near z=0, far z=1. */
    mat4_t inv =
        mat4_inverse(mat4_mul(camera_proj(cam, aspect), camera_view(cam)));
    float ndc_x = 2.0f * screen_x / screen_w - 1.0f;
    float ndc_y = 1.0f - 2.0f * screen_y / screen_h;

    vec4_t nh = mat4_mul_vec4(inv, (vec4_t){ndc_x, ndc_y, 0.0f, 1.0f});
    vec4_t fh = mat4_mul_vec4(inv, (vec4_t){ndc_x, ndc_y, 1.0f, 1.0f});
    vec3_t near = vec3_scale((vec3_t){nh.x, nh.y, nh.z}, 1.0f / nh.w);
    vec3_t far = vec3_scale((vec3_t){fh.x, fh.y, fh.z}, 1.0f / fh.w);

    *out_origin = near;
    *out_dir = vec3_normalize(vec3_sub(far, near));
}

bool camera_project(const camera_t *cam, float aspect, float sw, float sh,
                    vec3_t world, float *out_x, float *out_y) {
    mat4_t vp = mat4_mul(camera_proj(cam, aspect), camera_view(cam));
    vec4_t c = mat4_mul_vec4(vp, (vec4_t){world.x, world.y, world.z, 1.0f});
    if (c.w <= 1e-5f) {
        return false;
    }
    *out_x = (c.x / c.w * 0.5f + 0.5f) * sw;
    *out_y = (1.0f - (c.y / c.w * 0.5f + 0.5f)) * sh;
    return true;
}

static void orbit(camera_t *cam, int dx, int dy) {
    cam->yaw -= (float)dx * ORBIT_SENSITIVITY;
    cam->pitch += (float)dy * ORBIT_SENSITIVITY;
    cam->pitch = kln_clampf(cam->pitch, -PITCH_LIMIT, PITCH_LIMIT);
}

/* Slide the target across the camera's screen plane so the scene tracks the
   cursor (grab-pan). Scaled by distance so the feel is constant at any zoom. */
static void pan(camera_t *cam, int dx, int dy) {
    vec3_t forward = vec3_normalize(vec3_neg(orbit_offset(cam)));
    vec3_t right = vec3_normalize(vec3_cross(forward, WORLD_UP));
    vec3_t up = vec3_cross(right, forward);
    float k = PAN_SENSITIVITY * cam->distance;
    vec3_t move = vec3_add(vec3_scale(right, -(float)dx * k),
                           vec3_scale(up, (float)dy * k));
    cam->target = vec3_add(cam->target, move);
}

bool camera_is_navigating(const camera_t *cam) {
    return cam->orbiting || cam->panning;
}

void camera_handle_event(camera_t *cam, const event_t *ev) {
    switch (ev->type) {
    case EVENT_BUTTON_DOWN:
        if (ev->button.button == MOUSE_BUTTON_LEFT) {
            cam->orbiting = true;
        } else if (ev->button.button == MOUSE_BUTTON_RIGHT) {
            cam->panning = true;
        }
        break;
    case EVENT_BUTTON_UP:
        if (ev->button.button == MOUSE_BUTTON_LEFT) {
            cam->orbiting = false;
        } else if (ev->button.button == MOUSE_BUTTON_RIGHT) {
            cam->panning = false;
        }
        break;
    case EVENT_MOUSE_MOVE:
        if (cam->orbiting) {
            orbit(cam, ev->motion.dx, ev->motion.dy);
        }
        if (cam->panning) {
            pan(cam, ev->motion.dx, ev->motion.dy);
        }
        break;
    case EVENT_SCROLL:
        cam->distance *= (ev->scroll.delta > 0.0f) ? ZOOM_FACTOR
                                                   : 1.0f / ZOOM_FACTOR;
        cam->distance = kln_clampf(cam->distance, MIN_DISTANCE, MAX_DISTANCE);
        break;
    default:
        break;
    }
}
