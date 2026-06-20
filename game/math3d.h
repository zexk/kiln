#pragma once
/* Shim: maps kyub math3d names onto kiln's linalg/frustum API. */
#include "linalg.h"
#include "frustum.h"

typedef vec3_t vec3;
typedef mat4_t mat4;

#define PI KLN_PI

/* vec3 function aliases */
#define vec3_mul            vec3_scale
#define mat4_multiply       mat4_mul
#define mat4_lookat         mat4_look_at
#define mat4_translate      mat4_translation

/* frustum aliases */
typedef frustum_t  Frustum;
typedef plane_t    Plane;
#define frustum_intersects_box  frustum_intersects_aabb
