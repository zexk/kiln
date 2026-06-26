#include "kv_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define INITIAL_VERTS 4096
#define FACE_EPSILON  0.002f

void kv_mesh_init(KvMesh *m) {
    m->count = 0;
    m->cap   = INITIAL_VERTS;
    m->verts = malloc(sizeof(KvVertex) * m->cap);
    m->vao   = R_INVALID_HANDLE;
    m->vbo   = R_INVALID_HANDLE;
}

static void push_vertex(KvMesh *m,
                        float x, float y, float z,
                        float r, float g, float b,
                        float nx, float ny, float nz, float ao,
                        float u, float v, float layer) {
    if (m->count >= m->cap) {
        m->cap  *= 2;
        KvVertex *nv = realloc(m->verts, sizeof(KvVertex) * m->cap);
        if (!nv) return;
        m->verts = nv;
    }
    x += nx * FACE_EPSILON;
    y += ny * FACE_EPSILON;
    z += nz * FACE_EPSILON;
    m->verts[m->count++] = (KvVertex){
        .x=x, .y=y, .z=z, ._w=1.0f,
        .r=r, .g=g, .b=b, ._a=1.0f,
        .nx=nx, .ny=ny, .nz=nz, .ao=ao,
        .u=u, .v=v, .lay=layer, ._p=0.0f,
    };
}

static bool local_transparent(uint16_t blk[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE],
                               int x, int y, int z) {
    if (x < 0 || x >= KV_CHUNK_SIZE || y < 0 || y >= KV_CHUNK_SIZE || z < 0 || z >= KV_CHUNK_SIZE)
        return true;
    return !kv_block_opaque(blk[x][y][z]);
}

static float vertex_ao(bool s1, bool s2, bool corner) {
    if (s1 && s2) return 0.5f;
    int c = (s1?1:0) + (s2?1:0) + (corner?1:0);
    if (c == 0) return 1.0f;
    if (c == 1) return 0.85f;
    if (c == 2) return 0.7f;
    return 0.5f;
}

#define S(x,y,z) (!local_transparent(blk,(x),(y),(z)))

static void add_face(KvMesh *m,
                     uint16_t blk[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE],
                     int lx, int ly, int lz, int face, uint16_t type,
                     float ox, float oy, float oz) {
    float p[4][3];
    float nx = 0, ny = 0, nz = 0;
    float pad = 0.001f, u0=pad, v0=pad, u1=1.0f-pad, v1=1.0f-pad;

    switch (face) {
    case 0: nz= 1; p[0][0]=ox;  p[0][1]=oy;  p[0][2]=oz+1; p[1][0]=ox+1;p[1][1]=oy;  p[1][2]=oz+1; p[2][0]=ox+1;p[2][1]=oy+1;p[2][2]=oz+1; p[3][0]=ox;  p[3][1]=oy+1;p[3][2]=oz+1; break;
    case 1: nz=-1; p[0][0]=ox+1;p[0][1]=oy;  p[0][2]=oz;   p[1][0]=ox;  p[1][1]=oy;  p[1][2]=oz;   p[2][0]=ox;  p[2][1]=oy+1;p[2][2]=oz;   p[3][0]=ox+1;p[3][1]=oy+1;p[3][2]=oz;   break;
    case 2: nx=-1; p[0][0]=ox;  p[0][1]=oy;  p[0][2]=oz;   p[1][0]=ox;  p[1][1]=oy;  p[1][2]=oz+1; p[2][0]=ox;  p[2][1]=oy+1;p[2][2]=oz+1; p[3][0]=ox;  p[3][1]=oy+1;p[3][2]=oz;   break;
    case 3: nx= 1; p[0][0]=ox+1;p[0][1]=oy;  p[0][2]=oz+1; p[1][0]=ox+1;p[1][1]=oy;  p[1][2]=oz;   p[2][0]=ox+1;p[2][1]=oy+1;p[2][2]=oz;   p[3][0]=ox+1;p[3][1]=oy+1;p[3][2]=oz+1; break;
    case 4: ny= 1; p[0][0]=ox;  p[0][1]=oy+1;p[0][2]=oz+1; p[1][0]=ox+1;p[1][1]=oy+1;p[1][2]=oz+1; p[2][0]=ox+1;p[2][1]=oy+1;p[2][2]=oz;   p[3][0]=ox;  p[3][1]=oy+1;p[3][2]=oz;   break;
    default:ny=-1; p[0][0]=ox;  p[0][1]=oy;  p[0][2]=oz;   p[1][0]=ox+1;p[1][1]=oy;  p[1][2]=oz;   p[2][0]=ox+1;p[2][1]=oy;  p[2][2]=oz+1; p[3][0]=ox;  p[3][1]=oy;  p[3][2]=oz+1; break;
    }

    float ao[4];
    switch (face) {
    case 0:
        ao[0]=vertex_ao(S(lx,ly-1,lz+1),S(lx-1,ly,lz+1),S(lx-1,ly-1,lz+1));
        ao[1]=vertex_ao(S(lx+1,ly-1,lz+1),S(lx+2,ly,lz+1),S(lx+2,ly-1,lz+1));
        ao[2]=vertex_ao(S(lx+1,ly+2,lz+1),S(lx+2,ly+1,lz+1),S(lx+2,ly+2,lz+1));
        ao[3]=vertex_ao(S(lx,ly+2,lz+1),S(lx-1,ly+1,lz+1),S(lx-1,ly+2,lz+1));
        break;
    case 1:
        ao[0]=vertex_ao(S(lx+1,ly-1,lz),S(lx+2,ly,lz),S(lx+2,ly-1,lz));
        ao[1]=vertex_ao(S(lx,ly-1,lz),S(lx-1,ly,lz),S(lx-1,ly-1,lz));
        ao[2]=vertex_ao(S(lx,ly+2,lz),S(lx-1,ly+1,lz),S(lx-1,ly+2,lz));
        ao[3]=vertex_ao(S(lx+1,ly+2,lz),S(lx+2,ly+1,lz),S(lx+2,ly+2,lz));
        break;
    case 2:
        ao[0]=vertex_ao(S(lx,ly-1,lz),S(lx,ly,lz-1),S(lx,ly-1,lz-1));
        ao[1]=vertex_ao(S(lx,ly-1,lz+1),S(lx,ly,lz+2),S(lx,ly-1,lz+2));
        ao[2]=vertex_ao(S(lx,ly+2,lz+1),S(lx,ly+1,lz+2),S(lx,ly+2,lz+2));
        ao[3]=vertex_ao(S(lx,ly+2,lz),S(lx,ly+1,lz-1),S(lx,ly+2,lz-1));
        break;
    case 3:
        ao[0]=vertex_ao(S(lx+1,ly-1,lz+1),S(lx+1,ly,lz+2),S(lx+1,ly-1,lz+2));
        ao[1]=vertex_ao(S(lx+1,ly-1,lz),S(lx+1,ly,lz-1),S(lx+1,ly-1,lz-1));
        ao[2]=vertex_ao(S(lx+1,ly+2,lz),S(lx+1,ly+1,lz-1),S(lx+1,ly+2,lz-1));
        ao[3]=vertex_ao(S(lx+1,ly+2,lz+1),S(lx+1,ly+1,lz+2),S(lx+1,ly+2,lz+2));
        break;
    case 4:
        ao[0]=vertex_ao(S(lx-1,ly+1,lz+1),S(lx,ly+1,lz+2),S(lx-1,ly+1,lz+2));
        ao[1]=vertex_ao(S(lx+2,ly+1,lz+1),S(lx+1,ly+1,lz+2),S(lx+2,ly+1,lz+2));
        ao[2]=vertex_ao(S(lx+2,ly+1,lz),S(lx+1,ly+1,lz-1),S(lx+2,ly+1,lz-1));
        ao[3]=vertex_ao(S(lx-1,ly+1,lz),S(lx,ly+1,lz-1),S(lx-1,ly+1,lz-1));
        break;
    default:
        ao[0]=vertex_ao(S(lx-1,ly,lz),S(lx,ly,lz-1),S(lx-1,ly,lz-1));
        ao[1]=vertex_ao(S(lx+2,ly,lz),S(lx+1,ly,lz-1),S(lx+2,ly,lz-1));
        ao[2]=vertex_ao(S(lx+2,ly,lz+1),S(lx+1,ly,lz+2),S(lx+2,ly,lz+2));
        ao[3]=vertex_ao(S(lx-1,ly,lz+1),S(lx,ly,lz+2),S(lx-1,ly,lz+2));
        break;
    }

    float r, g, b;
    kv_block_tint(type, &r, &g, &b);
    float layer = (float)kv_tex_layer(type, face);

    push_vertex(m, p[0][0],p[0][1],p[0][2], r,g,b, nx,ny,nz, ao[0], u0,v0, layer);
    push_vertex(m, p[1][0],p[1][1],p[1][2], r,g,b, nx,ny,nz, ao[1], u1,v0, layer);
    push_vertex(m, p[2][0],p[2][1],p[2][2], r,g,b, nx,ny,nz, ao[2], u1,v1, layer);
    push_vertex(m, p[0][0],p[0][1],p[0][2], r,g,b, nx,ny,nz, ao[0], u0,v0, layer);
    push_vertex(m, p[2][0],p[2][1],p[2][2], r,g,b, nx,ny,nz, ao[2], u1,v1, layer);
    push_vertex(m, p[3][0],p[3][1],p[3][2], r,g,b, nx,ny,nz, ao[3], u0,v1, layer);
}

#undef S

void kv_mesh_generate(KvMesh *m,
                      uint16_t blk[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE],
                      int32_t cx, int32_t cy, int32_t cz) {
    m->count = 0;
    for (int lx = 0; lx < KV_CHUNK_SIZE; lx++) {
        for (int ly = 0; ly < KV_CHUNK_SIZE; ly++) {
            for (int lz = 0; lz < KV_CHUNK_SIZE; lz++) {
                uint16_t t = blk[lx][ly][lz];
                if (t == KV_BLOCK_AIR) continue;
                float ox = (float)(cx * KV_CHUNK_SIZE + lx);
                float oy = (float)(cy * KV_CHUNK_SIZE + ly);
                float oz = (float)(cz * KV_CHUNK_SIZE + lz);
                if (local_transparent(blk,lx,ly,lz+1)) add_face(m,blk,lx,ly,lz,0,t,ox,oy,oz);
                if (local_transparent(blk,lx,ly,lz-1)) add_face(m,blk,lx,ly,lz,1,t,ox,oy,oz);
                if (local_transparent(blk,lx-1,ly,lz)) add_face(m,blk,lx,ly,lz,2,t,ox,oy,oz);
                if (local_transparent(blk,lx+1,ly,lz)) add_face(m,blk,lx,ly,lz,3,t,ox,oy,oz);
                if (local_transparent(blk,lx,ly+1,lz)) add_face(m,blk,lx,ly,lz,4,t,ox,oy,oz);
                if (local_transparent(blk,lx,ly-1,lz)) add_face(m,blk,lx,ly,lz,5,t,ox,oy,oz);
            }
        }
    }
}

static uint16_t dominant_block(uint16_t blk[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE],
                                int x, int y, int z, int step) {
    for (int dx = 0; dx < step; dx++)
        for (int dy = step-1; dy >= 0; dy--)
            for (int dz = 0; dz < step; dz++) {
                int nx2 = x+dx, ny2 = y+dy, nz2 = z+dz;
                if (nx2>=0&&nx2<KV_CHUNK_SIZE&&ny2>=0&&ny2<KV_CHUNK_SIZE&&nz2>=0&&nz2<KV_CHUNK_SIZE)
                    if (blk[nx2][ny2][nz2] != KV_BLOCK_AIR)
                        return blk[nx2][ny2][nz2];
            }
    return KV_BLOCK_AIR;
}

static bool lod_solid_overlap(uint16_t blk[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE],
                              int x, int y, int z, int step,
                              int y_min, int y_max) {
    for (int dy = 0; dy < step; dy++) {
        int ny = y + dy;
        if (ny < y_min || ny > y_max) continue;
        for (int dx = 0; dx < step; dx++)
            for (int dz = 0; dz < step; dz++) {
                int nx = x+dx, nz = z+dz;
                if (nx>=0&&nx<KV_CHUNK_SIZE&&ny>=0&&ny<KV_CHUNK_SIZE&&nz>=0&&nz<KV_CHUNK_SIZE)
                    if (blk[nx][ny][nz] != KV_BLOCK_AIR && kv_block_opaque(blk[nx][ny][nz]))
                        return true;
            }
    }
    return false;
}

/* Highest non-air block local-y within a step^3 super-block. */
static int super_top_y(uint16_t blk[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE],
                       int x, int y, int z, int step) {
    for (int dy = step-1; dy >= 0; dy--)
        for (int dx = 0; dx < step; dx++)
            for (int dz = 0; dz < step; dz++) {
                int nx=x+dx, ny=y+dy, nz=z+dz;
                if (nx>=0&&nx<KV_CHUNK_SIZE&&ny>=0&&ny<KV_CHUNK_SIZE&&nz>=0&&nz<KV_CHUNK_SIZE)
                    if (blk[nx][ny][nz] != KV_BLOCK_AIR) return ny;
            }
    return y;
}

/* Lowest non-air block local-y within a step^3 super-block. */
static int super_bot_y(uint16_t blk[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE],
                       int x, int y, int z, int step) {
    for (int dy = 0; dy < step; dy++)
        for (int dx = 0; dx < step; dx++)
            for (int dz = 0; dz < step; dz++) {
                int nx=x+dx, ny=y+dy, nz=z+dz;
                if (nx>=0&&nx<KV_CHUNK_SIZE&&ny>=0&&ny<KV_CHUNK_SIZE&&nz>=0&&nz<KV_CHUNK_SIZE)
                    if (blk[nx][ny][nz] != KV_BLOCK_AIR) return ny;
            }
    return y;
}

/* Side face (face 0-3) with independent y extent [oy_min, oy_max) and xz size s. */
static void add_side_face_lod(KvMesh *m, int face, uint16_t type,
                               float ox, float oy_min, float oy_max, float oz, float s) {
    float p[4][3];
    float nx=0, ny=0, nz=0;
    float pad=0.001f, u0=pad, v0=pad, u1=1.0f-pad, v1=1.0f-pad;
    switch (face) {
    case 0: nz= 1; p[0][0]=ox;  p[0][1]=oy_min;p[0][2]=oz+s; p[1][0]=ox+s;p[1][1]=oy_min;p[1][2]=oz+s; p[2][0]=ox+s;p[2][1]=oy_max;p[2][2]=oz+s; p[3][0]=ox;  p[3][1]=oy_max;p[3][2]=oz+s; break;
    case 1: nz=-1; p[0][0]=ox+s;p[0][1]=oy_min;p[0][2]=oz;   p[1][0]=ox;  p[1][1]=oy_min;p[1][2]=oz;   p[2][0]=ox;  p[2][1]=oy_max;p[2][2]=oz;   p[3][0]=ox+s;p[3][1]=oy_max;p[3][2]=oz;   break;
    case 2: nx=-1; p[0][0]=ox;  p[0][1]=oy_min;p[0][2]=oz;   p[1][0]=ox;  p[1][1]=oy_min;p[1][2]=oz+s; p[2][0]=ox;  p[2][1]=oy_max;p[2][2]=oz+s; p[3][0]=ox;  p[3][1]=oy_max;p[3][2]=oz;   break;
    default:nx= 1; p[0][0]=ox+s;p[0][1]=oy_min;p[0][2]=oz+s; p[1][0]=ox+s;p[1][1]=oy_min;p[1][2]=oz;   p[2][0]=ox+s;p[2][1]=oy_max;p[2][2]=oz;   p[3][0]=ox+s;p[3][1]=oy_max;p[3][2]=oz+s; break;
    }
    float r, g, b;
    kv_block_tint(type, &r, &g, &b);
    float layer = (float)kv_tex_layer(type, face);
    push_vertex(m, p[0][0],p[0][1],p[0][2], r,g,b, nx,ny,nz, 1.0f, u0,v0, layer);
    push_vertex(m, p[1][0],p[1][1],p[1][2], r,g,b, nx,ny,nz, 1.0f, u1,v0, layer);
    push_vertex(m, p[2][0],p[2][1],p[2][2], r,g,b, nx,ny,nz, 1.0f, u1,v1, layer);
    push_vertex(m, p[0][0],p[0][1],p[0][2], r,g,b, nx,ny,nz, 1.0f, u0,v0, layer);
    push_vertex(m, p[2][0],p[2][1],p[2][2], r,g,b, nx,ny,nz, 1.0f, u1,v1, layer);
    push_vertex(m, p[3][0],p[3][1],p[3][2], r,g,b, nx,ny,nz, 1.0f, u0,v1, layer);
}

static void add_face_lod(KvMesh *m, int face, uint16_t type, float ox, float oy, float oz, float s) {
    float p[4][3];
    float nx=0, ny=0, nz=0;
    float pad=0.001f, u0=pad, v0=pad, u1=1.0f-pad, v1=1.0f-pad;

    switch (face) {
    case 0: nz= 1; p[0][0]=ox;  p[0][1]=oy;  p[0][2]=oz+s; p[1][0]=ox+s;p[1][1]=oy;  p[1][2]=oz+s; p[2][0]=ox+s;p[2][1]=oy+s;p[2][2]=oz+s; p[3][0]=ox;  p[3][1]=oy+s;p[3][2]=oz+s; break;
    case 1: nz=-1; p[0][0]=ox+s;p[0][1]=oy;  p[0][2]=oz;   p[1][0]=ox;  p[1][1]=oy;  p[1][2]=oz;   p[2][0]=ox;  p[2][1]=oy+s;p[2][2]=oz;   p[3][0]=ox+s;p[3][1]=oy+s;p[3][2]=oz;   break;
    case 2: nx=-1; p[0][0]=ox;  p[0][1]=oy;  p[0][2]=oz;   p[1][0]=ox;  p[1][1]=oy;  p[1][2]=oz+s; p[2][0]=ox;  p[2][1]=oy+s;p[2][2]=oz+s; p[3][0]=ox;  p[3][1]=oy+s;p[3][2]=oz;   break;
    case 3: nx= 1; p[0][0]=ox+s;p[0][1]=oy;  p[0][2]=oz+s; p[1][0]=ox+s;p[1][1]=oy;  p[1][2]=oz;   p[2][0]=ox+s;p[2][1]=oy+s;p[2][2]=oz;   p[3][0]=ox+s;p[3][1]=oy+s;p[3][2]=oz+s; break;
    case 4: ny= 1; p[0][0]=ox;  p[0][1]=oy+s;p[0][2]=oz+s; p[1][0]=ox+s;p[1][1]=oy+s;p[1][2]=oz+s; p[2][0]=ox+s;p[2][1]=oy+s;p[2][2]=oz;   p[3][0]=ox;  p[3][1]=oy+s;p[3][2]=oz;   break;
    default:ny=-1; p[0][0]=ox;  p[0][1]=oy;  p[0][2]=oz;   p[1][0]=ox+s;p[1][1]=oy;  p[1][2]=oz;   p[2][0]=ox+s;p[2][1]=oy;  p[2][2]=oz+s; p[3][0]=ox;  p[3][1]=oy;  p[3][2]=oz+s; break;
    }

    float r, g, b;
    kv_block_tint(type, &r, &g, &b);
    float layer = (float)kv_tex_layer(type, face);

    push_vertex(m, p[0][0],p[0][1],p[0][2], r,g,b, nx,ny,nz, 1.0f, u0,v0, layer);
    push_vertex(m, p[1][0],p[1][1],p[1][2], r,g,b, nx,ny,nz, 1.0f, u1,v0, layer);
    push_vertex(m, p[2][0],p[2][1],p[2][2], r,g,b, nx,ny,nz, 1.0f, u1,v1, layer);
    push_vertex(m, p[0][0],p[0][1],p[0][2], r,g,b, nx,ny,nz, 1.0f, u0,v0, layer);
    push_vertex(m, p[2][0],p[2][1],p[2][2], r,g,b, nx,ny,nz, 1.0f, u1,v1, layer);
    push_vertex(m, p[3][0],p[3][1],p[3][2], r,g,b, nx,ny,nz, 1.0f, u0,v1, layer);
}

void kv_mesh_generate_lod(KvMesh *m,
                          uint16_t blk[KV_CHUNK_SIZE][KV_CHUNK_SIZE][KV_CHUNK_SIZE],
                          int32_t cx, int32_t cy, int32_t cz, int step) {
    if (step <= 1) { kv_mesh_generate(m, blk, cx, cy, cz); return; }
    m->count = 0;
    float s = (float)step;
    for (int lx = 0; lx < KV_CHUNK_SIZE; lx += step) {
        for (int ly = 0; ly < KV_CHUNK_SIZE; ly += step) {
            for (int lz = 0; lz < KV_CHUNK_SIZE; lz += step) {
                uint16_t t = dominant_block(blk,lx,ly,lz,step);
                if (t == KV_BLOCK_AIR) continue;
                /* Actual surface bounds within this super-block. */
                int top_y = super_top_y(blk,lx,ly,lz,step);
                int bot_y = super_bot_y(blk,lx,ly,lz,step);
                float ox     = (float)(cx*KV_CHUNK_SIZE + lx);
                float oy_min = (float)(cy*KV_CHUNK_SIZE + bot_y);
                float oy_max = (float)(cy*KV_CHUNK_SIZE + top_y + 1);
                float oz     = (float)(cz*KV_CHUNK_SIZE + lz);
                /* Top/bottom faces: check overlap in the face's y-range. */
                if (!lod_solid_overlap(blk,lx,ly+step,lz,step,top_y+1-step,top_y+1)) add_face_lod(m,4,t,ox,oy_max-s,oz,s);
                if (!lod_solid_overlap(blk,lx,ly-step,lz,step,bot_y-step,bot_y-1)) add_face_lod(m,5,t,ox,oy_min,  oz,s);
                /* Side faces: check overlap in [bot_y, top_y] only. */
                if (!lod_solid_overlap(blk,lx,  ly,  lz+step,step,bot_y,top_y)) add_side_face_lod(m,0,t,ox,oy_min,oy_max,oz,s);
                if (!lod_solid_overlap(blk,lx,  ly,  lz-step,step,bot_y,top_y)) add_side_face_lod(m,1,t,ox,oy_min,oy_max,oz,s);
                if (!lod_solid_overlap(blk,lx-step,ly,lz,    step,bot_y,top_y)) add_side_face_lod(m,2,t,ox,oy_min,oy_max,oz,s);
                if (!lod_solid_overlap(blk,lx+step,ly,lz,    step,bot_y,top_y)) add_side_face_lod(m,3,t,ox,oy_min,oy_max,oz,s);
            }
        }
    }
}

static void mesh_setup_attribs(void) {
    renderer_attrib_pointer(0, 3, R_TYPE_FLOAT, false, sizeof(KvVertex), 0);
    renderer_enable_attrib(0);
    renderer_attrib_pointer(1, 3, R_TYPE_FLOAT, false, sizeof(KvVertex), 16);
    renderer_enable_attrib(1);
    renderer_attrib_pointer(2, 3, R_TYPE_FLOAT, false, sizeof(KvVertex), 32);
    renderer_enable_attrib(2);
    renderer_attrib_pointer(3, 1, R_TYPE_FLOAT, false, sizeof(KvVertex), 44);
    renderer_enable_attrib(3);
    renderer_attrib_pointer(4, 2, R_TYPE_FLOAT, false, sizeof(KvVertex), 48);
    renderer_enable_attrib(4);
    renderer_attrib_pointer(5, 1, R_TYPE_FLOAT, false, sizeof(KvVertex), 56);
    renderer_enable_attrib(5);
}

void kv_mesh_upload(KvMesh *m) {
    if (m->count == 0) return;
    if (m->vao == R_INVALID_HANDLE) m->vao = renderer_create_vao();
    if (m->vbo == R_INVALID_HANDLE) m->vbo = renderer_create_buffer();
    renderer_bind_vao(m->vao);
    renderer_bind_buffer(R_BUF_ARRAY, m->vbo);
    renderer_buffer_data(R_BUF_ARRAY, m->count * sizeof(KvVertex), m->verts, R_USAGE_DYNAMIC);
    mesh_setup_attribs();
    renderer_bind_buffer(R_BUF_ARRAY, R_INVALID_HANDLE);
    renderer_bind_vao(R_INVALID_HANDLE);
}

void kv_mesh_free(KvMesh *m) {
    free(m->verts);
    m->verts = NULL;
    if (m->vao != R_INVALID_HANDLE) { renderer_destroy_vao(m->vao);    m->vao = R_INVALID_HANDLE; }
    if (m->vbo != R_INVALID_HANDLE) { renderer_destroy_buffer(m->vbo); m->vbo = R_INVALID_HANDLE; }
    m->count = m->cap = 0;
}
