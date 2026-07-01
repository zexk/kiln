#pragma once

/* Internal types shared across kv_block.c / kv_mesh.c / kv_world.c.
   Not part of the public kv.h API. */

#include <stdbool.h>
#include <stdint.h>

#include "kv.h"
#include "frustum.h"

/* ── Block registry entry ────────────────────────────────────────────────── */

#define KV_MAX_BLOCKS 4096

typedef struct {
    kv_block_def_t def;
    int            layer_default;
    int            layer_top;
    int            layer_bottom;
    int            layer_side;
} KvBlockEntry;

extern KvBlockEntry s_kv_blocks[KV_MAX_BLOCKS];
extern uint16_t     s_kv_block_count;   /* always ≥ 1; index 0 = implicit air */

static inline int kv_tex_layer(uint16_t id, int face) {
    if (id == KV_BLOCK_AIR || id >= s_kv_block_count) return 0;
    const KvBlockEntry *e = &s_kv_blocks[id];
    int l;
    if      (face == 4) l = e->layer_top    >= 0 ? e->layer_top    : e->layer_default;
    else if (face == 5) l = e->layer_bottom >= 0 ? e->layer_bottom : e->layer_default;
    else                l = e->layer_side   >= 0 ? e->layer_side   : e->layer_default;
    return l >= 0 ? l : 0;
}

static inline bool kv_block_opaque(uint16_t id) {
    if (id == KV_BLOCK_AIR || id >= s_kv_block_count) return false;
    return s_kv_blocks[id].def.opaque;
}

static inline void kv_block_tint(uint16_t id, float *r, float *g, float *b) {
    if (id == KV_BLOCK_AIR || id >= s_kv_block_count) { *r = *g = *b = 1.0f; return; }
    const kv_block_def_t *d = &s_kv_blocks[id].def;
    bool has_tint = (d->r != 0.0f || d->g != 0.0f || d->b != 0.0f);
    *r = has_tint ? d->r : 1.0f;
    *g = has_tint ? d->g : 1.0f;
    *b = has_tint ? d->b : 1.0f;
}

/* Look up a block ID by its namespaced string (for chunk save/load). */
uint16_t kv_block_id_from_string(const char *s);

/* ── GPU vertex ──────────────────────────────────────────────────────────── */

/* Must match VERTEX_FORMAT_TERRAIN in kiln's r_pipeline.c (stride 64): */
typedef struct {
    float x,   y,   z,  _w;   /* offset  0 – position (w = padding) */
    float r,   g,   b,  _a;   /* offset 16 – tint    (a = padding) */
    float nx,  ny,  nz, ao;   /* offset 32 – normal + ambient occlusion */
    float u,   v,  lay, _p;   /* offset 48 – UV, texture layer, padding */
} KvVertex;                    /* 64 bytes  */

/* ── Mesh ────────────────────────────────────────────────────────────────── */

typedef struct {
    KvVertex *verts;
    uint32_t  count;
    uint32_t  cap;
    R_VAO     vao;
    R_Buffer  vbo;
} KvMesh;

/* Face-adjacent neighbor chunks' block grids, used only at the finest LOD
   (step 1) to cull faces that are occluded by an already-loaded neighbor
   across a chunk boundary. A NULL field means that neighbor isn't loaded —
   the boundary face is drawn rather than risk culling into unloaded/void
   space. Coarser LODs don't use this (their super-block faces are cheap
   enough that boundary overdraw isn't worth the extra bookkeeping). */
typedef struct {
    uint16_t (*xn)[KV_CHUNK_SIZE][KV_CHUNK_SIZE]; /* -x neighbor */
    uint16_t (*xp)[KV_CHUNK_SIZE][KV_CHUNK_SIZE]; /* +x neighbor */
    uint16_t (*yn)[KV_CHUNK_SIZE][KV_CHUNK_SIZE]; /* -y neighbor */
    uint16_t (*yp)[KV_CHUNK_SIZE][KV_CHUNK_SIZE]; /* +y neighbor */
    uint16_t (*zn)[KV_CHUNK_SIZE][KV_CHUNK_SIZE]; /* -z neighbor */
    uint16_t (*zp)[KV_CHUNK_SIZE][KV_CHUNK_SIZE]; /* +z neighbor */
} KvNeighbors;

void kv_mesh_init(KvMesh *m);
void kv_mesh_generate(KvMesh *m,
                      uint16_t blocks[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE],
                      int32_t cx, int32_t cy, int32_t cz,
                      const KvNeighbors *nbrs);
void kv_mesh_generate_lod(KvMesh *m,
                          uint16_t blocks[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE],
                          int32_t cx, int32_t cy, int32_t cz, int step,
                          const KvNeighbors *nbrs);
void kv_mesh_upload(KvMesh *m);
void kv_mesh_free(KvMesh *m);

/* ── Chunk ───────────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t blocks[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE];
    uint16_t meta[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE];
    int32_t  cx, cy, cz;
    vec3_t   aabb_min, aabb_max;
} KvChunk;

typedef struct {
    KvChunk *chunk;
    KvMesh   meshes[KV_LOD_LEVELS];
    bool     active;
    bool     lod_valid[KV_LOD_LEVELS]; /* which LOD levels have uploaded meshes */
    bool     lod_dirty[KV_LOD_LEVELS]; /* which LOD levels need a rebuild       */
    bool     save_dirty;
} KvSlot;

/* Hash table entry for O(1) chunk coordinate lookup. */
typedef struct {
    int32_t cx, cy, cz;
    int32_t idx; /* -1 = empty, -2 = tombstone, >=0 = slot index */
} KvHtEntry;

/* Pending-load record used for distance-sorted streaming. */
typedef struct {
    int32_t cx, cy, cz;
    int     dist;
} KvPending;

/* ── World (opaque externally) ───────────────────────────────────────────── */

struct kv_world_t {
    KvSlot    *slots;
    int        cap;
    int        count;
    int        horiz_dist;
    int        vert_radius;
    kv_gen_fn  gen;
    void      *gen_ctx;
    char       save_dir[256];
    vec3_t     last_cam;
    int32_t    last_cam_cx, last_cam_cy, last_cam_cz;
    bool       has_pending_load;
    R_Program  shader;
    int        loc_model, loc_view, loc_proj;
    int        loc_fog_color, loc_fog_density, loc_texture;
    /* O(1) coordinate→slot lookup */
    KvHtEntry *ht;
    int        ht_cap;
    int        ht_tomb; /* tombstone count; triggers rebuild when high */
    /* scratch buffer for distance-sorted pending loads */
    KvPending *pending_buf;
};
