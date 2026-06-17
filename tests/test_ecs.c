#include "ecs.h"

#include <criterion/criterion.h>
#include <criterion/new/assert.h>

typedef struct {
    float x, y, z;
} position_t;

typedef struct {
    float x, y, z;
} velocity_t;

Test(ecs, add_get_component) {
    world_t *world = world_create();
    component_id_t pos_id = component_register(world, "position", sizeof(position_t), __alignof__(position_t));

    entity_t e = entity_create(world);
    cr_assert(entity_is_alive(world, e));
    cr_assert(not(entity_has_component(world, e, pos_id)));

    position_t *p = entity_add_component(world, e, pos_id);
    cr_assert(ne(ptr, p, NULL));
    *p = (position_t){ 1.0f, 2.0f, 3.0f };

    cr_assert(entity_has_component(world, e, pos_id));
    position_t *got = entity_get_component(world, e, pos_id);
    cr_assert(ne(ptr, got, NULL));
    cr_assert(ieee_ulp_eq(flt, got->x, 1.0f, 4));
    cr_assert(ieee_ulp_eq(flt, got->y, 2.0f, 4));
    cr_assert(ieee_ulp_eq(flt, got->z, 3.0f, 4));

    world_destroy(world);
}

Test(ecs, migration_preserves_data) {
    world_t *world = world_create();
    component_id_t pos_id = component_register(world, "position", sizeof(position_t), __alignof__(position_t));
    component_id_t vel_id = component_register(world, "velocity", sizeof(velocity_t), __alignof__(velocity_t));

    entity_t e = entity_create(world);
    *(position_t *)entity_add_component(world, e, pos_id) = (position_t){ 4.0f, 5.0f, 6.0f };

    /* adding velocity migrates e into the {position, velocity} archetype */
    *(velocity_t *)entity_add_component(world, e, vel_id) = (velocity_t){ 0.1f, 0.2f, 0.3f };

    position_t *p = entity_get_component(world, e, pos_id);
    cr_assert(ieee_ulp_eq(flt, p->x, 4.0f, 4));
    cr_assert(ieee_ulp_eq(flt, p->y, 5.0f, 4));
    cr_assert(ieee_ulp_eq(flt, p->z, 6.0f, 4));

    velocity_t *v = entity_get_component(world, e, vel_id);
    cr_assert(ieee_ulp_eq(flt, v->x, 0.1f, 4));
    cr_assert(ieee_ulp_eq(flt, v->y, 0.2f, 4));
    cr_assert(ieee_ulp_eq(flt, v->z, 0.3f, 4));

    /* removing position migrates e back down; velocity must survive */
    entity_remove_component(world, e, pos_id);
    cr_assert(not(entity_has_component(world, e, pos_id)));
    cr_assert(eq(ptr, entity_get_component(world, e, pos_id), NULL));

    velocity_t *v_after = entity_get_component(world, e, vel_id);
    cr_assert(ieee_ulp_eq(flt, v_after->x, 0.1f, 4));
    cr_assert(ieee_ulp_eq(flt, v_after->y, 0.2f, 4));
    cr_assert(ieee_ulp_eq(flt, v_after->z, 0.3f, 4));

    world_destroy(world);
}

Test(ecs, swap_remove_keeps_other_entities_intact) {
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

    cr_assert(not(entity_is_alive(world, b)));
    cr_assert(entity_is_alive(world, a));
    cr_assert(entity_is_alive(world, c));

    cr_assert(ieee_ulp_eq(flt, ((position_t *)entity_get_component(world, a, pos_id))->x, 1.0f, 4));
    cr_assert(ieee_ulp_eq(flt, ((position_t *)entity_get_component(world, c, pos_id))->x, 3.0f, 4));

    world_destroy(world);
}

Test(ecs, entity_recycling_bumps_generation) {
    world_t *world = world_create();

    entity_t e1 = entity_create(world);
    entity_destroy(world, e1);
    entity_t e2 = entity_create(world);

    cr_assert(eq(u32, ECS_ENTITY_INDEX(e1), ECS_ENTITY_INDEX(e2)));
    cr_assert(eq(u32, ECS_ENTITY_GEN(e2), ECS_ENTITY_GEN(e1) + 1));
    cr_assert(not(entity_is_alive(world, e1)));
    cr_assert(entity_is_alive(world, e2));

    world_destroy(world);
}

Test(ecs, first_entity_is_not_null_handle) {
    world_t *world = world_create();

    /* The first entity occupies index 0; its handle must still differ from
       ECS_ENTITY_NULL (0), and the null handle must never read as alive — both
       rely on generations starting at 1. */
    entity_t e = entity_create(world);
    cr_assert(ne(u64, e, ECS_ENTITY_NULL));
    cr_assert(eq(u32, ECS_ENTITY_INDEX(e), 0));
    cr_assert(not(entity_is_alive(world, ECS_ENTITY_NULL)));
    cr_assert(entity_is_alive(world, e));

    world_destroy(world);
}

Test(ecs, query_iteration) {
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
        cr_assert(ne(ptr, p, NULL));
        sum_x += p->x;
        seen++;
    }

    cr_assert(eq(int, seen, 2)); /* a and b, not c */
    cr_assert(ieee_ulp_eq(flt, sum_x, 3.0f, 4));

    world_destroy(world);
}

Test(ecs, query_exclude_filter) {
    world_t *world = world_create();
    component_id_t pos_id = component_register(world, "position", sizeof(position_t), __alignof__(position_t));
    component_id_t vel_id = component_register(world, "velocity", sizeof(velocity_t), __alignof__(velocity_t));

    entity_t a = entity_create(world); /* position only — must match */
    *(position_t *)entity_add_component(world, a, pos_id) = (position_t){ 1, 0, 0 };

    entity_t b = entity_create(world); /* position + velocity — excluded */
    *(position_t *)entity_add_component(world, b, pos_id) = (position_t){ 2, 0, 0 };
    *(velocity_t *)entity_add_component(world, b, vel_id) = (velocity_t){ 0, 0, 0 };

    entity_t c = entity_create(world); /* velocity only — not required, also excluded */
    *(velocity_t *)entity_add_component(world, c, vel_id) = (velocity_t){ 0, 0, 0 };

    signature_t require, exclude;
    signature_clear(&require);
    signature_set(&require, pos_id);
    signature_clear(&exclude);
    signature_set(&exclude, vel_id);

    query_iter_t it = query_iter(world, (query_desc_t){ .require = require, .exclude = exclude });

    int seen = 0;
    while (query_next(&it)) {
        position_t *p = query_get(&it, pos_id);
        cr_assert(ieee_ulp_eq(flt, p->x, 1.0f, 4));
        seen++;
    }
    cr_assert(eq(int, seen, 1)); /* only a; b excluded by velocity, c lacks position */

    world_destroy(world);
}
