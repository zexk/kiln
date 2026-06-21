#pragma once
#include "ecs.h"   /* kiln: world_t */
#include "world.h" /* game: World (voxel) */

void sys_movement(world_t *ecs, World *world, float dt);
