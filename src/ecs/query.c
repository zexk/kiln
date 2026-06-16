#include "query.h"

query_iter_t query_iter(world_t *world, query_desc_t desc) {
    return (query_iter_t){
        .world = world,
        .require = desc.require,
        .exclude = desc.exclude,
        .archetype_index = 0,
        .archetype = NULL,
        .row = 0,
    };
}

static int archetype_matches(const archetype_t *archetype, const signature_t *require, const signature_t *exclude) {
    if (!signature_is_superset(&archetype->signature, require)) {
        return 0;
    }
    if (signature_intersects(&archetype->signature, exclude)) {
        return 0;
    }
    return 1;
}

int query_next(query_iter_t *it) {
    for (;;) {
        if (it->archetype != NULL) {
            it->row++;
            if (it->row < it->archetype->count) {
                return 1;
            }
            it->archetype = NULL; /* exhausted; look for the next matching archetype */
        }

        if (it->archetype_index >= it->world->archetype_count) {
            return 0;
        }

        archetype_t *candidate = it->world->archetypes[it->archetype_index++];
        if (candidate->count > 0 && archetype_matches(candidate, &it->require, &it->exclude)) {
            it->archetype = candidate;
            it->row = 0;
            return 1;
        }
    }
}

entity_t query_entity(const query_iter_t *it) {
    return it->archetype->entities[it->row];
}

void *query_get(const query_iter_t *it, component_id_t id) {
    int col = archetype_column_index(it->archetype, id);
    if (col < 0) {
        return NULL;
    }
    size_t size = it->world->components[id].size;
    return (char *)it->archetype->columns[col] + (size_t)it->row * size;
}
