#include "archetype.h"

#include <stdlib.h>
#include <string.h>

#include "world.h"
#include "core.h"

archetype_t *archetype_create(world_t *world, signature_t signature) {
    /* the struct and its column-pointer array are create-once and live for the
       world's lifetime, so they come from the world arena, not the heap */
    archetype_t *a = arena_calloc(world->arena, 1, sizeof(archetype_t));
    a->signature = signature;

    for (component_id_t id = 0; id < world->component_count; id++) {
        if (signature_has(&signature, id)) {
            a->component_ids[a->component_count++] = id;
        }
    }

    if (a->component_count > 0) {
        a->columns = arena_calloc(world->arena, a->component_count, sizeof(void *));
    }

    return a;
}

void archetype_destroy(archetype_t *archetype) {
    /* only the per-row growable buffers are heap-backed; the struct and the
       columns pointer array are owned by the world arena */
    for (uint32_t i = 0; i < archetype->component_count; i++) {
        free(archetype->columns[i]);
    }
    free(archetype->entities);
}

int archetype_column_index(const archetype_t *archetype, component_id_t id) {
    /* component_ids is sorted ascending; linear scan is fine at this size */
    for (uint32_t i = 0; i < archetype->component_count; i++) {
        if (archetype->component_ids[i] == id) {
            return (int)i;
        }
    }
    return -1;
}

static void archetype_grow(world_t *world, archetype_t *archetype, uint32_t min_capacity) {
    if (archetype->capacity >= min_capacity) {
        return;
    }

    uint32_t new_capacity = archetype->capacity ? archetype->capacity * 2 : 8;
    while (new_capacity < min_capacity) {
        new_capacity *= 2;
    }

    archetype->entities = realloc(archetype->entities, new_capacity * sizeof(entity_t));
    CORE_CHECK_ALLOC(archetype->entities);

    for (uint32_t i = 0; i < archetype->component_count; i++) {
        component_id_t id = archetype->component_ids[i];
        size_t size = world->components[id].size;
        archetype->columns[i] = realloc(archetype->columns[i], (size_t)new_capacity * size);
        CORE_CHECK_ALLOC(archetype->columns[i]);
    }

    archetype->capacity = new_capacity;
}

uint32_t archetype_add_row(world_t *world, archetype_t *archetype, entity_t entity) {
    archetype_grow(world, archetype, archetype->count + 1);
    uint32_t row = archetype->count++;
    archetype->entities[row] = entity;
    return row;
}

entity_t archetype_remove_row(world_t *world, archetype_t *archetype, uint32_t row) {
    uint32_t last = archetype->count - 1;
    entity_t moved = ECS_ENTITY_NULL;

    if (row != last) {
        moved = archetype->entities[last];
        archetype->entities[row] = moved;

        for (uint32_t i = 0; i < archetype->component_count; i++) {
            size_t size = world->components[archetype->component_ids[i]].size;
            char *col = archetype->columns[i];
            memcpy(col + (size_t)row * size, col + (size_t)last * size, size);
        }
    }

    archetype->count--;
    return moved;
}
