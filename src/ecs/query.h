#pragma once

#include "world.h"

typedef struct {
    signature_t require;
    signature_t exclude;
} query_desc_t;

typedef struct {
    world_t *world;
    signature_t require;
    signature_t exclude;
    uint32_t archetype_index; /* next archetype to inspect in world->archetypes */
    archetype_t *archetype;   /* current archetype, NULL before the first match */
    uint32_t row;
} query_iter_t;

query_iter_t query_iter(world_t *world, query_desc_t desc);

/* advances to the next matching (entity, row); returns 0 once exhausted */
int query_next(query_iter_t *it);

entity_t query_entity(const query_iter_t *it);
void *query_get(const query_iter_t *it, component_id_t id);
