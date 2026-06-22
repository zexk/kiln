#include "systems.h"
#include "components.h"
#include "physics.h"

static bool world_solid_cb(void *ctx, int x, int y, int z) {
    return world_is_solid((World *)ctx, x, y, z);
}

void sys_movement(world_t *ecs, World *world, float dt) {
    signature_t sig;
    signature_clear(&sig);
    signature_set(&sig, COMP_TRANSFORM);
    signature_set(&sig, COMP_MOVEMENT);
    query_iter_t it = query_iter(ecs, (query_desc_t){.require = sig});
    while (query_next(&it)) {
        C_Transform *transform = query_get(&it, COMP_TRANSFORM);
        C_Movement  *movement  = query_get(&it, COMP_MOVEMENT);

        phys_body_t body = {
            .position = transform->position,
            .velocity = movement->velocity,
            .half_w   = PLAYER_HALF_WIDTH,
            .foot_off = PLAYER_EYES_HEIGHT,
            .head_off = PLAYER_HEIGHT - PLAYER_EYES_HEIGHT,
            .gravity  = GRAVITY,
            .grounded = movement->grounded,
        };

        phys_step(&body, dt, world_solid_cb, world);

        transform->position = body.position;
        movement->velocity  = body.velocity;
        movement->grounded  = body.grounded;

        /* Respawn if the entity fell out of the world. */
        if (transform->position.y < 0.0f) {
            transform->position = (vec3){8.0f, 20.0f, 8.0f};
            movement->velocity  = (vec3){0.0f, 0.0f, 0.0f};
            movement->grounded  = false;
        }
    }
}
