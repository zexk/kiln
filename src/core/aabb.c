#include "aabb.h"
#include <math.h>
#include <float.h>

aabb_t aabb_from_minmax(vec3_t min, vec3_t max) {
    return (aabb_t){ min, max };
}

aabb_t aabb_from_center(vec3_t center, vec3_t half_extents) {
    return (aabb_t){
        .min = vec3_sub(center, half_extents),
        .max = vec3_add(center, half_extents),
    };
}

bool aabb_contains_point(const aabb_t *box, vec3_t p) {
    return p.x >= box->min.x && p.x <= box->max.x &&
           p.y >= box->min.y && p.y <= box->max.y &&
           p.z >= box->min.z && p.z <= box->max.z;
}

bool aabb_intersects(const aabb_t *a, const aabb_t *b) {
    return a->min.x <= b->max.x && a->max.x >= b->min.x &&
           a->min.y <= b->max.y && a->max.y >= b->min.y &&
           a->min.z <= b->max.z && a->max.z >= b->min.z;
}

bool aabb_ray_intersect(const aabb_t *box, vec3_t origin, vec3_t dir, float *t_out) {
    float tmin = -FLT_MAX, tmax = FLT_MAX;
    float ds[3]   = { dir.x,    dir.y,    dir.z    };
    float os[3]   = { origin.x, origin.y, origin.z };
    float mins[3] = { box->min.x, box->min.y, box->min.z };
    float maxs[3] = { box->max.x, box->max.y, box->max.z };

    for (int i = 0; i < 3; i++) {
        if (fabsf(ds[i]) < 1e-8f) {
            if (os[i] < mins[i] || os[i] > maxs[i]) return false;
        } else {
            float inv = 1.0f / ds[i];
            float t0 = (mins[i] - os[i]) * inv;
            float t1 = (maxs[i] - os[i]) * inv;
            if (t0 > t1) { float tmp = t0; t0 = t1; t1 = tmp; }
            if (t0 > tmin) tmin = t0;
            if (t1 < tmax) tmax = t1;
            if (tmin > tmax) return false;
        }
    }

    if (tmax < 0.0f) return false;
    *t_out = tmin >= 0.0f ? tmin : tmax;
    return true;
}

aabb_t aabb_expand(const aabb_t *box, vec3_t p) {
    return (aabb_t){
        .min = { fminf(box->min.x, p.x), fminf(box->min.y, p.y), fminf(box->min.z, p.z) },
        .max = { fmaxf(box->max.x, p.x), fmaxf(box->max.y, p.y), fmaxf(box->max.z, p.z) },
    };
}

aabb_t aabb_merge(const aabb_t *a, const aabb_t *b) {
    return (aabb_t){
        .min = { fminf(a->min.x, b->min.x), fminf(a->min.y, b->min.y), fminf(a->min.z, b->min.z) },
        .max = { fmaxf(a->max.x, b->max.x), fmaxf(a->max.y, b->max.y), fmaxf(a->max.z, b->max.z) },
    };
}
