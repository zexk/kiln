#include "kv_internal.h"

#include <criterion/criterion.h>
#include <criterion/new/assert.h>

#include <string.h>

/* kv_mesh_generate/_lod are pure CPU greedy-meshing functions — testable
   without a live Vulkan device as long as kv_mesh_upload is never called
   (vao/vbo stay R_INVALID_HANDLE, so kv_mesh_free never touches the
   renderer either). */

#define VERTS_PER_FACE 6 /* two triangles, unindexed */

static uint16_t register_block(bool opaque) {
    kv_block_def_t def = {0};
    def.id     = "test:block";
    def.name   = "Test Block";
    def.solid  = true;
    def.opaque = opaque;
    return kv_block_register(&def);
}

typedef uint16_t block_grid_t[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE];

Test(voxel, mesh_init_starts_empty_with_default_capacity) {
    KvMesh m;
    kv_mesh_init(&m);
    cr_assert(eq(u32, m.count, 0));
    cr_assert(m.cap > 0);
    cr_assert(ne(ptr, m.verts, NULL));
    kv_mesh_free(&m);
}

Test(voxel, empty_chunk_generates_no_geometry) {
    block_grid_t blocks;
    memset(blocks, 0, sizeof(blocks));

    KvMesh m;
    kv_mesh_init(&m);
    kv_mesh_generate(&m, blocks, 0, 0, 0);

    cr_assert(eq(u32, m.count, 0));
    kv_mesh_free(&m);
}

Test(voxel, isolated_block_generates_all_six_faces) {
    uint16_t id = register_block(true);
    block_grid_t blocks;
    memset(blocks, 0, sizeof(blocks));
    blocks[5][5][5] = id;

    KvMesh m;
    kv_mesh_init(&m);
    kv_mesh_generate(&m, blocks, 0, 0, 0);

    cr_assert(eq(u32, m.count, 6 * VERTS_PER_FACE));
    kv_mesh_free(&m);
}

Test(voxel, block_at_chunk_corner_still_generates_all_six_faces) {
    /* Out-of-chunk neighbours (x/y/z < 0 or >= KV_CHUNK_SIZE) must be treated
       as transparent, same as air, so a block sitting at the chunk boundary
       isn't missing faces just because it has no same-chunk neighbour there. */
    uint16_t id = register_block(true);
    block_grid_t blocks;
    memset(blocks, 0, sizeof(blocks));
    blocks[0][0][0] = id;

    KvMesh m;
    kv_mesh_init(&m);
    kv_mesh_generate(&m, blocks, 0, 0, 0);

    cr_assert(eq(u32, m.count, 6 * VERTS_PER_FACE));
    kv_mesh_free(&m);
}

Test(voxel, adjacent_opaque_blocks_cull_the_shared_face) {
    uint16_t id = register_block(true);
    block_grid_t blocks;
    memset(blocks, 0, sizeof(blocks));
    blocks[5][5][5] = id;
    blocks[6][5][5] = id; /* touching along +X / -X */

    KvMesh m;
    kv_mesh_init(&m);
    kv_mesh_generate(&m, blocks, 0, 0, 0);

    /* Each block loses exactly the one face touching the other: 5 + 5 = 10. */
    cr_assert(eq(u32, m.count, 10 * VERTS_PER_FACE));
    kv_mesh_free(&m);
}

Test(voxel, non_opaque_neighbour_does_not_cull_the_adjacent_face) {
    /* A non-opaque block (glass) touching an opaque block (stone): stone
       still shows its face into the glass (glass doesn't occlude), but
       glass's face into the opaque stone is culled. */
    uint16_t stone = register_block(true);
    uint16_t glass = register_block(false);
    block_grid_t blocks;
    memset(blocks, 0, sizeof(blocks));
    blocks[5][5][5] = stone;
    blocks[6][5][5] = glass;

    KvMesh m;
    kv_mesh_init(&m);
    kv_mesh_generate(&m, blocks, 0, 0, 0);

    /* stone: all 6 faces exposed (neighbour is non-opaque).
       glass: 5 faces exposed (its -X face into stone is culled). */
    cr_assert(eq(u32, m.count, (6 + 5) * VERTS_PER_FACE));
    kv_mesh_free(&m);
}

Test(voxel, two_non_opaque_neighbours_do_not_cull_each_other) {
    uint16_t glass_a = register_block(false);
    uint16_t glass_b = register_block(false);
    block_grid_t blocks;
    memset(blocks, 0, sizeof(blocks));
    blocks[5][5][5] = glass_a;
    blocks[6][5][5] = glass_b;

    KvMesh m;
    kv_mesh_init(&m);
    kv_mesh_generate(&m, blocks, 0, 0, 0);

    cr_assert(eq(u32, m.count, 12 * VERTS_PER_FACE));
    kv_mesh_free(&m);
}

Test(voxel, world_offset_translates_vertex_positions) {
    uint16_t id = register_block(true);
    block_grid_t blocks;
    memset(blocks, 0, sizeof(blocks));
    blocks[0][0][0] = id;

    KvMesh m;
    kv_mesh_init(&m);
    /* Chunk (1,0,0) at KV_CHUNK_SIZE=16 puts local (0,0,0) at world x=16. */
    kv_mesh_generate(&m, blocks, 1, 0, 0);

    cr_assert(m.count > 0);
    for (uint32_t i = 0; i < m.count; i++) {
        cr_assert(m.verts[i].x >= 16.0f - 0.01f && m.verts[i].x <= 17.0f + 0.01f,
                  "vertex %u x=%f out of expected chunk-offset range", i, m.verts[i].x);
    }
    kv_mesh_free(&m);
}

/* --- LOD meshing --- */

Test(voxel, lod_step_one_matches_kv_mesh_generate) {
    uint16_t id = register_block(true);
    block_grid_t blocks;
    memset(blocks, 0, sizeof(blocks));
    blocks[5][5][5] = id;

    KvMesh a, b;
    kv_mesh_init(&a);
    kv_mesh_init(&b);
    kv_mesh_generate(&a, blocks, 0, 0, 0);
    kv_mesh_generate_lod(&b, blocks, 0, 0, 0, 1);

    cr_assert(eq(u32, a.count, b.count));
    kv_mesh_free(&a);
    kv_mesh_free(&b);
}

Test(voxel, lod_coarser_step_still_produces_geometry_for_solid_region) {
    uint16_t id = register_block(true);
    block_grid_t blocks;
    memset(blocks, 0, sizeof(blocks));
    /* Fill the whole chunk solid so every super-block at step=4 is non-air. */
    for (int x = 0; x < KV_CHUNK_SIZE; x++)
        for (int y = 0; y < KV_CHUNK_SIZE; y++)
            for (int z = 0; z < KV_CHUNK_SIZE; z++)
                blocks[x][y][z] = id;

    KvMesh m;
    kv_mesh_init(&m);
    kv_mesh_generate_lod(&m, blocks, 0, 0, 0, 4);

    /* A fully solid chunk has no internal faces at all — only the six outer
       faces of the whole chunk should be emitted, coarsened into 4x4
       super-blocks: (16/4)^2 = 16 quads per side * 6 sides. */
    uint32_t expected_faces = 16 * 6;
    cr_assert(eq(u32, m.count, expected_faces * VERTS_PER_FACE));
    kv_mesh_free(&m);
}

Test(voxel, lod_empty_chunk_generates_no_geometry) {
    block_grid_t blocks;
    memset(blocks, 0, sizeof(blocks));

    KvMesh m;
    kv_mesh_init(&m);
    kv_mesh_generate_lod(&m, blocks, 0, 0, 0, 4);

    cr_assert(eq(u32, m.count, 0));
    kv_mesh_free(&m);
}
