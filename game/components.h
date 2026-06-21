#pragma once

#include "ecs.h"      /* kiln: world_t, entity_t, component_id_t */
#include "math3d.h"
#include "voxel.h"
#include <stdbool.h>

extern component_id_t COMP_TRANSFORM;
extern component_id_t COMP_BLOCK_DEF;
extern component_id_t COMP_MOVEMENT;
extern component_id_t COMP_HEALTH;

extern world_t  *g_ecs;
extern entity_t  g_block_entities[256];

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

void     components_init(world_t *world);
entity_t register_block_type(world_t *world, BlockType type, const char *id, const char *name,
                             bool solid, bool opaque, float hardness,
                             const char *tex_path, const char *tex_top,
                             const char *tex_bottom, const char *tex_side);
