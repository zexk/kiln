#pragma once

#include <stddef.h>
#include <stdint.h>

#include "archetype.h"
#include "component.h"
#include "entity.h"
#include "signature.h"

typedef struct {
    archetype_t *archetype;
    uint32_t row;
} entity_loc_t;

typedef struct world {
    component_info_t components[ECS_MAX_COMPONENTS];
    component_id_t component_count;

    archetype_t **archetypes;
    uint32_t archetype_count;
    uint32_t archetype_capacity;
    archetype_t *empty_archetype;

    entity_loc_t *locations;  /* entity index -> location, sized entity_capacity */
    uint32_t *generations;    /* entity index -> current generation */
    uint32_t entity_capacity;
    uint32_t next_index; /* next never-before-used entity index */

    uint32_t *free_indices; /* recycled entity indices, stack */
    uint32_t free_count;
    uint32_t free_capacity;
} world_t;

world_t *world_create(void);
void world_destroy(world_t *world);

component_id_t component_register(world_t *world, const char *name, size_t size, size_t align);

entity_t entity_create(world_t *world);
void entity_destroy(world_t *world, entity_t entity);
int entity_is_alive(world_t *world, entity_t entity);

/* returns a pointer to the component's storage so the caller can initialize it;
   a no-op (returns the existing pointer) if the entity already has `id` */
void *entity_add_component(world_t *world, entity_t entity, component_id_t id);
void entity_remove_component(world_t *world, entity_t entity, component_id_t id);
void *entity_get_component(world_t *world, entity_t entity, component_id_t id);
int entity_has_component(world_t *world, entity_t entity, component_id_t id);
