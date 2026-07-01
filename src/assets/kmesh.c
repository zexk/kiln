#include "kmesh.h"

#include "fs.h"
#include "mesh.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t vcount;
    uint32_t icount;
} kmesh_header_t;

typedef char kmesh_header_size_check[sizeof(kmesh_header_t) == 16 ? 1 : -1];

/* ---------- save ---------- */

typedef struct {
    const cpu_mesh_t *mesh;
    bool              idx16;
    bool              has_uvs;
} kmesh_write_ctx_t;

static bool write_kmesh(FILE *f, void *vctx) {
    const kmesh_write_ctx_t *ctx = vctx;
    const cpu_mesh_t        *m   = ctx->mesh;

    kmesh_header_t h;
    h.magic   = KMESH_MAGIC;
    h.version = KMESH_VERSION;
    h.flags   = (ctx->idx16 ? KMESH_FLAG_IDX16 : 0u) |
                (ctx->has_uvs ? KMESH_FLAG_HAS_UVS : 0u);
    h.vcount  = m->vertex_count;
    h.icount  = m->index_count;
    if (fwrite(&h, sizeof(h), 1, f) != 1) {
        return false;
    }

    /* Positions: extract from interleaved vertex array into a contiguous block. */
    vec3_t *pos = malloc(m->vertex_count * sizeof(vec3_t));
    if (!pos) {
        return false;
    }
    for (uint32_t i = 0; i < m->vertex_count; i++) {
        pos[i] = m->vertices[i].position;
    }
    bool ok = fwrite(pos, sizeof(vec3_t), m->vertex_count, f) == m->vertex_count;
    free(pos);
    if (!ok) {
        return false;
    }

    /* UVs (optional). */
    if (ctx->has_uvs) {
        vec2_t *uvs = malloc(m->vertex_count * sizeof(vec2_t));
        if (!uvs) {
            return false;
        }
        for (uint32_t i = 0; i < m->vertex_count; i++) {
            uvs[i] = m->vertices[i].uv;
        }
        ok = fwrite(uvs, sizeof(vec2_t), m->vertex_count, f) == m->vertex_count;
        free(uvs);
        if (!ok) {
            return false;
        }
    }

    /* Indices. */
    if (ctx->idx16) {
        uint16_t *idx = malloc(m->index_count * sizeof(uint16_t));
        if (!idx) {
            return false;
        }
        for (uint32_t i = 0; i < m->index_count; i++) {
            idx[i] = (uint16_t)m->indices[i];
        }
        ok = fwrite(idx, sizeof(uint16_t), m->index_count, f) == m->index_count;
        free(idx);
        if (!ok) {
            return false;
        }
    } else {
        if (fwrite(m->indices, sizeof(uint32_t), m->index_count, f) !=
            m->index_count) {
            return false;
        }
    }

    return true;
}

bool kmesh_save(const char *path, const cpu_mesh_t *mesh) {
    if (!mesh || mesh->vertex_count == 0 || mesh->index_count == 0) {
        fprintf(stderr, "[kmesh] empty mesh, skipping '%s'\n", path);
        return false;
    }

    /* Any non-zero UV signals a textured mesh; otherwise skip the UV block. */
    bool has_uvs = false;
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        if (mesh->vertices[i].uv.x != 0.0f || mesh->vertices[i].uv.y != 0.0f) {
            has_uvs = true;
            break;
        }
    }

    kmesh_write_ctx_t ctx;
    ctx.mesh    = mesh;
    ctx.idx16   = mesh->vertex_count <= 0xFFFFu;
    ctx.has_uvs = has_uvs;

    if (!fs_write_atomic(path, write_kmesh, &ctx)) {
        fprintf(stderr, "[kmesh] save failed: '%s'\n", path);
        return false;
    }

    size_t pos_bytes = (size_t)mesh->vertex_count * sizeof(vec3_t);
    size_t uv_bytes  = has_uvs ? (size_t)mesh->vertex_count * sizeof(vec2_t) : 0;
    size_t idx_bytes = ctx.idx16 ? (size_t)mesh->index_count * sizeof(uint16_t)
                                 : (size_t)mesh->index_count * sizeof(uint32_t);
    size_t total     = sizeof(kmesh_header_t) + pos_bytes + uv_bytes + idx_bytes;

    fprintf(stderr, "[kmesh] saved '%s': %u verts %u tris %zu bytes\n",
            path, mesh->vertex_count, mesh->index_count / 3, total);
    return true;
}

/* ---------- load ---------- */

bool kmesh_load(const char *path, cpu_mesh_t *out) {
    memset(out, 0, sizeof(*out));

    size_t size;
    char  *data = fs_read_file(path, &size);
    if (!data) {
        return false;
    }

    if (size < sizeof(kmesh_header_t)) {
        fprintf(stderr, "[kmesh] '%s': too small for header\n", path);
        free(data);
        return false;
    }

    kmesh_header_t h;
    memcpy(&h, data, sizeof(h));

    if (h.magic != KMESH_MAGIC) {
        fprintf(stderr, "[kmesh] '%s': bad magic\n", path);
        free(data);
        return false;
    }
    if (h.version != KMESH_VERSION) {
        fprintf(stderr, "[kmesh] '%s': unsupported version %u\n", path,
                (unsigned)h.version);
        free(data);
        return false;
    }
    if (h.vcount == 0 || h.icount == 0 || h.icount % 3 != 0) {
        fprintf(stderr, "[kmesh] '%s': invalid counts v=%u i=%u\n", path,
                h.vcount, h.icount);
        free(data);
        return false;
    }

    bool idx16   = (h.flags & KMESH_FLAG_IDX16)   != 0;
    bool has_uvs = (h.flags & KMESH_FLAG_HAS_UVS) != 0;

    size_t pos_bytes = (size_t)h.vcount * sizeof(vec3_t);
    size_t uv_bytes  = has_uvs ? (size_t)h.vcount * sizeof(vec2_t) : 0;
    size_t idx_bytes = idx16 ? (size_t)h.icount * sizeof(uint16_t)
                              : (size_t)h.icount * sizeof(uint32_t);
    size_t expected  = sizeof(kmesh_header_t) + pos_bytes + uv_bytes + idx_bytes;

    if (size < expected) {
        fprintf(stderr, "[kmesh] '%s': truncated (have %zu need %zu)\n", path,
                size, expected);
        free(data);
        return false;
    }

    out->vertices = calloc(h.vcount, sizeof(mesh_vertex_t));
    out->indices  = malloc((size_t)h.icount * sizeof(uint32_t));
    if (!out->vertices || !out->indices) {
        free(out->vertices);
        free(out->indices);
        free(data);
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->vertex_count = h.vcount;
    out->index_count  = h.icount;

    const char *p = data + sizeof(kmesh_header_t);

    for (uint32_t i = 0; i < h.vcount; i++) {
        memcpy(&out->vertices[i].position, p, sizeof(vec3_t));
        p += sizeof(vec3_t);
    }
    if (has_uvs) {
        for (uint32_t i = 0; i < h.vcount; i++) {
            memcpy(&out->vertices[i].uv, p, sizeof(vec2_t));
            p += sizeof(vec2_t);
        }
    }
    if (idx16) {
        for (uint32_t i = 0; i < h.icount; i++) {
            uint16_t v;
            memcpy(&v, p, sizeof(uint16_t));
            out->indices[i] = v;
            p += sizeof(uint16_t);
        }
    } else {
        memcpy(out->indices, p, (size_t)h.icount * sizeof(uint32_t));
    }

    free(data);

    /* Reject out-of-range indices: cpu_mesh_compute_normals indexes vertices
       through them, so a corrupt file would read/write out of bounds. */
    for (uint32_t i = 0; i < h.icount; i++) {
        if (out->indices[i] >= h.vcount) {
            fprintf(stderr, "[kmesh] '%s': index %u out of range (vcount %u)\n",
                    path, out->indices[i], h.vcount);
            cpu_mesh_free(out);
            return false;
        }
    }

    cpu_mesh_compute_normals(out);

    fprintf(stderr, "[kmesh] loaded '%s': %u verts %u tris\n", path,
            out->vertex_count, out->index_count / 3);
    return true;
}
