#pragma once

#include "voxel.h"
#include "mesh.h"
#include "math3d.h"

typedef struct {
    Chunk *chunk;
    Mesh  *mesh;
    bool   active;
    bool   dirty;
    bool   save_dirty;
} LoadedChunk;

typedef struct World {
    LoadedChunk *chunks;
    int          capacity;
    int          count;
    int          render_distance;
} World;

void      world_init(World *world, int render_distance);
void      world_update(World *world, vec3 camera_pos);
void      world_flush_saves(World *world);
void      world_free(World *world);
BlockType world_get_block(const World *world, int x, int y, int z);
bool      world_is_solid(const World *world, int x, int y, int z);
void      world_set_block(World *world, int x, int y, int z, BlockType type);

/* Player physics constants and helpers (BlockPos is from voxel.h). */
#define GRAVITY            25.0f
#define JUMP_VELOCITY      10.0f
#define PLAYER_HEIGHT       1.6f
#define PLAYER_EYES_HEIGHT  1.4f
#define PLAYER_HALF_WIDTH   0.3f

bool position_is_safe(const World *world, vec3 pos);
bool player_collides_with_block(const World *world, vec3 player_pos, BlockPos block);
