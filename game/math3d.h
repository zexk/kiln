#pragma once
/* Shim: maps kyub math3d names onto kiln's linalg/frustum API. */
#include "linalg.h"
#include "frustum.h"

typedef vec3_t vec3;
typedef mat4_t mat4;

#define PI KLN_PI

typedef frustum_t Frustum;
typedef plane_t   Plane;
