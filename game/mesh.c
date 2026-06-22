#include "mesh.h"
#include "components.h"
#include <stdlib.h>

void mesh_init(Mesh *mesh) {
    mesh->vertex_count    = 0;
    mesh->vertex_capacity = 4096;
    mesh->vertices        = malloc(sizeof(Vertex) * mesh->vertex_capacity);
    mesh->vao             = R_INVALID_HANDLE;
    mesh->vbo             = R_INVALID_HANDLE;
    mesh->ebo             = R_INVALID_HANDLE;
    mesh->indirect_draw_buffer   = R_INVALID_HANDLE;
    mesh->atomic_counter_buffer  = R_INVALID_HANDLE;
    /* VBO intentionally left INVALID here so mesh_upload creates it with the
       right size and memory type.  mesh_prepare_gpu creates its own VBO for
       the compute path. */
}

#define EPSILON 0.002f

static void add_vertex(Mesh *mesh, float x, float y, float z,
                       float r, float g, float b,
                       float nx, float ny, float nz, float ao,
                       float u, float v, float tex_layer) {
    if (mesh->vertex_count >= mesh->vertex_capacity) {
        mesh->vertex_capacity *= 2;
        Vertex *nv = realloc(mesh->vertices, sizeof(Vertex) * mesh->vertex_capacity);
        if (!nv) return;
        mesh->vertices = nv;
    }
    x += (nx != 0.0f) ? nx * EPSILON : 0.0f;
    y += (ny != 0.0f) ? ny * EPSILON : 0.0f;
    z += (nz != 0.0f) ? nz * EPSILON : 0.0f;
    mesh->vertices[mesh->vertex_count++] = (Vertex){
        .x = x, .y = y, .z = z, .w = 1.0f,
        .r = r, .g = g, .b = b, .a = 1.0f,
        .nx = nx, .ny = ny, .nz = nz, .ao = ao,
        .u = u, .v = v, .texture_layer = tex_layer, .p2 = 0,
    };
}

static void block_vertex_color(BlockType type, float *r, float *g, float *b) {
    if (type == BLOCK_WATER) { *r = 0.15f; *g = 0.50f; *b = 1.00f; }
    else                     { *r = 1.0f;  *g = 1.0f;  *b = 1.0f;  }
}

static int get_texture_layer(BlockType type, int face) {
    entity_t e = g_block_entities[type];
    C_BlockDef *def = entity_get_component(g_ecs, e, COMP_BLOCK_DEF);
    if (!def) return 0;
    switch (face) {
    case 4:  return def->layer_top    >= 0 ? def->layer_top    : def->layer_default;
    case 5:  return def->layer_bottom >= 0 ? def->layer_bottom : def->layer_default;
    default: return def->layer_side   >= 0 ? def->layer_side   : def->layer_default;
    }
}

static bool is_opaque(BlockType type) {
    entity_t e = g_block_entities[type];
    C_BlockDef *def = entity_get_component(g_ecs, e, COMP_BLOCK_DEF);
    return def && def->opaque;
}

static bool is_transparent(const Chunk *chunk, int x, int y, int z) {
    if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE)
        return true;
    return !is_opaque(chunk->blocks[x][y][z]);
}

static float vertex_ao(bool s1, bool s2, bool corner) {
    if (s1 && s2) return 0.5f;
    int c = (s1 ? 1 : 0) + (s2 ? 1 : 0) + (corner ? 1 : 0);
    if (c == 0) return 1.0f;
    if (c == 1) return 0.85f;
    if (c == 2) return 0.7f;
    return 0.5f;
}

static void add_face(Mesh *mesh, const Chunk *chunk, int x, int y, int z, int face, BlockType type) {
    float ox = (float)(x + chunk->x * CHUNK_SIZE);
    float oy = (float)y;
    float oz = (float)(z + chunk->z * CHUNK_SIZE);
    int   layer = get_texture_layer(type, face);
    float pad = 0.001f;
    float u0 = pad, v0 = pad, u1 = 1.0f - pad, v1 = 1.0f - pad;

    float p[4][3];
    float nx = 0, ny = 0, nz = 0;
    if      (face == 0) { nz= 1; p[0][0]=ox;   p[0][1]=oy;   p[0][2]=oz+1; p[1][0]=ox+1; p[1][1]=oy;   p[1][2]=oz+1; p[2][0]=ox+1; p[2][1]=oy+1; p[2][2]=oz+1; p[3][0]=ox;   p[3][1]=oy+1; p[3][2]=oz+1; }
    else if (face == 1) { nz=-1; p[0][0]=ox+1; p[0][1]=oy;   p[0][2]=oz;   p[1][0]=ox;   p[1][1]=oy;   p[1][2]=oz;   p[2][0]=ox;   p[2][1]=oy+1; p[2][2]=oz;   p[3][0]=ox+1; p[3][1]=oy+1; p[3][2]=oz; }
    else if (face == 2) { nx=-1; p[0][0]=ox;   p[0][1]=oy;   p[0][2]=oz;   p[1][0]=ox;   p[1][1]=oy;   p[1][2]=oz+1; p[2][0]=ox;   p[2][1]=oy+1; p[2][2]=oz+1; p[3][0]=ox;   p[3][1]=oy+1; p[3][2]=oz; }
    else if (face == 3) { nx= 1; p[0][0]=ox+1; p[0][1]=oy;   p[0][2]=oz+1; p[1][0]=ox+1; p[1][1]=oy;   p[1][2]=oz;   p[2][0]=ox+1; p[2][1]=oy+1; p[2][2]=oz;   p[3][0]=ox+1; p[3][1]=oy+1; p[3][2]=oz+1; }
    else if (face == 4) { ny= 1; p[0][0]=ox;   p[0][1]=oy+1; p[0][2]=oz+1; p[1][0]=ox+1; p[1][1]=oy+1; p[1][2]=oz+1; p[2][0]=ox+1; p[2][1]=oy+1; p[2][2]=oz;   p[3][0]=ox;   p[3][1]=oy+1; p[3][2]=oz; }
    else                { ny=-1; p[0][0]=ox;   p[0][1]=oy;   p[0][2]=oz;   p[1][0]=ox+1; p[1][1]=oy;   p[1][2]=oz;   p[2][0]=ox+1; p[2][1]=oy;   p[2][2]=oz+1; p[3][0]=ox;   p[3][1]=oy;   p[3][2]=oz+1; }

    float ao[4];
    if (face == 0) {
        ao[0]=vertex_ao(!is_transparent(chunk,x,y-1,z+1),!is_transparent(chunk,x-1,y,z+1),!is_transparent(chunk,x-1,y-1,z+1));
        ao[1]=vertex_ao(!is_transparent(chunk,x+1,y-1,z+1),!is_transparent(chunk,x+2,y,z+1),!is_transparent(chunk,x+2,y-1,z+1));
        ao[2]=vertex_ao(!is_transparent(chunk,x+1,y+2,z+1),!is_transparent(chunk,x+2,y+1,z+1),!is_transparent(chunk,x+2,y+2,z+1));
        ao[3]=vertex_ao(!is_transparent(chunk,x,y+2,z+1),!is_transparent(chunk,x-1,y+1,z+1),!is_transparent(chunk,x-1,y+2,z+1));
    } else if (face == 1) {
        ao[0]=vertex_ao(!is_transparent(chunk,x+1,y-1,z),!is_transparent(chunk,x+2,y,z),!is_transparent(chunk,x+2,y-1,z));
        ao[1]=vertex_ao(!is_transparent(chunk,x,y-1,z),!is_transparent(chunk,x-1,y,z),!is_transparent(chunk,x-1,y-1,z));
        ao[2]=vertex_ao(!is_transparent(chunk,x,y+2,z),!is_transparent(chunk,x-1,y+1,z),!is_transparent(chunk,x-1,y+2,z));
        ao[3]=vertex_ao(!is_transparent(chunk,x+1,y+2,z),!is_transparent(chunk,x+2,y+1,z),!is_transparent(chunk,x+2,y+2,z));
    } else if (face == 2) {
        ao[0]=vertex_ao(!is_transparent(chunk,x,y-1,z),!is_transparent(chunk,x,y,z-1),!is_transparent(chunk,x,y-1,z-1));
        ao[1]=vertex_ao(!is_transparent(chunk,x,y-1,z+1),!is_transparent(chunk,x,y,z+2),!is_transparent(chunk,x,y-1,z+2));
        ao[2]=vertex_ao(!is_transparent(chunk,x,y+2,z+1),!is_transparent(chunk,x,y+1,z+2),!is_transparent(chunk,x,y+2,z+2));
        ao[3]=vertex_ao(!is_transparent(chunk,x,y+2,z),!is_transparent(chunk,x,y+1,z-1),!is_transparent(chunk,x,y+2,z-1));
    } else if (face == 3) {
        ao[0]=vertex_ao(!is_transparent(chunk,x+1,y-1,z+1),!is_transparent(chunk,x+1,y,z+2),!is_transparent(chunk,x+1,y-1,z+2));
        ao[1]=vertex_ao(!is_transparent(chunk,x+1,y-1,z),!is_transparent(chunk,x+1,y,z-1),!is_transparent(chunk,x+1,y-1,z-1));
        ao[2]=vertex_ao(!is_transparent(chunk,x+1,y+2,z),!is_transparent(chunk,x+1,y+1,z-1),!is_transparent(chunk,x+1,y+2,z-1));
        ao[3]=vertex_ao(!is_transparent(chunk,x+1,y+2,z+1),!is_transparent(chunk,x+1,y+1,z+2),!is_transparent(chunk,x+1,y+2,z+2));
    } else if (face == 4) {
        ao[0]=vertex_ao(!is_transparent(chunk,x-1,y+1,z+1),!is_transparent(chunk,x,y+1,z+2),!is_transparent(chunk,x-1,y+1,z+2));
        ao[1]=vertex_ao(!is_transparent(chunk,x+2,y+1,z+1),!is_transparent(chunk,x+1,y+1,z+2),!is_transparent(chunk,x+2,y+1,z+2));
        ao[2]=vertex_ao(!is_transparent(chunk,x+2,y+1,z),!is_transparent(chunk,x+1,y+1,z-1),!is_transparent(chunk,x+2,y+1,z-1));
        ao[3]=vertex_ao(!is_transparent(chunk,x-1,y+1,z),!is_transparent(chunk,x,y+1,z-1),!is_transparent(chunk,x-1,y+1,z-1));
    } else {
        ao[0]=vertex_ao(!is_transparent(chunk,x-1,y,z),!is_transparent(chunk,x,y,z-1),!is_transparent(chunk,x-1,y,z-1));
        ao[1]=vertex_ao(!is_transparent(chunk,x+2,y,z),!is_transparent(chunk,x+1,y,z-1),!is_transparent(chunk,x+2,y,z-1));
        ao[2]=vertex_ao(!is_transparent(chunk,x+2,y,z+1),!is_transparent(chunk,x+1,y,z+2),!is_transparent(chunk,x+2,y,z+2));
        ao[3]=vertex_ao(!is_transparent(chunk,x-1,y,z+1),!is_transparent(chunk,x,y,z+2),!is_transparent(chunk,x-1,y,z+2));
    }

    float cr, cg, cb;
    block_vertex_color(type, &cr, &cg, &cb);
    add_vertex(mesh,p[0][0],p[0][1],p[0][2],cr,cg,cb,nx,ny,nz,ao[0],u0,v0,(float)layer);
    add_vertex(mesh,p[1][0],p[1][1],p[1][2],cr,cg,cb,nx,ny,nz,ao[1],u1,v0,(float)layer);
    add_vertex(mesh,p[2][0],p[2][1],p[2][2],cr,cg,cb,nx,ny,nz,ao[2],u1,v1,(float)layer);
    add_vertex(mesh,p[0][0],p[0][1],p[0][2],cr,cg,cb,nx,ny,nz,ao[0],u0,v0,(float)layer);
    add_vertex(mesh,p[2][0],p[2][1],p[2][2],cr,cg,cb,nx,ny,nz,ao[2],u1,v1,(float)layer);
    add_vertex(mesh,p[3][0],p[3][1],p[3][2],cr,cg,cb,nx,ny,nz,ao[3],u0,v1,(float)layer);
}

static BlockType dominant_block(const Chunk *chunk, int x, int y, int z, int step) {
    for (int dx = 0; dx < step; dx++)
        for (int dy = 0; dy < step; dy++)
            for (int dz = 0; dz < step; dz++) {
                int nx = x + dx, ny = y + dy, nz = z + dz;
                if (nx >= 0 && nx < CHUNK_SIZE && ny >= 0 && ny < CHUNK_SIZE && nz >= 0 && nz < CHUNK_SIZE)
                    if (chunk->blocks[nx][ny][nz] != BLOCK_AIR)
                        return chunk->blocks[nx][ny][nz];
            }
    return BLOCK_AIR;
}

static bool lod_cell_solid(const Chunk *chunk, int x, int y, int z, int step) {
    return dominant_block(chunk, x, y, z, step) != BLOCK_AIR;
}

static void add_face_lod(Mesh *mesh, const Chunk *chunk, int x, int y, int z, int face, BlockType type, int step) {
    float ox = (float)(x + chunk->x * CHUNK_SIZE);
    float oy = (float)y;
    float oz = (float)(z + chunk->z * CHUNK_SIZE);
    float s  = (float)step;
    int   layer = get_texture_layer(type, face);
    float pad = 0.001f;
    float u0 = pad, v0 = pad, u1 = 1.0f - pad, v1 = 1.0f - pad;

    float p[4][3];
    float nx = 0, ny = 0, nz = 0;
    if      (face == 0) { nz= 1; p[0][0]=ox;   p[0][1]=oy;   p[0][2]=oz+s; p[1][0]=ox+s; p[1][1]=oy;   p[1][2]=oz+s; p[2][0]=ox+s; p[2][1]=oy+s; p[2][2]=oz+s; p[3][0]=ox;   p[3][1]=oy+s; p[3][2]=oz+s; }
    else if (face == 1) { nz=-1; p[0][0]=ox+s; p[0][1]=oy;   p[0][2]=oz;   p[1][0]=ox;   p[1][1]=oy;   p[1][2]=oz;   p[2][0]=ox;   p[2][1]=oy+s; p[2][2]=oz;   p[3][0]=ox+s; p[3][1]=oy+s; p[3][2]=oz; }
    else if (face == 2) { nx=-1; p[0][0]=ox;   p[0][1]=oy;   p[0][2]=oz;   p[1][0]=ox;   p[1][1]=oy;   p[1][2]=oz+s; p[2][0]=ox;   p[2][1]=oy+s; p[2][2]=oz+s; p[3][0]=ox;   p[3][1]=oy+s; p[3][2]=oz; }
    else if (face == 3) { nx= 1; p[0][0]=ox+s; p[0][1]=oy;   p[0][2]=oz+s; p[1][0]=ox+s; p[1][1]=oy;   p[1][2]=oz;   p[2][0]=ox+s; p[2][1]=oy+s; p[2][2]=oz;   p[3][0]=ox+s; p[3][1]=oy+s; p[3][2]=oz+s; }
    else if (face == 4) { ny= 1; p[0][0]=ox;   p[0][1]=oy+s; p[0][2]=oz+s; p[1][0]=ox+s; p[1][1]=oy+s; p[1][2]=oz+s; p[2][0]=ox+s; p[2][1]=oy+s; p[2][2]=oz;   p[3][0]=ox;   p[3][1]=oy+s; p[3][2]=oz; }
    else                { ny=-1; p[0][0]=ox;   p[0][1]=oy;   p[0][2]=oz;   p[1][0]=ox+s; p[1][1]=oy;   p[1][2]=oz;   p[2][0]=ox+s; p[2][1]=oy;   p[2][2]=oz+s; p[3][0]=ox;   p[3][1]=oy;   p[3][2]=oz+s; }

    float ao = 1.0f;
    float cr, cg, cb;
    block_vertex_color(type, &cr, &cg, &cb);
    add_vertex(mesh, p[0][0],p[0][1],p[0][2], cr,cg,cb, nx,ny,nz, ao, u0,v0, (float)layer);
    add_vertex(mesh, p[1][0],p[1][1],p[1][2], cr,cg,cb, nx,ny,nz, ao, u1,v0, (float)layer);
    add_vertex(mesh, p[2][0],p[2][1],p[2][2], cr,cg,cb, nx,ny,nz, ao, u1,v1, (float)layer);
    add_vertex(mesh, p[0][0],p[0][1],p[0][2], cr,cg,cb, nx,ny,nz, ao, u0,v0, (float)layer);
    add_vertex(mesh, p[2][0],p[2][1],p[2][2], cr,cg,cb, nx,ny,nz, ao, u1,v1, (float)layer);
    add_vertex(mesh, p[3][0],p[3][1],p[3][2], cr,cg,cb, nx,ny,nz, ao, u0,v1, (float)layer);
}

void mesh_generate_lod(Mesh *mesh, const Chunk *chunk, int step) {
    if (step <= 1) { mesh_generate_greedy(mesh, chunk); return; }
    mesh->vertex_count = 0;
    for (int x = 0; x < CHUNK_SIZE; x += step) {
        for (int y = 0; y < CHUNK_SIZE; y += step) {
            for (int z = 0; z < CHUNK_SIZE; z += step) {
                BlockType t = dominant_block(chunk, x, y, z, step);
                if (t == BLOCK_AIR) continue;
                if (!lod_cell_solid(chunk, x,      y,      z+step, step)) add_face_lod(mesh, chunk, x, y, z, 0, t, step);
                if (!lod_cell_solid(chunk, x,      y,      z-step, step)) add_face_lod(mesh, chunk, x, y, z, 1, t, step);
                if (!lod_cell_solid(chunk, x-step, y,      z,      step)) add_face_lod(mesh, chunk, x, y, z, 2, t, step);
                if (!lod_cell_solid(chunk, x+step, y,      z,      step)) add_face_lod(mesh, chunk, x, y, z, 3, t, step);
                if (!lod_cell_solid(chunk, x,      y+step, z,      step)) add_face_lod(mesh, chunk, x, y, z, 4, t, step);
                if (!lod_cell_solid(chunk, x,      y-step, z,      step)) add_face_lod(mesh, chunk, x, y, z, 5, t, step);
            }
        }
    }
}

void mesh_generate_gpu(Mesh *mesh, R_Program compute_program, R_Texture voxel_tex, int chunk_x, int chunk_z) {
    renderer_use_program(compute_program);
    renderer_bind_image_texture(0, voxel_tex, R_ACCESS_READ_ONLY);
    renderer_bind_buffer_base(R_BUF_SHADER_STORAGE, 1, mesh->vbo);
    renderer_bind_buffer_base(R_BUF_ATOMIC_COUNTER, 0, mesh->atomic_counter_buffer);
    uint32_t zero = 0;
    renderer_buffer_sub_data(R_BUF_ATOMIC_COUNTER, 0, sizeof(uint32_t), &zero);
    int loc = renderer_uniform_location(compute_program, "uChunkPos");
    renderer_uniform_ivec2(loc, chunk_x, chunk_z);
    renderer_dispatch_compute(4, 4, 4);
    renderer_memory_barrier(R_BARRIER_ALL);
    mesh_update_draw_count(mesh);
}

void mesh_generate_greedy(Mesh *mesh, const Chunk *chunk) {
    mesh->vertex_count = 0;
    for (int x = 0; x < CHUNK_SIZE; x++) {
        for (int y = 0; y < CHUNK_SIZE; y++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                if (chunk->blocks[x][y][z] == BLOCK_AIR) continue;
                BlockType t = chunk->blocks[x][y][z];
                if (is_transparent(chunk, x, y, z + 1)) add_face(mesh, chunk, x, y, z, 0, t);
                if (is_transparent(chunk, x, y, z - 1)) add_face(mesh, chunk, x, y, z, 1, t);
                if (is_transparent(chunk, x - 1, y, z)) add_face(mesh, chunk, x, y, z, 2, t);
                if (is_transparent(chunk, x + 1, y, z)) add_face(mesh, chunk, x, y, z, 3, t);
                if (is_transparent(chunk, x, y + 1, z)) add_face(mesh, chunk, x, y, z, 4, t);
                if (is_transparent(chunk, x, y - 1, z)) add_face(mesh, chunk, x, y, z, 5, t);
            }
        }
    }
}

static void mesh_setup_attribs(void) {
    renderer_attrib_pointer(0, 3, R_TYPE_FLOAT, false, sizeof(Vertex), 0);
    renderer_enable_attrib(0);
    renderer_attrib_pointer(1, 3, R_TYPE_FLOAT, false, sizeof(Vertex), 4 * sizeof(float));
    renderer_enable_attrib(1);
    renderer_attrib_pointer(2, 3, R_TYPE_FLOAT, false, sizeof(Vertex), 8 * sizeof(float));
    renderer_enable_attrib(2);
    renderer_attrib_pointer(3, 1, R_TYPE_FLOAT, false, sizeof(Vertex), 11 * sizeof(float));
    renderer_enable_attrib(3);
    renderer_attrib_pointer(4, 2, R_TYPE_FLOAT, false, sizeof(Vertex), 12 * sizeof(float));
    renderer_enable_attrib(4);
    renderer_attrib_pointer(5, 1, R_TYPE_FLOAT, false, sizeof(Vertex), 14 * sizeof(float));
    renderer_enable_attrib(5);
}

void mesh_upload(Mesh *mesh) {
    if (mesh->vertex_count == 0) return; /* nothing to upload; draw caller skips 0-count meshes */
    if (mesh->vao == R_INVALID_HANDLE) mesh->vao = renderer_create_vao();
    if (mesh->vbo == R_INVALID_HANDLE) mesh->vbo = renderer_create_buffer();
    renderer_bind_vao(mesh->vao);
    renderer_bind_buffer(R_BUF_ARRAY, mesh->vbo);
    renderer_buffer_data(R_BUF_ARRAY, mesh->vertex_count * sizeof(Vertex), mesh->vertices, R_USAGE_DYNAMIC);
    mesh_setup_attribs();
    renderer_bind_buffer(R_BUF_ARRAY, R_INVALID_HANDLE);
    renderer_bind_vao(R_INVALID_HANDLE);
}

void mesh_prepare_gpu(Mesh *mesh) {
    if (mesh->vao == R_INVALID_HANDLE) mesh->vao = renderer_create_vao();
    renderer_bind_vao(mesh->vao);
    renderer_bind_buffer(R_BUF_ARRAY, mesh->vbo);
    mesh_setup_attribs();
    renderer_bind_buffer(R_BUF_ARRAY, R_INVALID_HANDLE);
    renderer_bind_vao(R_INVALID_HANDLE);

    mesh->indirect_draw_buffer = renderer_create_buffer();
    renderer_bind_buffer(R_BUF_DRAW_INDIRECT, mesh->indirect_draw_buffer);
    uint32_t zero_cmd[4] = {0, 1, 0, 0};
    renderer_buffer_data(R_BUF_DRAW_INDIRECT, sizeof(zero_cmd), zero_cmd, R_USAGE_DYNAMIC);
    renderer_bind_buffer(R_BUF_DRAW_INDIRECT, R_INVALID_HANDLE);

    mesh->atomic_counter_buffer = renderer_create_buffer();
    renderer_bind_buffer(R_BUF_ATOMIC_COUNTER, mesh->atomic_counter_buffer);
    uint32_t zero = 0;
    renderer_buffer_data(R_BUF_ATOMIC_COUNTER, sizeof(uint32_t), &zero, R_USAGE_DYNAMIC);
    renderer_bind_buffer(R_BUF_ATOMIC_COUNTER, R_INVALID_HANDLE);
}

void mesh_update_draw_count(Mesh *mesh) {
    uint32_t count = 0;
    renderer_bind_buffer(R_BUF_ATOMIC_COUNTER, mesh->atomic_counter_buffer);
    renderer_get_buffer_sub_data(R_BUF_ATOMIC_COUNTER, 0, sizeof(uint32_t), &count);
    renderer_bind_buffer(R_BUF_ATOMIC_COUNTER, R_INVALID_HANDLE);
    uint32_t cmd[4] = {count, 1, 0, 0};
    renderer_bind_buffer(R_BUF_DRAW_INDIRECT, mesh->indirect_draw_buffer);
    renderer_buffer_sub_data(R_BUF_DRAW_INDIRECT, 0, sizeof(cmd), cmd);
    renderer_bind_buffer(R_BUF_DRAW_INDIRECT, R_INVALID_HANDLE);
}

void mesh_free(Mesh *mesh) {
    free(mesh->vertices);
    mesh->vertices = NULL;
    if (mesh->indirect_draw_buffer != R_INVALID_HANDLE)  { renderer_destroy_buffer(mesh->indirect_draw_buffer);  mesh->indirect_draw_buffer  = R_INVALID_HANDLE; }
    if (mesh->atomic_counter_buffer != R_INVALID_HANDLE) { renderer_destroy_buffer(mesh->atomic_counter_buffer); mesh->atomic_counter_buffer = R_INVALID_HANDLE; }
    if (mesh->vao != R_INVALID_HANDLE) { renderer_destroy_vao(mesh->vao);    mesh->vao = R_INVALID_HANDLE; }
    if (mesh->vbo != R_INVALID_HANDLE) { renderer_destroy_buffer(mesh->vbo); mesh->vbo = R_INVALID_HANDLE; }
}
