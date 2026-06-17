#include "obj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal growable arrays — the parser doesn't know counts up front. */
typedef struct {
    vec3_t *data;
    size_t count;
    size_t cap;
} vec3_vec;

typedef struct {
    vec2_t *data;
    size_t count;
    size_t cap;
} vec2_vec;

/* A face corner after triangulation: indices into positions and uvs (uv == -1
   when the face had no texcoord). */
typedef struct {
    uint32_t pos;
    int32_t uv;
} corner_t;

typedef struct {
    corner_t *data;
    size_t count;
    size_t cap;
} corner_vec;

static bool vec3_push(vec3_vec *v, vec3_t x) {
    if (v->count == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 1024;
        vec3_t *p = realloc(v->data, cap * sizeof(*p));
        if (!p) {
            return false;
        }
        v->data = p;
        v->cap = cap;
    }
    v->data[v->count++] = x;
    return true;
}

static bool vec2_push(vec2_vec *v, vec2_t x) {
    if (v->count == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 1024;
        vec2_t *p = realloc(v->data, cap * sizeof(*p));
        if (!p) {
            return false;
        }
        v->data = p;
        v->cap = cap;
    }
    v->data[v->count++] = x;
    return true;
}

static bool corner_push(corner_vec *v, corner_t x) {
    if (v->count == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 1024;
        corner_t *p = realloc(v->data, cap * sizeof(*p));
        if (!p) {
            return false;
        }
        v->data = p;
        v->cap = cap;
    }
    v->data[v->count++] = x;
    return true;
}

/* Resolve an OBJ index (1-based, negative = relative to the end) to 0-based,
   or -1 if out of range. */
static long resolve_index(long raw, size_t count) {
    long zero = (raw > 0) ? raw - 1 : (long)count + raw;
    return (zero >= 0 && zero < (long)count) ? zero : -1;
}

/* Parse one face vertex token ("v", "v/vt", "v/vt/vn", "v//vn"). */
static corner_t parse_corner(const char *tok, size_t npos, size_t nuv) {
    corner_t c = {0, -1};
    long praw = strtol(tok, NULL, 10);
    long p = resolve_index(praw, npos);
    c.pos = (p >= 0) ? (uint32_t)p : 0;

    const char *slash = strchr(tok, '/');
    if (slash && slash[1] != '/' && slash[1] != '\0') {
        long uraw = strtol(slash + 1, NULL, 10);
        long u = resolve_index(uraw, nuv);
        c.uv = (u >= 0) ? (int32_t)u : -1;
    }
    return c;
}

bool obj_load(const char *path, cpu_mesh_t *out) {
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[obj] cannot open '%s'\n", path);
        return false;
    }

    vec3_vec positions = {0};
    vec2_vec uvs = {0};
    corner_vec corners = {0}; /* triangulated; 3 per triangle */
    bool ok = true;
    char line[1024];

    while (ok && fgets(line, sizeof(line), f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            vec3_t p;
            if (sscanf(line + 2, "%f %f %f", &p.x, &p.y, &p.z) == 3) {
                ok = vec3_push(&positions, p);
            }
        } else if (line[0] == 'v' && line[1] == 't') {
            vec2_t t;
            if (sscanf(line + 2, "%f %f", &t.x, &t.y) == 2) {
                ok = vec2_push(&uvs, t);
            }
        } else if (line[0] == 'f' && line[1] == ' ') {
            corner_t face[64];
            int n = 0;
            char *tok = strtok(line + 2, " \t\r\n");
            while (tok && n < 64) {
                face[n++] = parse_corner(tok, positions.count, uvs.count);
                tok = strtok(NULL, " \t\r\n");
            }
            for (int k = 1; k + 1 < n; k++) { /* fan triangulation */
                ok = corner_push(&corners, face[0]) &&
                     corner_push(&corners, face[k]) &&
                     corner_push(&corners, face[k + 1]);
                if (!ok) {
                    break;
                }
            }
        }
        /* vn is ignored: normals are always recomputed smooth below */
    }
    fclose(f);

    if (ok && (positions.count == 0 || corners.count == 0)) {
        fprintf(stderr, "[obj] '%s' has no geometry\n", path);
        ok = false;
    }

    /* Per-position smooth normals (computed before de-indexing so vertices that
       split only because of a uv seam still share one normal — no shading
       seam). */
    vec3_t *pos_normals = NULL;
    if (ok) {
        pos_normals = calloc(positions.count, sizeof(vec3_t));
        ok = pos_normals != NULL;
    }
    if (ok) {
        for (size_t i = 0; i + 2 < corners.count; i += 3) {
            uint32_t a = corners.data[i].pos;
            uint32_t b = corners.data[i + 1].pos;
            uint32_t c = corners.data[i + 2].pos;
            vec3_t fn = vec3_cross(vec3_sub(positions.data[b], positions.data[a]),
                                   vec3_sub(positions.data[c], positions.data[a]));
            pos_normals[a] = vec3_add(pos_normals[a], fn);
            pos_normals[b] = vec3_add(pos_normals[b], fn);
            pos_normals[c] = vec3_add(pos_normals[c], fn);
        }
        for (size_t i = 0; i < positions.count; i++) {
            pos_normals[i] = (vec3_length_sq(pos_normals[i]) > 1e-12f)
                                 ? vec3_normalize(pos_normals[i])
                                 : (vec3_t){0.0f, 1.0f, 0.0f};
        }
    }

    /* De-index into unique (position, uv) vertices. A per-position chain keeps
       the lookup O(1) without a hash table (chains are 1-2 long in practice). */
    mesh_vertex_t *verts = NULL;
    uint32_t *indices = NULL;
    int32_t *chain_uv = NULL;
    int32_t *chain_next = NULL;
    int32_t *first = NULL;
    if (ok) {
        verts = malloc(corners.count * sizeof(*verts));
        indices = malloc(corners.count * sizeof(*indices));
        chain_uv = malloc(corners.count * sizeof(*chain_uv));
        chain_next = malloc(corners.count * sizeof(*chain_next));
        first = malloc(positions.count * sizeof(*first));
        ok = verts && indices && chain_uv && chain_next && first;
    }
    if (ok) {
        for (size_t i = 0; i < positions.count; i++) {
            first[i] = -1;
        }
        uint32_t vcount = 0;
        for (size_t i = 0; i < corners.count; i++) {
            corner_t c = corners.data[i];
            int32_t v = first[c.pos];
            while (v >= 0 && chain_uv[v] != c.uv) {
                v = chain_next[v];
            }
            if (v < 0) {
                v = (int32_t)vcount++;
                verts[v].position = positions.data[c.pos];
                verts[v].normal = pos_normals[c.pos];
                verts[v].uv = (c.uv >= 0) ? uvs.data[c.uv]
                                          : (vec2_t){0.0f, 0.0f};
                chain_uv[v] = c.uv;
                chain_next[v] = first[c.pos];
                first[c.pos] = v;
            }
            indices[i] = (uint32_t)v;
        }
        out->vertices = verts;
        out->vertex_count = vcount;
        out->indices = indices;
        out->index_count = (uint32_t)corners.count;
        verts = NULL; /* ownership handed to out */
        indices = NULL;
    }

    free(positions.data);
    free(uvs.data);
    free(corners.data);
    free(pos_normals);
    free(chain_uv);
    free(chain_next);
    free(first);
    free(verts);
    free(indices);

    if (!ok) {
        cpu_mesh_free(out);
        memset(out, 0, sizeof(*out));
    } else {
        fprintf(stderr, "[obj] loaded '%s': %u verts, %u tris\n", path,
                out->vertex_count, out->index_count / 3);
    }
    return ok;
}
