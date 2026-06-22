#pragma once
#include "linalg.h"
#include <stdbool.h>

/* Callback the caller provides to answer "is this voxel solid?".
   x/y/z are world-space integer voxel coordinates.
   ctx is the opaque pointer passed to phys_step. */
typedef bool (*phys_solid_fn)(void *ctx, int x, int y, int z);

/* Axis-aligned bounding box physics body.
   position  — reference point; foot and head offsets are measured from it.
   velocity  — world-space velocity in units/s.
   half_w    — AABB half-extent on the X and Z axes.
   foot_off  — distance below position.y to the bottom of the AABB.
   head_off  — distance above position.y to the top of the AABB.
   gravity   — downward acceleration in units/s² (set 0 to disable).
   grounded  — set true by phys_step when resting on a solid surface. */
typedef struct {
    vec3_t position;
    vec3_t velocity;
    float  half_w;
    float  foot_off;
    float  head_off;
    float  gravity;
    bool   grounded;
} phys_body_t;

/* Integrate the body by dt seconds against the solid-query function.
   Resolves axis-separated AABB collision on all three axes and updates
   grounded.  dt is internally clamped to 1/60 s to prevent tunnelling
   through thin walls on hitch frames. */
void phys_step(phys_body_t *body, float dt, phys_solid_fn solid, void *ctx);
