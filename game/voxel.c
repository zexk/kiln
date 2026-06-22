#include "voxel.h"
#include "noise.h"
#include "math3d.h"
#include <string.h>
#include <math.h>

#define SEA_LEVEL 5

typedef enum { BIOME_PLAINS, BIOME_DESERT, BIOME_FOREST, BIOME_TUNDRA, BIOME_MOUNTAINS } Biome;

static Biome get_biome(float temp, float humidity, float mountain) {
    if (mountain > 0.62f)                            return BIOME_MOUNTAINS;
    if (temp > 0.65f && humidity < 0.48f)            return BIOME_DESERT;
    if (temp < 0.38f)                                return BIOME_TUNDRA;
    if (humidity > 0.56f)                            return BIOME_FOREST;
    return BIOME_PLAINS;
}

static int biome_height(Biome b, float wx, float wz) {
    switch (b) {
    case BIOME_PLAINS:    return 7  + (int)(noise_fbm2d(wx*0.012f,          wz*0.012f,          4, 2.0f, 0.5f) * 4.0f);
    case BIOME_DESERT:    return 4  + (int)(noise_fbm2d(wx*0.006f,          wz*0.006f,          3, 2.0f, 0.5f) * 3.0f);
    case BIOME_FOREST:    return 8  + (int)(noise_fbm2d(wx*0.016f,          wz*0.016f,          4, 2.0f, 0.5f) * 5.0f);
    case BIOME_TUNDRA:    return 5  + (int)(noise_fbm2d(wx*0.008f,          wz*0.008f,          3, 2.0f, 0.5f) * 4.0f);
    case BIOME_MOUNTAINS: return 9  + (int)(noise_fbm2d(wx*0.022f,          wz*0.022f,          5, 2.0f, 0.5f) * 6.0f);
    default:              return 7;
    }
}

static void fill_column(Chunk *chunk, int lx, int lz, int height, Biome b) {
    for (int y = 0; y <= height; y++) {
        BlockType t;
        int depth = height - y;
        switch (b) {
        case BIOME_DESERT:
            t = (depth < 4) ? BLOCK_SAND : BLOCK_STONE;
            break;
        case BIOME_MOUNTAINS:
            t = (depth == 0) ? BLOCK_STONE : (depth < 2) ? BLOCK_GRAVEL : BLOCK_STONE;
            break;
        case BIOME_TUNDRA:
            t = (depth == 0) ? BLOCK_GRAVEL : (depth < 3) ? BLOCK_DIRT : BLOCK_STONE;
            break;
        case BIOME_FOREST:
            t = (depth == 0) ? BLOCK_GRASS : (depth < 5) ? BLOCK_DIRT : BLOCK_STONE;
            break;
        default: /* PLAINS */
            t = (depth == 0) ? BLOCK_GRASS : (depth < 3) ? BLOCK_DIRT : BLOCK_STONE;
            break;
        }
        chunk->blocks[lx][y][lz] = t;
    }
}

static bool is_cave(float wx, float wy, float wz) {
    float n1 = noise_fbm3d(wx*0.040f,        wy*0.055f,        wz*0.040f,        3, 2.0f, 0.5f);
    float n2 = noise_fbm3d(wx*0.040f+300.0f, wy*0.055f+300.0f, wz*0.040f+300.0f, 3, 2.0f, 0.5f);
    float d1 = n1 - 0.5f, d2 = n2 - 0.5f;
    return (d1*d1 + d2*d2) < 0.018f;
}

void chunk_init(Chunk *chunk, int x, int z) {
    chunk->x = x;
    chunk->z = z;
    chunk->min = (vec3){(float)(x * CHUNK_SIZE), 0.0f, (float)(z * CHUNK_SIZE)};
    chunk->max = (vec3){(float)((x + 1) * CHUNK_SIZE), (float)CHUNK_SIZE, (float)((z + 1) * CHUNK_SIZE)};
    memset(chunk->blocks, BLOCK_AIR, sizeof(chunk->blocks));

    /* ── terrain ─────────────────────────────────────────────────────────── */
    for (int lx = 0; lx < CHUNK_SIZE; lx++) {
        for (int lz = 0; lz < CHUNK_SIZE; lz++) {
            float wx = (float)(x * CHUNK_SIZE + lx);
            float wz = (float)(z * CHUNK_SIZE + lz);

            float temp     = noise_fbm2d(wx*0.0020f,          wz*0.0020f,          3, 2.0f, 0.5f);
            float humidity = noise_fbm2d(wx*0.0020f + 500.0f, wz*0.0020f + 500.0f, 3, 2.0f, 0.5f);
            float mountain = noise_fbm2d(wx*0.0030f + 1000.0f,wz*0.0030f + 1000.0f,2, 2.0f, 0.5f);

            Biome b      = get_biome(temp, humidity, mountain);
            int   height = biome_height(b, wx, wz);
            if (height >= CHUNK_SIZE) height = CHUNK_SIZE - 1;

            fill_column(chunk, lx, lz, height, b);
        }
    }

    /* ── cave carving (skip y=0 floor and top 3 rows to protect surface) ── */
    for (int lx = 0; lx < CHUNK_SIZE; lx++) {
        for (int y = 1; y < CHUNK_SIZE - 3; y++) {
            for (int lz = 0; lz < CHUNK_SIZE; lz++) {
                if (chunk->blocks[lx][y][lz] == BLOCK_AIR) continue;
                float wx = (float)(x * CHUNK_SIZE + lx);
                float wz = (float)(z * CHUNK_SIZE + lz);
                if (is_cave(wx, (float)y, wz))
                    chunk->blocks[lx][y][lz] = BLOCK_AIR;
            }
        }
    }

    /* ── sea-level water fill (top-down column fill stops at first solid) ── */
    for (int lx = 0; lx < CHUNK_SIZE; lx++) {
        for (int lz = 0; lz < CHUNK_SIZE; lz++) {
            for (int y = SEA_LEVEL; y >= 1; y--) {
                if (chunk->blocks[lx][y][lz] == BLOCK_AIR)
                    chunk->blocks[lx][y][lz] = BLOCK_WATER;
                else
                    break;
            }
        }
    }
}
