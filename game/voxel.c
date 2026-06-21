#include "voxel.h"
#include "noise.h"
#include "math3d.h"
#include <string.h>
#include <math.h>


void chunk_init(Chunk *chunk, int x, int z) {
    chunk->x = x;
    chunk->z = z;
    chunk->min = (vec3){(float)(x * CHUNK_SIZE), 0.0f, (float)(z * CHUNK_SIZE)};
    chunk->max = (vec3){(float)((x + 1) * CHUNK_SIZE), (float)CHUNK_SIZE, (float)((z + 1) * CHUNK_SIZE)};
    memset(chunk->blocks, BLOCK_AIR, sizeof(chunk->blocks));

    for (int lx = 0; lx < CHUNK_SIZE; lx++) {
        for (int lz = 0; lz < CHUNK_SIZE; lz++) {
            float wx = (float)(x * CHUNK_SIZE + lx);
            float wz = (float)(z * CHUNK_SIZE + lz);
            float n  = noise_fbm2d(wx * 0.01f, wz * 0.01f, 4, 2.0f, 0.5f);
            int height = (int)(n * (CHUNK_SIZE - 1));

            for (int y = 0; y <= height; y++) {
                if      (y == height)       chunk->blocks[lx][y][lz] = BLOCK_GRASS;
                else if (y > height - 3)    chunk->blocks[lx][y][lz] = BLOCK_DIRT;
                else                        chunk->blocks[lx][y][lz] = BLOCK_STONE;
            }
        }
    }
}
