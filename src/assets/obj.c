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
    uint32_t *data;
    size_t count;
    size_t cap;
} u32_vec;

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

static bool u32_push(u32_vec *v, uint32_t x) {
    if (v->count == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 1024;
        uint32_t *p = realloc(v->data, cap * sizeof(*p));
        if (!p) {
            return false;
        }
        v->data = p;
        v->cap = cap;
    }
    v->data[v->count++] = x;
    return true;
}

/* Resolve an OBJ position index (1-based, negative = relative to the end) to a
   0-based index, or -1 if out of range. */
static long resolve_index(long raw, size_t count) {
    long zero = (raw > 0) ? raw - 1 : (long)count + raw;
    return (zero >= 0 && zero < (long)count) ? zero : -1;
}

bool obj_load(const char *path, cpu_mesh_t *out) {
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[obj] cannot open '%s'\n", path);
        return false;
    }

    vec3_vec positions = {0};
    u32_vec indices = {0};
    bool ok = true;
    char line[1024];

    while (ok && fgets(line, sizeof(line), f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            vec3_t p;
            if (sscanf(line + 2, "%f %f %f", &p.x, &p.y, &p.z) == 3) {
                ok = vec3_push(&positions, p);
            }
        } else if (line[0] == 'f' && line[1] == ' ') {
            /* Collect each corner's position index, then fan-triangulate. */
            long corners[64];
            int n = 0;
            char *tok = strtok(line + 2, " \t\r\n");
            while (tok && n < 64) {
                long raw = strtol(tok, NULL, 10); /* stops at '/' */
                long idx = resolve_index(raw, positions.count);
                if (idx >= 0) {
                    corners[n++] = idx;
                }
                tok = strtok(NULL, " \t\r\n");
            }
            for (int k = 1; k + 1 < n; k++) {
                ok = u32_push(&indices, (uint32_t)corners[0]) &&
                     u32_push(&indices, (uint32_t)corners[k]) &&
                     u32_push(&indices, (uint32_t)corners[k + 1]);
                if (!ok) {
                    break;
                }
            }
        }
        /* everything else (vt, vn, #, g, o, s, usemtl, ...) is ignored */
    }
    fclose(f);

    if (ok && (positions.count == 0 || indices.count == 0)) {
        fprintf(stderr, "[obj] '%s' has no geometry\n", path);
        ok = false;
    }

    if (ok) {
        out->vertex_count = (uint32_t)positions.count;
        out->index_count = (uint32_t)indices.count;
        out->vertices = malloc(sizeof(mesh_vertex_t) * out->vertex_count);
        out->indices = indices.data; /* hand off ownership */
        indices.data = NULL;
        if (!out->vertices) {
            ok = false;
        } else {
            for (uint32_t i = 0; i < out->vertex_count; i++) {
                out->vertices[i].position = positions.data[i];
            }
            cpu_mesh_compute_normals(out);
        }
    }

    free(positions.data);
    free(indices.data);
    if (!ok) {
        cpu_mesh_free(out);
        memset(out, 0, sizeof(*out));
    } else {
        fprintf(stderr, "[obj] loaded '%s': %u verts, %u tris\n", path,
                out->vertex_count, out->index_count / 3);
    }
    return ok;
}
