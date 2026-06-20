#include "ecs.h"
#include <stdlib.h>
#include <string.h>

void ecs_init(ECS *ecs, int max_entities) {
    ecs->max_entities = max_entities;
    ecs->masks    = calloc((size_t)max_entities, sizeof(uint32_t));
    ecs->alive    = calloc((size_t)max_entities, sizeof(bool));
    ecs->free_list = calloc((size_t)max_entities, sizeof(int));
    ecs->free_count = 0;
    ecs->comp_count = 0;
    memset(ecs->comp_sizes, 0, sizeof(ecs->comp_sizes));
    memset(ecs->comp_data,  0, sizeof(ecs->comp_data));
}

void ecs_shutdown(ECS *ecs) {
    free(ecs->masks);
    free(ecs->alive);
    free(ecs->free_list);
    for (int i = 0; i < ecs->comp_count; i++) {
        free(ecs->comp_data[i]);
    }
    ecs->max_entities = 0;
    ecs->comp_count = 0;
}

int ecs_register(ECS *ecs, size_t comp_size) {
    if (ecs->comp_count >= ECS_MAX_COMPONENTS) return -1;
    int id = ecs->comp_count++;
    ecs->comp_sizes[id] = comp_size;
    ecs->comp_data[id] = calloc((size_t)ecs->max_entities, comp_size);
    return id;
}

Entity ecs_spawn(ECS *ecs) {
    int idx;
    if (ecs->free_count > 0) {
        idx = ecs->free_list[--ecs->free_count];
    } else {
        static int next_idx = 1; /* 0 reserved for ECS_NULL */
        if (next_idx >= ecs->max_entities) return ECS_NULL;
        idx = next_idx++;
    }
    ecs->alive[idx] = true;
    ecs->masks[idx] = 0;
    return (Entity)idx;
}

void ecs_despawn(ECS *ecs, Entity e) {
    if (e >= (Entity)ecs->max_entities || !ecs->alive[e]) return;
    ecs->alive[e] = false;
    ecs->masks[e] = 0;
    if (ecs->free_count < ecs->max_entities) {
        ecs->free_list[ecs->free_count++] = (int)e;
    }
}

bool ecs_alive(const ECS *ecs, Entity e) {
    if (e >= (Entity)ecs->max_entities) return false;
    return ecs->alive[e];
}

void* ecs_add(ECS *ecs, Entity e, int comp_id) {
    if (e >= (Entity)ecs->max_entities || comp_id < 0 || comp_id >= ecs->comp_count) return NULL;
    if (!ecs->alive[e]) return NULL;
    ecs->masks[e] |= (1u << (uint32_t)comp_id);
    return ecs->comp_data[comp_id] + (e * ecs->comp_sizes[comp_id]);
}

void* ecs_get(const ECS *ecs, Entity e, int comp_id) {
    if (e >= (Entity)ecs->max_entities || comp_id < 0 || comp_id >= ecs->comp_count) return NULL;
    if (!ecs->alive[e]) return NULL;
    if (!(ecs->masks[e] & (1u << (uint32_t)comp_id))) return NULL;
    return ecs->comp_data[comp_id] + (e * ecs->comp_sizes[comp_id]);
}

void ecs_remove(ECS *ecs, Entity e, int comp_id) {
    if (e >= (Entity)ecs->max_entities || comp_id < 0 || comp_id >= ecs->comp_count) return;
    ecs->masks[e] &= ~(1u << (uint32_t)comp_id);
}

bool ecs_has(const ECS *ecs, Entity e, int comp_id) {
    if (e >= (Entity)ecs->max_entities || comp_id < 0 || comp_id >= ecs->comp_count) return false;
    if (!ecs->alive[e]) return false;
    return (ecs->masks[e] & (1u << (uint32_t)comp_id)) != 0;
}

int ecs_count(const ECS *ecs) {
    int n = 0;
    for (int i = 0; i < ecs->max_entities; i++) {
        if (ecs->alive[i]) n++;
    }
    return n;
}
