#include "systems.h"
#include "components.h"
#include "logger.h"
#include <math.h>

void sys_movement(world_t *ecs, World *world, float dt) {
    signature_t sig;
    signature_clear(&sig);
    signature_set(&sig, COMP_TRANSFORM);
    signature_set(&sig, COMP_MOVEMENT);
    query_iter_t it = query_iter(ecs, (query_desc_t){.require = sig});
    while (query_next(&it)) {
        C_Transform *transform = query_get(&it, COMP_TRANSFORM);
        C_Movement  *movement  = query_get(&it, COMP_MOVEMENT);

        movement->velocity.y -= GRAVITY * dt;
        transform->position.y += movement->velocity.y * dt;

        if (movement->velocity.y < 0) {
            float feet_y   = transform->position.y - PLAYER_EYES_HEIGHT;
            int   feet_cell = (int)floorf(feet_y);
            float hw        = PLAYER_HALF_WIDTH;
            if (world_is_solid(world, (int)floorf(transform->position.x - hw), feet_cell, (int)floorf(transform->position.z - hw)) ||
                world_is_solid(world, (int)floorf(transform->position.x + hw), feet_cell, (int)floorf(transform->position.z - hw)) ||
                world_is_solid(world, (int)floorf(transform->position.x - hw), feet_cell, (int)floorf(transform->position.z + hw)) ||
                world_is_solid(world, (int)floorf(transform->position.x + hw), feet_cell, (int)floorf(transform->position.z + hw))) {
                transform->position.y = (float)(feet_cell + 1) + PLAYER_EYES_HEIGHT;
                movement->velocity.y  = 0.0f;
                movement->grounded    = true;
            } else {
                movement->grounded = false;
            }
        } else {
            movement->grounded = false;
        }

        if (transform->position.y < 0) {
            transform->position.y = 20.0f;
            transform->position.x = 8.0f;
            transform->position.z = 8.0f;
            movement->velocity.y  = 0.0f;
            movement->grounded    = false;
        }
    }
}
