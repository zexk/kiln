#pragma once

#include "voxel.h"
#include "mesh.h"
#include "math3d.h"

typedef struct {
    Chunk     *chunk;
    Mesh      *mesh;
    bool       active;
    bool       dirty;
    bool       save_dirty;
    R_Texture  voxel_tex;
} LoadedChunk;

typedef struct World {
    LoadedChunk *chunks;
    int          capacity;
    int          count;
    int          render_distance;
    R_Program    mesh_compute_program;
} World;

void      world_init(World *world, int render_distance);
void      world_update(World *world, vec3 camera_pos);
void      world_flush_saves(World *world);
void      world_free(World *world);
BlockType world_get_block(const World *world, int x, int y, int z);
bool      world_is_solid(const World *world, int x, int y, int z);
void      world_set_block(World *world, int x, int y, int z, BlockType type);
