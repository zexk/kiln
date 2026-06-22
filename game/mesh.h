#pragma once

#include "voxel.h"
#include "renderer.h"

typedef struct {
    float x, y, z, w;
    float r, g, b, a;
    float nx, ny, nz, ao;
    float u, v, texture_layer, p2;
} Vertex;

typedef struct {
    Vertex   *vertices;
    uint32_t  vertex_count;
    uint32_t  vertex_capacity;
    R_VAO     vao;
    R_Buffer  vbo;
    R_Buffer  ebo;
    R_Buffer  indirect_draw_buffer;
    R_Buffer  atomic_counter_buffer;
} Mesh;

#define LOD_LEVELS 3

void mesh_init(Mesh *mesh);
void mesh_generate_greedy(Mesh *mesh, const Chunk *chunk);
void mesh_generate_lod(Mesh *mesh, const Chunk *chunk, int step);
void mesh_generate_gpu(Mesh *mesh, R_Program compute_program, R_Texture voxel_tex, int chunk_x, int chunk_z);
void mesh_upload(Mesh *mesh);
void mesh_prepare_gpu(Mesh *mesh);
void mesh_update_draw_count(Mesh *mesh);
void mesh_free(Mesh *mesh);
