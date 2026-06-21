#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "linalg.h"
#include "render.h"

/* CPU particle emitter.  Particles are opaque mesh instances drawn via
   render_mesh_instanced — no new GPU pipeline required.  The caller owns the
   struct and calls particle_emitter_update once per frame to advance physics
   and submit the instanced draw.  Particles are billboarded via a uniform
   scale * translate transform (no orientation tracking). */

typedef struct {
    mesh_handle_t     mesh;
    material_handle_t material;

    /* alive particle state (parallel arrays, capacity elements each) */
    vec3_t *pos;
    vec3_t *vel;
    float  *life;      /* seconds remaining */
    float  *max_life;
    float  *scale;
    uint32_t count;
    uint32_t capacity;

    /* per-frame scratch buffer for instanced model matrices */
    mat4_t *scratch;

    vec3_t gravity;    /* acceleration applied every tick, e.g. (0,-9.8,0) */
} particle_emitter_t;

/* Allocate internal arrays.  Returns false on OOM. */
bool particle_emitter_init(particle_emitter_t *e,
                           mesh_handle_t mesh, material_handle_t material,
                           uint32_t capacity);
void particle_emitter_free(particle_emitter_t *e);

/* Spawn one particle.  Silently dropped when the emitter is full. */
void particle_emitter_emit(particle_emitter_t *e,
                           vec3_t pos, vec3_t vel,
                           float life, float scale);

/* Advance physics by dt seconds and submit a render_mesh_instanced draw for
   all live particles.  Call once per frame between render_set_camera and
   render_draw. */
void particle_emitter_update(particle_emitter_t *e, float dt);
