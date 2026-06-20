#pragma once

#include "math3d.h"
#include "ecs.h"
#include "voxel.h"
#include "game_input.h"

typedef struct World World;

#define GRAVITY           25.0f
#define JUMP_VELOCITY     10.0f
#define PLAYER_HEIGHT      1.6f
#define PLAYER_EYES_HEIGHT 1.4f
#define PLAYER_HALF_WIDTH  0.3f

typedef struct {
    vec3   front;
    vec3   up;
    float  yaw;
    float  pitch;
    float  speed;
    float  sensitivity;
    Entity player;
} Camera;

void   camera_init(Camera *cam, ECS *ecs);
void   camera_update(Camera *cam, float dt, World *world, GameInput *gi, ECS *ecs);
mat4   camera_get_view_matrix(const Camera *cam, ECS *ecs);
bool   position_is_safe(const World *world, vec3 pos);
bool   player_collides_with_block(const World *world, vec3 player_pos, BlockPos block);
