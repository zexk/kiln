#include "world.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

world_t *world_create(void) {
    arena_t *arena = arena_create(0);
    world_t *world = arena_calloc(arena, 1, sizeof(world_t));
    world->arena = arena;

    signature_t empty;
    signature_clear(&empty);

    world->empty_archetype = archetype_create(world, empty);
    world->archetypes = malloc(sizeof(archetype_t *));
    world->archetypes[0] = world->empty_archetype;
    world->archetype_count = 1;
    world->archetype_capacity = 1;

    return world;
}

void world_destroy(world_t *world) {
    /* the world lives inside its own arena, so grab the handle before freeing */
    arena_t *arena = world->arena;

    /* release the heap-backed growable buffers the arena does not own */
    for (uint32_t i = 0; i < world->archetype_count; i++) {
        archetype_destroy(world->archetypes[i]);
    }
    free(world->archetypes);
    free(world->locations);
    free(world->generations);
    free(world->free_indices);

    /* frees the world, every archetype struct, and their column-pointer arrays */
    arena_destroy(arena);
}

component_id_t component_register(world_t *world, const char *name, size_t size, size_t align) {
    assert(world->component_count < ECS_MAX_COMPONENTS);

    component_id_t id = world->component_count++;
    world->components[id] = (component_info_t){ .name = name, .size = size, .align = align };
    return id;
}

static archetype_t *world_find_archetype(world_t *world, signature_t signature) {
    for (uint32_t i = 0; i < world->archetype_count; i++) {
        if (signature_equal(&world->archetypes[i]->signature, &signature)) {
            return world->archetypes[i];
        }
    }
    return NULL;
}

static archetype_t *world_find_or_create_archetype(world_t *world, signature_t signature) {
    archetype_t *existing = world_find_archetype(world, signature);
    if (existing) {
        return existing;
    }

    if (world->archetype_count == world->archetype_capacity) {
        world->archetype_capacity = world->archetype_capacity ? world->archetype_capacity * 2 : 4;
        world->archetypes = realloc(world->archetypes, world->archetype_capacity * sizeof(archetype_t *));
    }

    archetype_t *a = archetype_create(world, signature);
    world->archetypes[world->archetype_count++] = a;
    return a;
}

static archetype_t *world_archetype_with(world_t *world, archetype_t *from, component_id_t id) {
    if (from->add_edge[id]) {
        return from->add_edge[id];
    }

    signature_t signature = from->signature;
    signature_set(&signature, id);

    archetype_t *to = world_find_or_create_archetype(world, signature);
    from->add_edge[id] = to;
    to->remove_edge[id] = from;
    return to;
}

static archetype_t *world_archetype_without(world_t *world, archetype_t *from, component_id_t id) {
    if (from->remove_edge[id]) {
        return from->remove_edge[id];
    }

    signature_t signature = from->signature;
    signature_unset(&signature, id);

    archetype_t *to = world_find_or_create_archetype(world, signature);
    from->remove_edge[id] = to;
    to->add_edge[id] = from;
    return to;
}

static void world_ensure_entity_capacity(world_t *world, uint32_t index) {
    if (index < world->entity_capacity) {
        return;
    }

    uint32_t new_capacity = world->entity_capacity ? world->entity_capacity * 2 : 16;
    while (new_capacity <= index) {
        new_capacity *= 2;
    }

    world->locations = realloc(world->locations, new_capacity * sizeof(entity_loc_t));
    world->generations = realloc(world->generations, new_capacity * sizeof(uint32_t));

    for (uint32_t i = world->entity_capacity; i < new_capacity; i++) {
        world->locations[i] = (entity_loc_t){ 0 };
        /* Start at 1 so entity {index 0, gen 0} — i.e. the value 0 — is never a
           live handle. That keeps ECS_ENTITY_NULL a safe sentinel for both
           entity_is_alive and the swap-remove "nothing moved" check. */
        world->generations[i] = 1;
    }

    world->entity_capacity = new_capacity;
}

entity_t entity_create(world_t *world) {
    uint32_t index;

    if (world->free_count > 0) {
        index = world->free_indices[--world->free_count];
    } else {
        index = world->next_index++;
        world_ensure_entity_capacity(world, index);
    }

    entity_t entity = ECS_ENTITY_MAKE(index, world->generations[index]);
    uint32_t row = archetype_add_row(world, world->empty_archetype, entity);
    world->locations[index] = (entity_loc_t){ .archetype = world->empty_archetype, .row = row };

    return entity;
}

int entity_is_alive(world_t *world, entity_t entity) {
    uint32_t index = ECS_ENTITY_INDEX(entity);
    if (index >= world->entity_capacity) {
        return 0;
    }
    return world->generations[index] == ECS_ENTITY_GEN(entity);
}

void entity_destroy(world_t *world, entity_t entity) {
    if (!entity_is_alive(world, entity)) {
        return;
    }

    uint32_t index = ECS_ENTITY_INDEX(entity);
    entity_loc_t loc = world->locations[index];

    entity_t moved = archetype_remove_row(world, loc.archetype, loc.row);
    if (moved != ECS_ENTITY_NULL) {
        world->locations[ECS_ENTITY_INDEX(moved)].row = loc.row;
    }

    world->generations[index]++;

    if (world->free_count == world->free_capacity) {
        world->free_capacity = world->free_capacity ? world->free_capacity * 2 : 16;
        world->free_indices = realloc(world->free_indices, world->free_capacity * sizeof(uint32_t));
    }
    world->free_indices[world->free_count++] = index;
}

static void *archetype_component_ptr(world_t *world, archetype_t *archetype, uint32_t row, component_id_t id) {
    int col = archetype_column_index(archetype, id);
    if (col < 0) {
        return NULL;
    }
    size_t size = world->components[id].size;
    return (char *)archetype->columns[col] + (size_t)row * size;
}

static uint32_t move_entity(world_t *world, entity_t entity, archetype_t *from, uint32_t from_row, archetype_t *to) {
    uint32_t to_row = archetype_add_row(world, to, entity);

    for (uint32_t i = 0; i < from->component_count; i++) {
        component_id_t id = from->component_ids[i];
        int to_col = archetype_column_index(to, id);
        if (to_col < 0) {
            continue; /* component dropped by this transition */
        }

        size_t size = world->components[id].size;
        memcpy((char *)to->columns[to_col] + (size_t)to_row * size,
               (char *)from->columns[i] + (size_t)from_row * size,
               size);
    }

    entity_t moved = archetype_remove_row(world, from, from_row);
    if (moved != ECS_ENTITY_NULL) {
        world->locations[ECS_ENTITY_INDEX(moved)].row = from_row;
    }

    world->locations[ECS_ENTITY_INDEX(entity)] = (entity_loc_t){ .archetype = to, .row = to_row };
    return to_row;
}

void *entity_add_component(world_t *world, entity_t entity, component_id_t id) {
    if (!entity_is_alive(world, entity)) {
        return NULL;
    }

    uint32_t index = ECS_ENTITY_INDEX(entity);
    entity_loc_t loc = world->locations[index];

    if (signature_has(&loc.archetype->signature, id)) {
        return archetype_component_ptr(world, loc.archetype, loc.row, id);
    }

    archetype_t *to = world_archetype_with(world, loc.archetype, id);
    uint32_t to_row = move_entity(world, entity, loc.archetype, loc.row, to);

    return archetype_component_ptr(world, to, to_row, id);
}

void entity_remove_component(world_t *world, entity_t entity, component_id_t id) {
    if (!entity_is_alive(world, entity)) {
        return;
    }

    uint32_t index = ECS_ENTITY_INDEX(entity);
    entity_loc_t loc = world->locations[index];

    if (!signature_has(&loc.archetype->signature, id)) {
        return;
    }

    archetype_t *to = world_archetype_without(world, loc.archetype, id);
    move_entity(world, entity, loc.archetype, loc.row, to);
}

void *entity_get_component(world_t *world, entity_t entity, component_id_t id) {
    if (!entity_is_alive(world, entity)) {
        return NULL;
    }

    entity_loc_t loc = world->locations[ECS_ENTITY_INDEX(entity)];
    return archetype_component_ptr(world, loc.archetype, loc.row, id);
}

int entity_has_component(world_t *world, entity_t entity, component_id_t id) {
    if (!entity_is_alive(world, entity)) {
        return 0;
    }

    entity_loc_t loc = world->locations[ECS_ENTITY_INDEX(entity)];
    return signature_has(&loc.archetype->signature, id);
}
