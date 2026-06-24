#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "linalg.h"
#include "renderer.h"

/* ── Constants ───────────────────────────────────────────────────────────── */

#define KV_CHUNK_SIZE    16
#define KV_LOD_LEVELS     3
#define KV_BLOCK_AIR      ((uint16_t)0)
#define KV_MAX_BLOCK_TYPES 4096   /* must match KV_MAX_BLOCKS in kv_internal.h */

/* ── Block registry ──────────────────────────────────────────────────────── */

/* Block definition supplied by the game. Borrowed string pointers must remain
   valid until kv_build_texture_array() returns. r/g/b tint: leave as 0,0,0 to
   get white (no tint); non-zero values are used as-is. */
typedef struct {
    const char *id;        /* namespaced ID e.g. "kyub:stone"   */
    const char *name;
    bool        solid;
    bool        opaque;
    float       hardness;
    float       r, g, b;  /* vertex tint; {0,0,0} → {1,1,1}    */
    const char *tex_path;
    const char *tex_top;    /* NULL → falls back to tex_path     */
    const char *tex_bottom;
    const char *tex_side;
} kv_block_def_t;

/* Register a block type; returns its uint16_t ID (≥ 1).
   KV_BLOCK_AIR (0) is always implicit — do not register it. */
uint16_t              kv_block_register(const kv_block_def_t *def);
uint16_t              kv_block_count(void);
const kv_block_def_t *kv_block_get(uint16_t id);
bool                  kv_block_is_solid(uint16_t id);
bool                  kv_block_is_opaque(uint16_t id);

/* Top-face texture array layer for block_id (set after kv_build_texture_array).
   Returns -1 for air or an unregistered/textureless block. */
int kv_block_tex_layer_top(uint16_t block_id);

/* Build a tile_size×tile_size texture array from all registered block textures.
   Resolves per-face layer indices into the internal registry. Call once after
   all kv_block_register() calls and before kv_world_draw(). */
R_Texture kv_build_texture_array(int tile_size);

/* ── Terrain generator callback ──────────────────────────────────────────── */

/* Invoked for each newly loaded chunk. cx/cy/cz are chunk coordinates;
   multiply by KV_CHUNK_SIZE for world-absolute block positions. Fill `blocks`
   with registered block IDs; fill `meta` with per-block state (may stay 0). */
typedef void (*kv_gen_fn)(
    uint16_t blocks[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE],
    uint16_t meta[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE],
    int32_t cx, int32_t cy, int32_t cz,
    void *ctx);

/* ── World ───────────────────────────────────────────────────────────────── */

typedef struct kv_world_t kv_world_t;

/* horiz_dist: chunk streaming radius on the XZ plane.
   vert_radius: chunk radius on Y (keep small — 2 gives 5 layers / 80 blocks).
   save_dir:   directory for per-chunk save files (created if absent). */
kv_world_t *kv_world_create(int horiz_dist, int vert_radius,
                             kv_gen_fn gen, void *gen_ctx,
                             const char *save_dir);
void        kv_world_destroy(kv_world_t *world);

/* Call every frame to stream chunks in/out and remesh dirty ones. */
void kv_world_update(kv_world_t *world, vec3_t camera_pos);

/* Flush pending chunk saves to disk (safe to call from the game loop). */
void kv_world_flush_saves(kv_world_t *world);

/* Block access — world-absolute coordinates. */
uint16_t kv_world_get_block(const kv_world_t *world, int x, int y, int z);
uint16_t kv_world_get_meta(const kv_world_t *world, int x, int y, int z);
void     kv_world_set_block(kv_world_t *world, int x, int y, int z, uint16_t type);
void     kv_world_set_meta(kv_world_t *world, int x, int y, int z, uint16_t meta);
bool     kv_world_is_solid(const kv_world_t *world, int x, int y, int z);

/* Physics bridge: signature matches phys_solid_fn — pass kv_world_t* as ctx. */
bool kv_solid_query(void *world_opaque, int x, int y, int z);

/* Draw all loaded chunks. fog_color / fog_density are passed to the voxel
   shader each frame. The voxel shader must be compiled from
   kiln/src/voxel/shaders/voxel.{vert,frag} into "shaders/voxel.{vert,frag}"
   relative to the game executable. */
void kv_world_draw(kv_world_t *world, R_Texture tex_array,
                   mat4_t view, mat4_t proj,
                   vec3_t fog_color, float fog_density);
