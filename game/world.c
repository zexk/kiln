#include "world.h"
#include "logger.h"
#include "components.h"
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#if defined(_WIN32)
#  include <direct.h>
#  define kyub_mkdir(path) _mkdir(path)
#else
#  include <sys/stat.h>
#  define kyub_mkdir(path) mkdir(path, 0755)
#endif

#define MAX_RENDER_DISTANCE 8
#define WORLD_SAVE_DIR      "saves/default"
#define CHUNK_SAVE_MAGIC    "KYUBCHNK"
#define CHUNK_SAVE_MAJOR    1
#define CHUNK_SAVE_MINOR    0
#define CHUNK_VOLUME        (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)

typedef struct { char id[64]; BlockType type; } SavePaletteEntry;

static void write_u16(FILE *f, uint16_t v) { fputc((int)(v & 0xff), f); fputc((int)((v >> 8) & 0xff), f); }
static void write_u32(FILE *f, uint32_t v) { write_u16(f, (uint16_t)(v & 0xffff)); write_u16(f, (uint16_t)((v >> 16) & 0xffff)); }
static void write_i32(FILE *f, int32_t v)  { write_u32(f, (uint32_t)v); }

static bool read_u16(FILE *f, uint16_t *o) {
    int b0 = fgetc(f), b1 = fgetc(f);
    if (b0 == EOF || b1 == EOF) return false;
    *o = (uint16_t)((uint16_t)b0 | ((uint16_t)b1 << 8));
    return true;
}
static bool read_u32(FILE *f, uint32_t *o) {
    uint16_t lo, hi;
    if (!read_u16(f, &lo) || !read_u16(f, &hi)) return false;
    *o = (uint32_t)lo | ((uint32_t)hi << 16);
    return true;
}
static bool read_i32(FILE *f, int32_t *o) { uint32_t v; if (!read_u32(f, &v)) return false; *o = (int32_t)v; return true; }

static void ensure_save_dirs(void) {
    if (kyub_mkdir("saves") != 0 && errno != EEXIST)
        LOG_WARN(CAT_WORLD, "Failed to create saves dir: %s", strerror(errno));
    if (kyub_mkdir(WORLD_SAVE_DIR) != 0 && errno != EEXIST)
        LOG_WARN(CAT_WORLD, "Failed to create world save dir: %s", strerror(errno));
}

static void chunk_save_path(char *out, size_t n, int x, int z) {
    snprintf(out, n, "%s/chunk_%d_%d.kch", WORLD_SAVE_DIR, x, z);
}

static const char *block_id_for_type(BlockType type) {
    entity_t e = g_block_entities[type];
    C_BlockDef *d = (e != ECS_ENTITY_NULL) ? entity_get_component(g_ecs, e, COMP_BLOCK_DEF) : NULL;
    return (d && d->id) ? d->id : "kyub:air";
}

static BlockType block_type_for_id(const char *id) {
    for (int t = 0; t < 256; t++) {
        entity_t e = g_block_entities[t];
        C_BlockDef *d = (e != ECS_ENTITY_NULL) ? entity_get_component(g_ecs, e, COMP_BLOCK_DEF) : NULL;
        if (d && d->id && strcmp(d->id, id) == 0) return (BlockType)t;
    }
    return BLOCK_AIR;
}

bool position_is_safe(const World *world, vec3 pos) {
    float hw    = PLAYER_HALF_WIDTH;
    float min_x = pos.x - hw, max_x = pos.x + hw;
    float min_z = pos.z - hw, max_z = pos.z + hw;
    float min_y = pos.y - PLAYER_EYES_HEIGHT + 0.01f;
    float max_y = pos.y + (PLAYER_HEIGHT - PLAYER_EYES_HEIGHT) - 0.01f;
    for (int y = (int)floorf(min_y); y <= (int)floorf(max_y); y++) {
        if (world_is_solid(world, (int)floorf(min_x), y, (int)floorf(min_z))) return false;
        if (world_is_solid(world, (int)floorf(min_x), y, (int)floorf(max_z))) return false;
        if (world_is_solid(world, (int)floorf(max_x), y, (int)floorf(min_z))) return false;
        if (world_is_solid(world, (int)floorf(max_x), y, (int)floorf(max_z))) return false;
    }
    return true;
}

bool player_collides_with_block(const World *world, vec3 player_pos, BlockPos block) {
    (void)world;
    float hw     = PLAYER_HALF_WIDTH;
    float p_minx = player_pos.x - hw,        p_maxx = player_pos.x + hw;
    float p_minz = player_pos.z - hw,        p_maxz = player_pos.z + hw;
    float p_miny = player_pos.y - PLAYER_EYES_HEIGHT;
    float p_maxy = player_pos.y + (PLAYER_HEIGHT - PLAYER_EYES_HEIGHT);
    float b_minx = (float)block.x,           b_maxx = (float)block.x + 1.0f;
    float b_miny = (float)block.y,           b_maxy = (float)block.y + 1.0f;
    float b_minz = (float)block.z,           b_maxz = (float)block.z + 1.0f;
    return (p_minx < b_maxx && p_maxx > b_minx) &&
           (p_miny < b_maxy && p_maxy > b_miny) &&
           (p_minz < b_maxz && p_maxz > b_minz);
}

static uint16_t palette_index_for_id(const char **ids, uint16_t *count, const char *id) {
    for (uint16_t i = 0; i < *count; i++) if (strcmp(ids[i], id) == 0) return i;
    if (*count >= 256) return 0;
    ids[*count] = id;
    return (*count)++;
}

static bool save_chunk_data(const LoadedChunk *lc) {
    if (!lc || !lc->active || !lc->chunk) return false;
    ensure_save_dirs();
    const Chunk *chunk = lc->chunk;
    char path[256], tmp[280];
    chunk_save_path(path, sizeof(path), chunk->x, chunk->z);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) { LOG_WARN(CAT_WORLD, "Failed to open chunk save %s: %s", tmp, strerror(errno)); return false; }

    const char *palette[256]; uint16_t palette_count = 0;
    uint16_t block_indices[CHUNK_VOLUME]; int n = 0;
    for (int x = 0; x < CHUNK_SIZE; x++)
        for (int y = 0; y < CHUNK_SIZE; y++)
            for (int z = 0; z < CHUNK_SIZE; z++) {
                const char *id = block_id_for_type((BlockType)chunk->blocks[x][y][z]);
                block_indices[n++] = palette_index_for_id(palette, &palette_count, id);
            }

    fwrite(CHUNK_SAVE_MAGIC, 1, 8, f);
    write_u16(f, CHUNK_SAVE_MAJOR); write_u16(f, CHUNK_SAVE_MINOR);
    write_i32(f, chunk->x); write_i32(f, chunk->z);
    write_u32(f, 2);

    uint32_t palette_size = 2;
    for (uint16_t i = 0; i < palette_count; i++) palette_size += 2 + (uint32_t)strlen(palette[i]);
    fwrite("PLTE", 1, 4, f); write_u32(f, palette_size); write_u16(f, palette_count);
    for (uint16_t i = 0; i < palette_count; i++) { uint16_t l = (uint16_t)strlen(palette[i]); write_u16(f, l); fwrite(palette[i], 1, l, f); }

    fwrite("BLKS", 1, 4, f); write_u32(f, 7 + CHUNK_VOLUME * 2);
    write_u16(f, CHUNK_SIZE); write_u16(f, CHUNK_SIZE); write_u16(f, CHUNK_SIZE);
    fputc(0, f);
    for (int i = 0; i < CHUNK_VOLUME; i++) write_u16(f, block_indices[i]);

    bool ok = ferror(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp); LOG_WARN(CAT_WORLD, "Write error for chunk save %s", tmp); return false; }
    if (rename(tmp, path) != 0) { remove(tmp); LOG_WARN(CAT_WORLD, "Rename failed: %s", strerror(errno)); return false; }
    LOG_DEBUG(CAT_WORLD, "Saved chunk %d,%d", chunk->x, chunk->z);
    return true;
}

static bool load_chunk_data(Chunk *chunk) {
    char path[256]; chunk_save_path(path, sizeof(path), chunk->x, chunk->z);
    FILE *f = fopen(path, "rb"); if (!f) return false;
    char magic[8]; uint16_t major, minor; int32_t file_x, file_z; uint32_t section_count;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, CHUNK_SAVE_MAGIC, 8) != 0 ||
        !read_u16(f, &major) || !read_u16(f, &minor) || !read_i32(f, &file_x) || !read_i32(f, &file_z) ||
        !read_u32(f, &section_count)) { LOG_WARN(CAT_WORLD, "Invalid chunk save: %s", path); fclose(f); return false; }
    (void)minor;
    if (major > CHUNK_SAVE_MAJOR) { LOG_WARN(CAT_WORLD, "Newer chunk save version %u: %s", major, path); fclose(f); return false; }
    if (file_x != chunk->x || file_z != chunk->z) { LOG_WARN(CAT_WORLD, "Coord mismatch in %s", path); fclose(f); return false; }

    SavePaletteEntry palette[256]; uint16_t palette_count = 0;
    for (int i = 0; i < 256; i++) { palette[i].id[0] = '\0'; palette[i].type = BLOCK_AIR; }
    bool loaded_blocks = false;

    for (uint32_t s = 0; s < section_count; s++) {
        char type[4]; uint32_t size; long pstart;
        if (fread(type, 1, 4, f) != 4 || !read_u32(f, &size)) break;
        pstart = ftell(f);
        if (memcmp(type, "PLTE", 4) == 0) {
            uint16_t count, loaded = 0;
            if (!read_u16(f, &count)) break;
            if (count > 256) count = 256;
            for (uint16_t i = 0; i < count; i++) {
                uint16_t file_len, copy_len;
                if (!read_u16(f, &file_len)) break;
                copy_len = file_len < (uint16_t)(sizeof(palette[i].id) - 1) ? file_len : (uint16_t)(sizeof(palette[i].id) - 1);
                if (fread(palette[i].id, 1, copy_len, f) != copy_len) break;
                if (file_len > copy_len) fseek(f, (long)(file_len - copy_len), SEEK_CUR);
                palette[i].id[copy_len] = '\0';
                palette[i].type = block_type_for_id(palette[i].id);
                if (palette[i].type == BLOCK_AIR && strcmp(palette[i].id, "kyub:air") != 0)
                    LOG_WARN(CAT_WORLD, "Unknown block id in save: %s", palette[i].id);
                loaded++;
            }
            palette_count = loaded;
        } else if (memcmp(type, "BLKS", 4) == 0) {
            uint16_t sx, sy, sz; int enc;
            if (!read_u16(f, &sx) || !read_u16(f, &sy) || !read_u16(f, &sz)) break;
            enc = fgetc(f);
            if (sx == CHUNK_SIZE && sy == CHUNK_SIZE && sz == CHUNK_SIZE && enc == 0) {
                for (int x = 0; x < CHUNK_SIZE; x++)
                    for (int y = 0; y < CHUNK_SIZE; y++)
                        for (int z = 0; z < CHUNK_SIZE; z++) {
                            uint16_t idx;
                            if (!read_u16(f, &idx)) goto done;
                            chunk->blocks[x][y][z] = (idx < palette_count) ? palette[idx].type : BLOCK_AIR;
                        }
                loaded_blocks = true;
            }
        }
        if (fseek(f, pstart + (long)size, SEEK_SET) != 0) break;
    }
done:
    fclose(f);
    if (loaded_blocks) LOG_DEBUG(CAT_WORLD, "Loaded saved chunk %d,%d", chunk->x, chunk->z);
    return loaded_blocks;
}

void world_init(World *world, int render_distance) {
    if (render_distance < 1) render_distance = 1;
    if (render_distance > MAX_RENDER_DISTANCE) render_distance = MAX_RENDER_DISTANCE;
    world->render_distance     = render_distance;
    world->capacity            = (2 * MAX_RENDER_DISTANCE + 1) * (2 * MAX_RENDER_DISTANCE + 1);
    world->chunks              = calloc(world->capacity, sizeof(LoadedChunk));
    world->count               = 0;
    world->mesh_compute_program = R_INVALID_HANDLE;
    ensure_save_dirs();
}

static int world_chunk_coord(int x) {
    if (x >= 0) return x / CHUNK_SIZE;
    return -((-x + CHUNK_SIZE - 1) / CHUNK_SIZE);
}

static bool chunk_is_loaded(const World *world, int x, int z) {
    for (int i = 0; i < world->capacity; i++)
        if (world->chunks[i].active && world->chunks[i].chunk->x == x && world->chunks[i].chunk->z == z)
            return true;
    return false;
}

static void load_chunk(World *world, int x, int z) {
    for (int i = 0; i < world->capacity; i++) {
        if (!world->chunks[i].active) {
            world->chunks[i].chunk = malloc(sizeof(Chunk));
            chunk_init(world->chunks[i].chunk, x, z);
            load_chunk_data(world->chunks[i].chunk);
            world->chunks[i].mesh = malloc(sizeof(Mesh));
            mesh_init(world->chunks[i].mesh);
            world->chunks[i].voxel_tex = R_INVALID_HANDLE;
            /* voxel_upload_texture only needed for GPU compute meshing */
            if (world->mesh_compute_program != R_INVALID_HANDLE)
                voxel_upload_texture(&world->chunks[i].voxel_tex, world->chunks[i].chunk);
            mesh_generate_greedy(world->chunks[i].mesh, world->chunks[i].chunk);
            mesh_upload(world->chunks[i].mesh);
            world->chunks[i].active = true;
            world->chunks[i].dirty = world->chunks[i].save_dirty = false;
            world->count++;
            LOG_DEBUG(CAT_WORLD, "Loaded chunk %d,%d (slot %d)", x, z, i);
            return;
        }
    }
    LOG_WARN(CAT_WORLD, "No free slot for chunk %d,%d (active=%d, cap=%d)", x, z, world->count, world->capacity);
}

static void unload_chunk(World *world, int index) {
    if (!world->chunks[index].active) return;
    int cx = world->chunks[index].chunk->x;
    int cz = world->chunks[index].chunk->z;
    if (world->chunks[index].save_dirty) save_chunk_data(&world->chunks[index]);
    mesh_free(world->chunks[index].mesh); free(world->chunks[index].mesh);
    free(world->chunks[index].chunk);
    if (world->chunks[index].voxel_tex != R_INVALID_HANDLE)
        renderer_destroy_texture(world->chunks[index].voxel_tex);
    world->chunks[index].active = false;
    world->count--;
    LOG_DEBUG(CAT_WORLD, "Unloaded chunk %d,%d", cx, cz);
}

void world_flush_saves(World *world) {
    if (!world) return;
    for (int i = 0; i < world->capacity; i++)
        if (world->chunks[i].active && world->chunks[i].save_dirty)
            if (save_chunk_data(&world->chunks[i])) world->chunks[i].save_dirty = false;
}

void world_update(World *world, vec3 camera_pos) {
    int cx = world_chunk_coord((int)floorf(camera_pos.x));
    int cz = world_chunk_coord((int)floorf(camera_pos.z));
    int load_dist = world->render_distance + 1;
    for (int x = cx - load_dist; x <= cx + load_dist; x++)
        for (int z = cz - load_dist; z <= cz + load_dist; z++)
            if (!chunk_is_loaded(world, x, z)) load_chunk(world, x, z);

    for (int i = 0; i < world->capacity; i++) {
        if (!world->chunks[i].active) continue;
        int dx = abs(world->chunks[i].chunk->x - cx);
        int dz = abs(world->chunks[i].chunk->z - cz);
        if (dx > world->render_distance + 2 || dz > world->render_distance + 2)
            unload_chunk(world, i);
    }

    for (int i = 0; i < world->capacity; i++) {
        if (world->chunks[i].active && world->chunks[i].dirty) {
            mesh_generate_greedy(world->chunks[i].mesh, world->chunks[i].chunk);
            mesh_upload(world->chunks[i].mesh);
            world->chunks[i].dirty = false;
        }
    }
}

void world_free(World *world) {
    for (int i = 0; i < world->capacity; i++)
        if (world->chunks[i].active) unload_chunk(world, i);
    free(world->chunks);
}

static LoadedChunk *find_chunk(const World *world, int cx, int cz) {
    for (int i = 0; i < world->capacity; i++)
        if (world->chunks[i].active && world->chunks[i].chunk->x == cx && world->chunks[i].chunk->z == cz)
            return &world->chunks[i];
    return NULL;
}

BlockType world_get_block(const World *world, int x, int y, int z) {
    if (y < 0 || y >= CHUNK_SIZE) return BLOCK_AIR;
    int cx = world_chunk_coord(x), cz = world_chunk_coord(z);
    LoadedChunk *lc = find_chunk(world, cx, cz);
    if (!lc) { LOG_WARN(CAT_WORLD, "get_block: chunk %d,%d not loaded", cx, cz); return BLOCK_AIR; }
    return lc->chunk->blocks[x - cx * CHUNK_SIZE][y][z - cz * CHUNK_SIZE];
}

bool world_is_solid(const World *world, int x, int y, int z) {
    return world_get_block(world, x, y, z) != BLOCK_AIR;
}

void world_set_block(World *world, int x, int y, int z, BlockType type) {
    if (y < 0 || y >= CHUNK_SIZE) return;
    int cx = world_chunk_coord(x), cz = world_chunk_coord(z);
    LoadedChunk *lc = find_chunk(world, cx, cz);
    if (!lc && world->count < world->capacity) { load_chunk(world, cx, cz); lc = find_chunk(world, cx, cz); }
    if (!lc) { LOG_WARN(CAT_WORLD, "set_block: chunk %d,%d not loaded", cx, cz); return; }
    lc->chunk->blocks[x - cx * CHUNK_SIZE][y][z - cz * CHUNK_SIZE] = type;
    lc->dirty = lc->save_dirty = true;
}
