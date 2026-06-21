#include "components.h"
#include <string.h>

component_id_t COMP_TRANSFORM;
component_id_t COMP_BLOCK_DEF;
component_id_t COMP_MOVEMENT;
component_id_t COMP_HEALTH;

world_t  *g_ecs = NULL;
entity_t  g_block_entities[256];

void components_init(world_t *world) {
    COMP_TRANSFORM = component_register(world, "transform", sizeof(C_Transform), _Alignof(C_Transform));
    COMP_BLOCK_DEF = component_register(world, "block_def", sizeof(C_BlockDef),  _Alignof(C_BlockDef));
    COMP_MOVEMENT  = component_register(world, "movement",  sizeof(C_Movement),  _Alignof(C_Movement));
    COMP_HEALTH    = component_register(world, "health",    sizeof(C_Health),    _Alignof(C_Health));
}

entity_t register_block_type(world_t *world, BlockType type, const char *id, const char *name,
                             bool solid, bool opaque, float hardness,
                             const char *tex_path, const char *tex_top,
                             const char *tex_bottom, const char *tex_side) {
    entity_t e    = entity_create(world);
    C_BlockDef *d = entity_add_component(world, e, COMP_BLOCK_DEF);
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
