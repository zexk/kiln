#include "systems.h"
#include "components.h"
#include "camera.h"
#include "logger.h"
#include <math.h>

void sys_movement(ECS *ecs, World *world, float dt) {
    for (Entity e = 1; e < (Entity)ecs->max_entities; e++) {
        if (!ecs_alive(ecs, e)) continue;
        if (!ecs_has(ecs, e, COMP_TRANSFORM) || !ecs_has(ecs, e, COMP_MOVEMENT)) continue;
        C_Transform *transform = ecs_get(ecs, e, COMP_TRANSFORM);
        C_Movement  *movement  = ecs_get(ecs, e, COMP_MOVEMENT);
        if (!transform || !movement) continue;

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
