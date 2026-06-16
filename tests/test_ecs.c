#include "ecs.h"

#include <assert.h>
#include <stdio.h>

typedef struct {
    float x, y, z;
} position_t;

typedef struct {
    float x, y, z;
} velocity_t;

static void test_add_get_component(void) {
    world_t *world = world_create();
    component_id_t pos_id = component_register(world, "position", sizeof(position_t), __alignof__(position_t));

    entity_t e = entity_create(world);
    assert(entity_is_alive(world, e));
    assert(!entity_has_component(world, e, pos_id));

    position_t *p = entity_add_component(world, e, pos_id);
    assert(p != NULL);
    *p = (position_t){ 1.0f, 2.0f, 3.0f };

    assert(entity_has_component(world, e, pos_id));
    position_t *got = entity_get_component(world, e, pos_id);
    assert(got != NULL);
    assert(got->x == 1.0f && got->y == 2.0f && got->z == 3.0f);

    world_destroy(world);
}

static void test_migration_preserves_data(void) {
    world_t *world = world_create();
    component_id_t pos_id = component_register(world, "position", sizeof(position_t), __alignof__(position_t));
    component_id_t vel_id = component_register(world, "velocity", sizeof(velocity_t), __alignof__(velocity_t));

    entity_t e = entity_create(world);
    *(position_t *)entity_add_component(world, e, pos_id) = (position_t){ 4.0f, 5.0f, 6.0f };

    /* adding velocity migrates e into the {position, velocity} archetype */
    *(velocity_t *)entity_add_component(world, e, vel_id) = (velocity_t){ 0.1f, 0.2f, 0.3f };

    position_t *p = entity_get_component(world, e, pos_id);
    assert(p->x == 4.0f && p->y == 5.0f && p->z == 6.0f);

    velocity_t *v = entity_get_component(world, e, vel_id);
    assert(v->x == 0.1f && v->y == 0.2f && v->z == 0.3f);

    /* removing position migrates e back down; velocity must survive */
    entity_remove_component(world, e, pos_id);
    assert(!entity_has_component(world, e, pos_id));
    assert(entity_get_component(world, e, pos_id) == NULL);

    velocity_t *v_after = entity_get_component(world, e, vel_id);
    assert(v_after->x == 0.1f && v_after->y == 0.2f && v_after->z == 0.3f);

    world_destroy(world);
}

static void test_swap_remove_keeps_other_entities_intact(void) {
    world_t *world = world_create();
    component_id_t pos_id = component_register(world, "position", sizeof(position_t), __alignof__(position_t));

    entity_t a = entity_create(world);
    entity_t b = entity_create(world);
    entity_t c = entity_create(world);

    *(position_t *)entity_add_component(world, a, pos_id) = (position_t){ 1, 0, 0 };
    *(position_t *)entity_add_component(world, b, pos_id) = (position_t){ 2, 0, 0 };
    *(position_t *)entity_add_component(world, c, pos_id) = (position_t){ 3, 0, 0 };

    /* destroying b swap-removes c into b's old row; c's data must follow it */
    entity_destroy(world, b);

    assert(!entity_is_alive(world, b));
    assert(entity_is_alive(world, a));
    assert(entity_is_alive(world, c));

    assert(((position_t *)entity_get_component(world, a, pos_id))->x == 1);
    assert(((position_t *)entity_get_component(world, c, pos_id))->x == 3);

    world_destroy(world);
}

static void test_entity_recycling_bumps_generation(void) {
    world_t *world = world_create();

    entity_t e1 = entity_create(world);
    entity_destroy(world, e1);
    entity_t e2 = entity_create(world);

    assert(ECS_ENTITY_INDEX(e1) == ECS_ENTITY_INDEX(e2));
    assert(ECS_ENTITY_GEN(e2) == ECS_ENTITY_GEN(e1) + 1);
    assert(!entity_is_alive(world, e1));
    assert(entity_is_alive(world, e2));

    world_destroy(world);
}

static void test_query_iteration(void) {
    world_t *world = world_create();
    component_id_t pos_id = component_register(world, "position", sizeof(position_t), __alignof__(position_t));
    component_id_t vel_id = component_register(world, "velocity", sizeof(velocity_t), __alignof__(velocity_t));

    entity_t a = entity_create(world); /* position only */
    *(position_t *)entity_add_component(world, a, pos_id) = (position_t){ 1, 1, 1 };

    entity_t b = entity_create(world); /* position + velocity */
    *(position_t *)entity_add_component(world, b, pos_id) = (position_t){ 2, 2, 2 };
    *(velocity_t *)entity_add_component(world, b, vel_id) = (velocity_t){ 9, 9, 9 };

    entity_t c = entity_create(world); /* velocity only, must not match */
    *(velocity_t *)entity_add_component(world, c, vel_id) = (velocity_t){ 5, 5, 5 };

    signature_t require;
    signature_clear(&require);
    signature_set(&require, pos_id);

    signature_t exclude;
    signature_clear(&exclude);

    query_iter_t it = query_iter(world, (query_desc_t){ .require = require, .exclude = exclude });

    int seen = 0;
    float sum_x = 0;
    while (query_next(&it)) {
        position_t *p = query_get(&it, pos_id);
        assert(p != NULL);
        sum_x += p->x;
        seen++;
    }

    assert(seen == 2); /* a and b, not c */
    assert(sum_x == 3.0f);

    world_destroy(world);
}

int main(void) {
    test_add_get_component();
    test_migration_preserves_data();
    test_swap_remove_keeps_other_entities_intact();
    test_entity_recycling_bumps_generation();
    test_query_iteration();

    printf("ecs: all tests passed\n");
    return 0;
}
