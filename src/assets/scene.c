#include "scene.h"

#include "fs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Binary scene format (.kscn):
     kscene_header_t (16 bytes)
     kscene_entity_t × count (104 bytes each)

   Entity record layout (no padding, little-endian):
     char  name[64]     prototype name
     float pos[3]       world position
     float rot[4]       quaternion xyzw
     float scale[3]     per-axis scale  */

#define KSCENE_MAGIC   0x4E435349U /* 'I','S','C','N' */
#define KSCENE_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t count;
    uint32_t pad;
} kscene_header_t;

#define KSCENE_ENTITY_BYTES (SCENE_NAME_MAX + 3*4 + 4*4 + 3*4) /* 104 */

typedef char kscene_header_size_check[sizeof(kscene_header_t) == 16 ? 1 : -1];

/* ---------- save ---------- */

typedef struct {
    const scene_entity_t *entities;
    int                   count;
} kscene_write_ctx_t;

static bool write_kscene(FILE *f, void *vctx) {
    const kscene_write_ctx_t *ctx = vctx;

    kscene_header_t h;
    h.magic   = KSCENE_MAGIC;
    h.version = KSCENE_VERSION;
    h.flags   = 0;
    h.count   = (uint32_t)ctx->count;
    h.pad     = 0;
    if (fwrite(&h, sizeof(h), 1, f) != 1) {
        return false;
    }

    for (int i = 0; i < ctx->count; i++) {
        const scene_entity_t *e = &ctx->entities[i];
        float pos[3] = {e->position.x, e->position.y, e->position.z};
        float rot[4] = {e->rotation.x, e->rotation.y, e->rotation.z,
                        e->rotation.w};
        float scl[3] = {e->scale.x, e->scale.y, e->scale.z};

        if (fwrite(e->name, SCENE_NAME_MAX, 1, f) != 1 ||
            fwrite(pos, sizeof(pos), 1, f) != 1 ||
            fwrite(rot, sizeof(rot), 1, f) != 1 ||
            fwrite(scl, sizeof(scl), 1, f) != 1) {
            return false;
        }
    }
    return true;
}

bool scene_save(const char *path, const scene_entity_t *entities, int count) {
    kscene_write_ctx_t ctx = {entities, count};
    if (!fs_write_atomic(path, write_kscene, &ctx)) {
        fprintf(stderr, "[scene] save failed: '%s'\n", path);
        return false;
    }
    fprintf(stderr, "[scene] saved %d entities to '%s'\n", count, path);
    return true;
}

/* ---------- load ---------- */

int scene_load(const char *path, scene_entity_t *out, int max_count) {
    size_t size;
    char  *data = fs_read_file(path, &size);
    if (!data) {
        return -1;
    }

    if (size < sizeof(kscene_header_t)) {
        fprintf(stderr, "[scene] '%s': too small\n", path);
        free(data);
        return -1;
    }

    kscene_header_t h;
    memcpy(&h, data, sizeof(h));

    if (h.magic != KSCENE_MAGIC || h.version != KSCENE_VERSION) {
        fprintf(stderr, "[scene] '%s': bad header (magic=%08x ver=%u)\n", path,
                h.magic, (unsigned)h.version);
        free(data);
        return -1;
    }

    int count = (int)h.count;
    size_t expected =
        sizeof(kscene_header_t) + (size_t)count * KSCENE_ENTITY_BYTES;
    if (size < expected) {
        fprintf(stderr, "[scene] '%s': truncated\n", path);
        free(data);
        return -1;
    }

    if (count > max_count) {
        fprintf(stderr, "[scene] truncating %d to %d\n", count, max_count);
        count = max_count;
    }

    const char *p = data + sizeof(kscene_header_t);
    for (int i = 0; i < count; i++) {
        scene_entity_t *e = &out[i];
        float pos[3], rot[4], scl[3];

        memcpy(e->name, p, SCENE_NAME_MAX); p += SCENE_NAME_MAX;
        memcpy(pos,     p, sizeof(pos));    p += sizeof(pos);
        memcpy(rot,     p, sizeof(rot));    p += sizeof(rot);
        memcpy(scl,     p, sizeof(scl));    p += sizeof(scl);

        e->position = (vec3_t){pos[0], pos[1], pos[2]};
        e->rotation = (quat_t){rot[0], rot[1], rot[2], rot[3]};
        e->scale    = (vec3_t){scl[0], scl[1], scl[2]};
    }

    free(data);
    fprintf(stderr, "[scene] loaded %d entities from '%s'\n", count, path);
    return count;
}
