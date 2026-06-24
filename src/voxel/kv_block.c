#include "kv_internal.h"
#include "texture.h"
#include <string.h>

KvBlockEntry s_kv_blocks[KV_MAX_BLOCKS];
uint16_t     s_kv_block_count = 1; /* 0 = air (implicit) */

uint16_t kv_block_register(const kv_block_def_t *def) {
    if (!def || s_kv_block_count >= KV_MAX_BLOCKS) return KV_BLOCK_AIR;
    uint16_t id = s_kv_block_count++;
    s_kv_blocks[id].def           = *def;
    s_kv_blocks[id].layer_default = -1;
    s_kv_blocks[id].layer_top     = -1;
    s_kv_blocks[id].layer_bottom  = -1;
    s_kv_blocks[id].layer_side    = -1;
    return id;
}

uint16_t kv_block_count(void) { return s_kv_block_count; }

const kv_block_def_t *kv_block_get(uint16_t id) {
    if (id == KV_BLOCK_AIR || id >= s_kv_block_count) return NULL;
    return &s_kv_blocks[id].def;
}

bool kv_block_is_solid(uint16_t id) {
    if (id == KV_BLOCK_AIR || id >= s_kv_block_count) return false;
    return s_kv_blocks[id].def.solid;
}

bool kv_block_is_opaque(uint16_t id) {
    if (id == KV_BLOCK_AIR || id >= s_kv_block_count) return false;
    return s_kv_blocks[id].def.opaque;
}

uint16_t kv_block_id_from_string(const char *s) {
    if (!s) return KV_BLOCK_AIR;
    for (uint16_t i = 1; i < s_kv_block_count; i++)
        if (s_kv_blocks[i].def.id && strcmp(s_kv_blocks[i].def.id, s) == 0) return i;
    return KV_BLOCK_AIR;
}

int kv_block_tex_layer_top(uint16_t block_id) {
    if (block_id == KV_BLOCK_AIR || block_id >= s_kv_block_count) return -1;
    return s_kv_blocks[block_id].layer_top;
}

R_Texture kv_build_texture_array(int tile_size) {
    texture_array_builder_t builder = {0};
    for (uint16_t i = 1; i < s_kv_block_count; i++) {
        KvBlockEntry          *e = &s_kv_blocks[i];
        const kv_block_def_t  *d = &e->def;
        const char *top    = d->tex_top    ? d->tex_top    : d->tex_path;
        const char *bottom = d->tex_bottom ? d->tex_bottom : d->tex_path;
        const char *side   = d->tex_side   ? d->tex_side   : d->tex_path;
        e->layer_default = texture_array_add(&builder, d->tex_path);
        e->layer_top     = texture_array_add(&builder, top);
        e->layer_bottom  = texture_array_add(&builder, bottom);
        e->layer_side    = texture_array_add(&builder, side);
    }
    return texture_array_load(&builder, tile_size, tile_size);
}
