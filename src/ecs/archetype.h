#pragma once

#include <stdint.h>

#include "entity.h"
#include "signature.h"

struct world;

typedef struct archetype {
    signature_t signature;
    component_id_t component_ids[ECS_MAX_COMPONENTS]; /* sorted ascending by id */
    uint32_t component_count;

    void **columns;    /* columns[i] holds component_ids[i]'s data, capacity * stride bytes */
    entity_t *entities; /* row -> entity */
    uint32_t count;
    uint32_t capacity;

    /* cached signature-transition graph, indexed by component_id_t */
    struct archetype *add_edge[ECS_MAX_COMPONENTS];
    struct archetype *remove_edge[ECS_MAX_COMPONENTS];
} archetype_t;

archetype_t *archetype_create(struct world *world, signature_t signature);
void archetype_destroy(archetype_t *archetype);

int archetype_column_index(const archetype_t *archetype, component_id_t id);

/* appends a row for `entity`, growing storage as needed; the row's component bytes are uninitialized */
uint32_t archetype_add_row(struct world *world, archetype_t *archetype, entity_t entity);

/* swap-removes `row`; returns the entity moved into its place, or ECS_ENTITY_NULL if `row` was last */
entity_t archetype_remove_row(struct world *world, archetype_t *archetype, uint32_t row);
