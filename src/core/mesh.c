#include "mesh.h"

#include <stdlib.h>

void cpu_mesh_free(cpu_mesh_t *mesh) {
    if (!mesh) {
        return;
    }
    free(mesh->vertices);
    free(mesh->indices);
    mesh->vertices = NULL;
    mesh->indices = NULL;
    mesh->vertex_count = 0;
    mesh->index_count = 0;
}

void cpu_mesh_compute_normals(cpu_mesh_t *mesh) {
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        mesh->vertices[i].normal = (vec3_t){0.0f, 0.0f, 0.0f};
    }

    /* Accumulate raw face normals (cross product, not normalized) so larger
       triangles weight the average more — the usual smooth-normal heuristic. */
    for (uint32_t i = 0; i + 2 < mesh->index_count; i += 3) {
        uint32_t a = mesh->indices[i];
        uint32_t b = mesh->indices[i + 1];
        uint32_t c = mesh->indices[i + 2];
        vec3_t pa = mesh->vertices[a].position;
        vec3_t pb = mesh->vertices[b].position;
        vec3_t pc = mesh->vertices[c].position;
        vec3_t fn = vec3_cross(vec3_sub(pb, pa), vec3_sub(pc, pa));
        mesh->vertices[a].normal = vec3_add(mesh->vertices[a].normal, fn);
        mesh->vertices[b].normal = vec3_add(mesh->vertices[b].normal, fn);
        mesh->vertices[c].normal = vec3_add(mesh->vertices[c].normal, fn);
    }

    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        vec3_t n = mesh->vertices[i].normal;
        if (vec3_length_sq(n) > 1e-12f) {
            mesh->vertices[i].normal = vec3_normalize(n);
        } else {
            mesh->vertices[i].normal = (vec3_t){0.0f, 1.0f, 0.0f};
        }
    }
}

bool cpu_mesh_bounds(const cpu_mesh_t *mesh, vec3_t *out_min, vec3_t *out_max) {
    if (mesh->vertex_count == 0) {
        return false;
    }
    vec3_t lo = mesh->vertices[0].position;
    vec3_t hi = lo;
    for (uint32_t i = 1; i < mesh->vertex_count; i++) {
        vec3_t p = mesh->vertices[i].position;
        lo.x = p.x < lo.x ? p.x : lo.x;
        lo.y = p.y < lo.y ? p.y : lo.y;
        lo.z = p.z < lo.z ? p.z : lo.z;
        hi.x = p.x > hi.x ? p.x : hi.x;
        hi.y = p.y > hi.y ? p.y : hi.y;
        hi.z = p.z > hi.z ? p.z : hi.z;
    }
    *out_min = lo;
    *out_max = hi;
    return true;
}

vec3_t cpu_mesh_recenter(cpu_mesh_t *mesh) {
    vec3_t lo, hi;
    if (!cpu_mesh_bounds(mesh, &lo, &hi)) {
        return (vec3_t){0.0f, 0.0f, 0.0f};
    }
    vec3_t center = vec3_scale(vec3_add(lo, hi), 0.5f);
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        mesh->vertices[i].position =
            vec3_sub(mesh->vertices[i].position, center);
    }
    return center;
}

bool cpu_mesh_cube(cpu_mesh_t *out) {
    static const vec3_t pos[8] = {
        {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f},
        {-0.5f, 0.5f, -0.5f},  {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f},
        {0.5f, 0.5f, 0.5f},    {-0.5f, 0.5f, 0.5f},
    };
    static const uint32_t idx[36] = {
        0, 1, 2, 2, 3, 0, /* -Z */
        5, 4, 7, 7, 6, 5, /* +Z */
        4, 0, 3, 3, 7, 4, /* -X */
        1, 5, 6, 6, 2, 1, /* +X */
        4, 5, 1, 1, 0, 4, /* -Y */
        3, 2, 6, 6, 7, 3, /* +Y */
    };

    out->vertices = malloc(sizeof(mesh_vertex_t) * 8);
    out->indices = malloc(sizeof(uint32_t) * 36);
    if (!out->vertices || !out->indices) {
        cpu_mesh_free(out);
        return false;
    }
    out->vertex_count = 8;
    out->index_count = 36;
    for (uint32_t i = 0; i < 8; i++) {
        out->vertices[i].position = pos[i];
        out->vertices[i].uv = (vec2_t){0.0f, 0.0f}; /* untextured */
    }
    for (uint32_t i = 0; i < 36; i++) {
        out->indices[i] = idx[i];
    }
    cpu_mesh_compute_normals(out);
    return true;
}
