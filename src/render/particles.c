#include "particles.h"

#include <stdlib.h>
#include <string.h>

bool particle_emitter_init(particle_emitter_t *e,
                           mesh_handle_t mesh, material_handle_t material,
                           uint32_t capacity) {
    memset(e, 0, sizeof(*e));
    e->mesh      = mesh;
    e->material  = material;
    e->capacity  = capacity;

    e->pos      = calloc(capacity, sizeof(vec3_t));
    e->vel      = calloc(capacity, sizeof(vec3_t));
    e->life     = calloc(capacity, sizeof(float));
    e->max_life = calloc(capacity, sizeof(float));
    e->scale    = calloc(capacity, sizeof(float));
    e->scratch  = malloc(capacity * sizeof(mat4_t));

    if (!e->pos || !e->vel || !e->life || !e->max_life ||
        !e->scale || !e->scratch) {
        particle_emitter_free(e);
        return false;
    }
    e->gravity = (vec3_t){0.0f, -9.8f, 0.0f};
    return true;
}

void particle_emitter_free(particle_emitter_t *e) {
    free(e->pos);      e->pos      = NULL;
    free(e->vel);      e->vel      = NULL;
    free(e->life);     e->life     = NULL;
    free(e->max_life); e->max_life = NULL;
    free(e->scale);    e->scale    = NULL;
    free(e->scratch);  e->scratch  = NULL;
    e->count    = 0;
    e->capacity = 0;
}

void particle_emitter_emit(particle_emitter_t *e,
                           vec3_t pos, vec3_t vel,
                           float life, float scale) {
    if (e->count >= e->capacity) return;
    uint32_t i    = e->count++;
    e->pos[i]      = pos;
    e->vel[i]      = vel;
    e->life[i]     = life;
    e->max_life[i] = life;
    e->scale[i]    = scale;
}

void particle_emitter_update(particle_emitter_t *e, float dt) {
    /* Advance physics; compact dead particles. */
    uint32_t alive = 0;
    for (uint32_t i = 0; i < e->count; i++) {
        e->life[i] -= dt;
        if (e->life[i] <= 0.0f) continue; /* dead — skip (gap compacted below) */

        e->vel[i] = vec3_add(e->vel[i], vec3_scale(e->gravity, dt));
        e->pos[i] = vec3_add(e->pos[i], vec3_scale(e->vel[i], dt));

        /* Compact into alive slot. */
        if (alive != i) {
            e->pos[alive]      = e->pos[i];
            e->vel[alive]      = e->vel[i];
            e->life[alive]     = e->life[i];
            e->max_life[alive] = e->max_life[i];
            e->scale[alive]    = e->scale[i];
        }
        alive++;
    }
    e->count = alive;

    if (alive == 0) return;

    /* Build model matrices: uniform scale + translation. */
    for (uint32_t i = 0; i < alive; i++) {
        float s = e->scale[i];
        mat4_t m = mat4_identity();
        m.m[0]  = s;
        m.m[5]  = s;
        m.m[10] = s;
        m.m[12] = e->pos[i].x;
        m.m[13] = e->pos[i].y;
        m.m[14] = e->pos[i].z;
        e->scratch[i] = m;
    }

    render_mesh_instanced(e->mesh, e->material, e->scratch, alive);
}
