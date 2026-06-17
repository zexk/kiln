#pragma once

#include <stdint.h>

#include "linalg.h"

/* The canonical vertex the renderer's mesh pipeline consumes: position plus a
   smooth normal for shading. Texture coords come later when there's a textured
   material path. */
typedef struct {
    vec3_t position;
    vec3_t normal;
    vec2_t uv;
} mesh_vertex_t;

/* A CPU-side indexed triangle mesh. Heap-owned; release with cpu_mesh_free.
   Producers (the OBJ loader, procedural generators) fill it; the renderer
   uploads it to a GPU buffer and the CPU copy can then be freed. */
typedef struct {
    mesh_vertex_t *vertices;
    uint32_t vertex_count;
    uint32_t *indices; /* 3 per triangle */
    uint32_t index_count;
} cpu_mesh_t;

void cpu_mesh_free(cpu_mesh_t *mesh);

/* Recompute every vertex normal as the area-weighted average of the faces that
   touch it (smooth shading). Expects positions and indices already set. */
void cpu_mesh_compute_normals(cpu_mesh_t *mesh);

/* Axis-aligned bounds over all vertex positions. Returns false for an empty
   mesh. Used to centre and scale loaded models to a viewable size. */
bool cpu_mesh_bounds(const cpu_mesh_t *mesh, vec3_t *out_min, vec3_t *out_max);

/* Translate all vertices so the AABB centre sits at the origin. Lets an entity
   transform's translation act as the model's visual pivot. Returns the centre
   that was removed (zero for an empty mesh). */
vec3_t cpu_mesh_recenter(cpu_mesh_t *mesh);

/* Generate a unit cube centred on the origin with per-face flat normals and
   box-mapped UVs — a dependency-free baseline mesh that always renders. */
bool cpu_mesh_cube(cpu_mesh_t *out);
