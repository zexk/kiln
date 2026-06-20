#include "components.h"
#include <string.h>

int    COMP_TRANSFORM;
int    COMP_BLOCK_DEF;
int    COMP_MOVEMENT;
int    COMP_HEALTH;

ECS    g_ecs;
Entity g_block_entities[256];

void components_init(ECS *ecs) {
    COMP_TRANSFORM = ecs_register(ecs, sizeof(C_Transform));
    COMP_BLOCK_DEF = ecs_register(ecs, sizeof(C_BlockDef));
    COMP_MOVEMENT  = ecs_register(ecs, sizeof(C_Movement));
    COMP_HEALTH    = ecs_register(ecs, sizeof(C_Health));
}

Entity register_block_type(ECS *ecs, BlockType type, const char *id, const char *name,
                           bool solid, bool opaque, float hardness,
                           const char *tex_path, const char *tex_top,
                           const char *tex_bottom, const char *tex_side) {
    Entity e      = ecs_spawn(ecs);
    C_BlockDef *d = ecs_add(ecs, e, COMP_BLOCK_DEF);
    d->id           = id;
    d->name         = name;
    d->solid        = solid;
    d->opaque       = opaque;
    d->hardness     = hardness;
    d->tex_path     = tex_path;
    d->tex_top      = tex_top;
    d->tex_bottom   = tex_bottom;
    d->tex_side     = tex_side;
    d->layer_default = d->layer_top = d->layer_bottom = d->layer_side = -1;
    g_block_entities[type] = e;
    return e;
}
