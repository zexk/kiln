#pragma once
#include <stdbool.h>
#include "linalg.h"

typedef struct { float a, b, c, d; } plane_t;
typedef struct { plane_t planes[6]; } frustum_t;

void frustum_extract(frustum_t *f, mat4_t vp);
bool frustum_intersects_aabb(const frustum_t *f, vec3_t min, vec3_t max);
