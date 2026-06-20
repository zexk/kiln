#pragma once

#include "ecs.h"
#include "math3d.h"
#include "voxel.h"
#include <stdbool.h>

extern int COMP_TRANSFORM;
extern int COMP_BLOCK_DEF;
extern int COMP_MOVEMENT;
extern int COMP_HEALTH;

extern ECS    g_ecs;
extern Entity g_block_entities[256];

typedef struct {
    vec3  position;
    float yaw;
    float pitch;
} C_Transform;

typedef struct {
    const char *id;
    const char *name;
    bool        solid;
    bool        opaque;
    float       hardness;
    const char *tex_path;
    const char *tex_top;
    const char *tex_bottom;
    const char *tex_side;
    int         layer_default;
    int         layer_top;
    int         layer_bottom;
    int         layer_side;
} C_BlockDef;

typedef struct {
    vec3  velocity;
    float speed;
    bool  grounded;
} C_Movement;

typedef struct {
    float current;
    float max;
} C_Health;

void   components_init(ECS *ecs);
Entity register_block_type(ECS *ecs, BlockType type, const char *id, const char *name,
                           bool solid, bool opaque, float hardness,
                           const char *tex_path, const char *tex_top,
                           const char *tex_bottom, const char *tex_side);
