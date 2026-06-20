#pragma once
#include <stdbool.h>
#include "linalg.h"

typedef struct {
    vec3_t min;
    vec3_t max;
} aabb_t;

aabb_t aabb_from_minmax(vec3_t min, vec3_t max);
aabb_t aabb_from_center(vec3_t center, vec3_t half_extents);
bool   aabb_contains_point(const aabb_t *box, vec3_t point);
bool   aabb_intersects(const aabb_t *a, const aabb_t *b);
bool   aabb_ray_intersect(const aabb_t *box, vec3_t origin, vec3_t dir, float *t_out);
aabb_t aabb_expand(const aabb_t *box, vec3_t point);
aabb_t aabb_merge(const aabb_t *a, const aabb_t *b);
