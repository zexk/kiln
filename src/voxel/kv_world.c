#include "kv_internal.h"
#include "log.h"
#include "frustum.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
#if defined(_WIN32)
#  include <direct.h>
#  define kv_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define kv_mkdir(p) mkdir((p), 0755)
#endif

/* ── Save format ─────────────────────────────────────────────────────────── */

#define SAVE_MAGIC      "KYUBCHNK"
#define SAVE_MAJOR      2
#define SAVE_MINOR      0
#define CHUNK_VOLUME    (KV_CHUNK_SIZE * KV_CHUNK_SIZE * KV_CHUNK_SIZE)
#define LOD_STEPS       {1, 2, 4}

static void write_u16(FILE *f, uint16_t v) { fputc(v&0xff,f); fputc((v>>8)&0xff,f); }
static void write_u32(FILE *f, uint32_t v) { write_u16(f,(uint16_t)(v&0xffff)); write_u16(f,(uint16_t)(v>>16)); }
static void write_i32(FILE *f, int32_t v)  { write_u32(f,(uint32_t)v); }

static bool read_u16(FILE *f, uint16_t *o) { int a=fgetc(f),b=fgetc(f); if(a==EOF||b==EOF)return false; *o=(uint16_t)(a|b<<8); return true; }
static bool read_u32(FILE *f, uint32_t *o) { uint16_t lo,hi; if(!read_u16(f,&lo)||!read_u16(f,&hi))return false; *o=(uint32_t)lo|((uint32_t)hi<<16); return true; }
static bool read_i32(FILE *f, int32_t *o)  { uint32_t v; if(!read_u32(f,&v))return false; *o=(int32_t)v; return true; }

static void ensure_dirs(const char *dir) {
    kv_mkdir("saves");
    if (kv_mkdir(dir) != 0 && errno != EEXIST)
        LOG_WARN(LOG_CAT_WORLD, "Failed to create save dir %s: %s", dir, strerror(errno));
}

static void chunk_path(char *out, size_t n, const char *dir, int32_t cx, int32_t cy, int32_t cz) {
    snprintf(out, n, "%s/chunk_%d_%d_%d.kch", dir, (int)cx, (int)cy, (int)cz);
}

static uint16_t palette_find_or_add(const char **ids, uint16_t *count, const char *id) {
    for (uint16_t i = 0; i < *count; i++) if (strcmp(ids[i], id) == 0) return i;
    if (*count >= 256) return 0;
    ids[(*count)] = id;
    return (*count)++;
}

static bool save_chunk(const KvSlot *sl, const char *save_dir) {
    if (!sl || !sl->active || !sl->chunk) return false;
    const KvChunk *c = sl->chunk;
    char path[512], tmp[540];
    chunk_path(path, sizeof(path), save_dir, c->cx, c->cy, c->cz);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    ensure_dirs(save_dir);

    FILE *f = fopen(tmp, "wb");
    if (!f) { LOG_WARN(LOG_CAT_WORLD,"Cannot open %s: %s", tmp, strerror(errno)); return false; }

    /* Collect blocks into flat arrays and build palette */
    const char *palette[256]; uint16_t pal_count = 0;
    uint16_t block_idx[CHUNK_VOLUME];
    uint16_t flat_meta[CHUNK_VOLUME];
    bool has_meta = false;
    int n = 0;
    for (int lx=0;lx<KV_CHUNK_SIZE;lx++) for (int ly=0;ly<KV_CHUNK_SIZE;ly++) for (int lz=0;lz<KV_CHUNK_SIZE;lz++) {
        uint16_t id = c->blocks[lx][ly][lz];
        const kv_block_def_t *d = kv_block_get(id);
        const char *id_str = (d && d->id) ? d->id : "kyub:air";
        block_idx[n] = palette_find_or_add(palette, &pal_count, id_str);
        flat_meta[n] = c->meta[lx][ly][lz];
        if (flat_meta[n]) has_meta = true;
        n++;
    }

    uint32_t section_count = has_meta ? 3u : 2u;

    /* Header */
    fwrite(SAVE_MAGIC,1,8,f);
    write_u16(f,SAVE_MAJOR); write_u16(f,SAVE_MINOR);
    write_i32(f,c->cx); write_i32(f,c->cy); write_i32(f,c->cz);
    write_u32(f,section_count);

    /* PLTE section */
    uint32_t pal_size = 2;
    for (uint16_t i=0;i<pal_count;i++) pal_size += 2 + (uint32_t)strlen(palette[i]);
    fwrite("PLTE",1,4,f); write_u32(f,pal_size); write_u16(f,pal_count);
    for (uint16_t i=0;i<pal_count;i++) { uint16_t l=(uint16_t)strlen(palette[i]); write_u16(f,l); fwrite(palette[i],1,l,f); }

    /* BLKS section: dims + encoding byte + palette indices */
    fwrite("BLKS",1,4,f); write_u32(f,7+CHUNK_VOLUME*2);
    write_u16(f,KV_CHUNK_SIZE); write_u16(f,KV_CHUNK_SIZE); write_u16(f,KV_CHUNK_SIZE);
    fputc(0,f);
    for (int i=0;i<CHUNK_VOLUME;i++) write_u16(f,block_idx[i]);

    /* META section (optional) */
    if (has_meta) {
        fwrite("META",1,4,f); write_u32(f,CHUNK_VOLUME*2);
        for (int i=0;i<CHUNK_VOLUME;i++) write_u16(f,flat_meta[i]);
    }

    bool ok = ferror(f)==0;
    if (fclose(f)!=0) ok=false;
    if (!ok) { remove(tmp); LOG_WARN(LOG_CAT_WORLD,"Write error %s",tmp); return false; }
    if (rename(tmp,path)!=0) { remove(tmp); LOG_WARN(LOG_CAT_WORLD,"Rename failed: %s",strerror(errno)); return false; }
    LOG_DEBUG(LOG_CAT_WORLD,"Saved chunk %d,%d,%d", (int)c->cx,(int)c->cy,(int)c->cz);
    return true;
}

static bool load_chunk(KvChunk *c, const char *save_dir) {
    char path[512];
    chunk_path(path,sizeof(path),save_dir,c->cx,c->cy,c->cz);
    FILE *f = fopen(path,"rb"); if(!f) return false;

    char magic[8]; uint16_t major,minor; int32_t fx,fy,fz; uint32_t sec_count;
    if (fread(magic,1,8,f)!=8||memcmp(magic,SAVE_MAGIC,8)||
        !read_u16(f,&major)||!read_u16(f,&minor)||
        !read_i32(f,&fx)||!read_i32(f,&fy)||!read_i32(f,&fz)||
        !read_u32(f,&sec_count)) {
        LOG_WARN(LOG_CAT_WORLD,"Bad chunk save: %s", path); fclose(f); return false;
    }
    (void)minor;
    if (major > SAVE_MAJOR) { LOG_WARN(LOG_CAT_WORLD,"Newer save v%u: %s",major,path); fclose(f); return false; }
    if (fx!=c->cx||fy!=c->cy||fz!=c->cz) { LOG_WARN(LOG_CAT_WORLD,"Coord mismatch: %s",path); fclose(f); return false; }

    /* Old v1 saves had no cy — treated as cy=0 (already validated above) */

    char pal_ids[256][64]; uint16_t pal_types[256]; uint16_t pal_count=0;
    memset(pal_ids,0,sizeof(pal_ids));
    bool loaded_blocks=false, loaded_meta=false;

    for (uint32_t s=0;s<sec_count;s++) {
        char sec_type[4]; uint32_t sec_size; long start;
        if (fread(sec_type,1,4,f)!=4||!read_u32(f,&sec_size)) break;
        start=ftell(f);

        if (!memcmp(sec_type,"PLTE",4)) {
            uint16_t cnt; if(!read_u16(f,&cnt)) break;
            if(cnt>256) cnt=256;
            for (uint16_t i=0;i<cnt;i++) {
                uint16_t len; if(!read_u16(f,&len)) break;
                uint16_t copy=len<63?len:63;
                if(fread(pal_ids[i],1,copy,f)!=copy) break;
                if(len>copy) fseek(f,len-copy,SEEK_CUR);
                pal_ids[i][copy]='\0';
                pal_types[i]=kv_block_id_from_string(pal_ids[i]);
                if(pal_types[i]==KV_BLOCK_AIR&&strcmp(pal_ids[i],"kyub:air")!=0)
                    LOG_WARN(LOG_CAT_WORLD,"Unknown block '%s' in %s",pal_ids[i],path);
            }
            pal_count=cnt;
        } else if (!memcmp(sec_type,"BLKS",4)) {
            uint16_t sx,sy,sz; int enc;
            if(!read_u16(f,&sx)||!read_u16(f,&sy)||!read_u16(f,&sz)) break;
            enc=fgetc(f);
            if(sx==KV_CHUNK_SIZE&&sy==KV_CHUNK_SIZE&&sz==KV_CHUNK_SIZE&&enc==0) {
                for (int lx=0;lx<KV_CHUNK_SIZE;lx++) for(int ly=0;ly<KV_CHUNK_SIZE;ly++) for(int lz=0;lz<KV_CHUNK_SIZE;lz++) {
                    uint16_t idx; if(!read_u16(f,&idx)) goto done;
                    c->blocks[lx][ly][lz]=(idx<pal_count)?pal_types[idx]:KV_BLOCK_AIR;
                }
                loaded_blocks=true;
            }
        } else if (!memcmp(sec_type,"META",4)) {
            for (int lx=0;lx<KV_CHUNK_SIZE;lx++) for(int ly=0;ly<KV_CHUNK_SIZE;ly++) for(int lz=0;lz<KV_CHUNK_SIZE;lz++) {
                uint16_t mv; if(!read_u16(f,&mv)) goto done;
                c->meta[lx][ly][lz]=mv;
            }
            loaded_meta=true;
        }
        if(fseek(f,start+(long)sec_size,SEEK_SET)!=0) break;
    }
done:
    fclose(f);
    (void)loaded_meta;
    if(loaded_blocks) LOG_DEBUG(LOG_CAT_WORLD,"Loaded chunk %d,%d,%d",(int)c->cx,(int)c->cy,(int)c->cz);
    return loaded_blocks;
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

static int chunk_coord(int w) {
    if (w >= 0) return w / KV_CHUNK_SIZE;
    return -((-w + KV_CHUNK_SIZE - 1) / KV_CHUNK_SIZE);
}

static KvSlot *find_slot(const kv_world_t *w, int32_t cx, int32_t cy, int32_t cz) {
    for (int i=0;i<w->cap;i++)
        if (w->slots[i].active && w->slots[i].chunk->cx==cx && w->slots[i].chunk->cy==cy && w->slots[i].chunk->cz==cz)
            return &w->slots[i];
    return NULL;
}

static void unload_slot(kv_world_t *w, int idx) {
    KvSlot *sl = &w->slots[idx]; if (!sl->active) return;
    if (sl->save_dirty) save_chunk(sl, w->save_dir);
    LOG_DEBUG(LOG_CAT_WORLD,"Unloaded chunk %d,%d,%d",(int)sl->chunk->cx,(int)sl->chunk->cy,(int)sl->chunk->cz);
    for (int l=0;l<KV_LOD_LEVELS;l++) kv_mesh_free(&sl->meshes[l]);
    free(sl->chunk);
    sl->chunk=NULL; sl->active=false; sl->dirty=sl->save_dirty=false;
    w->count--;
}

static void add_slot(kv_world_t *w, int32_t cx, int32_t cy, int32_t cz) {
    for (int i=0;i<w->cap;i++) {
        if (w->slots[i].active) continue;
        KvSlot *sl = &w->slots[i];
        sl->chunk = calloc(1, sizeof(KvChunk));
        sl->chunk->cx=cx; sl->chunk->cy=cy; sl->chunk->cz=cz;
        sl->chunk->aabb_min=(vec3_t){(float)(cx*KV_CHUNK_SIZE),(float)(cy*KV_CHUNK_SIZE),(float)(cz*KV_CHUNK_SIZE)};
        sl->chunk->aabb_max=(vec3_t){(float)((cx+1)*KV_CHUNK_SIZE),(float)((cy+1)*KV_CHUNK_SIZE),(float)((cz+1)*KV_CHUNK_SIZE)};

        if (!load_chunk(sl->chunk, w->save_dir))
            w->gen(sl->chunk->blocks, sl->chunk->meta, cx, cy, cz, w->gen_ctx);

        const int steps[KV_LOD_LEVELS] = LOD_STEPS;
        for (int l=0;l<KV_LOD_LEVELS;l++) {
            kv_mesh_init(&sl->meshes[l]);
            kv_mesh_generate_lod(&sl->meshes[l], sl->chunk->blocks, cx, cy, cz, steps[l]);
            kv_mesh_upload(&sl->meshes[l]);
        }
        sl->active=true; sl->dirty=false; sl->save_dirty=false;
        w->count++;
        LOG_DEBUG(LOG_CAT_WORLD,"Loaded chunk %d,%d,%d (slot %d)",(int)cx,(int)cy,(int)cz,i);
        return;
    }
    LOG_WARN(LOG_CAT_WORLD,"No free slot for chunk %d,%d,%d (active=%d, cap=%d)",(int)cx,(int)cy,(int)cz,w->count,w->cap);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

kv_world_t *kv_world_create(int horiz_dist, int vert_radius,
                             kv_gen_fn gen, void *gen_ctx,
                             const char *save_dir) {
    if (horiz_dist < 1) horiz_dist = 1;
    if (vert_radius < 0) vert_radius = 0;
    int hs = 2*(horiz_dist+2)+1;
    int vs = 2*(vert_radius+2)+1;
    kv_world_t *w = calloc(1, sizeof(kv_world_t));
    w->cap         = hs * hs * vs;
    w->slots       = calloc(w->cap, sizeof(KvSlot));
    w->horiz_dist  = horiz_dist;
    w->vert_radius = vert_radius;
    w->gen         = gen;
    w->gen_ctx     = gen_ctx;
    w->shader      = R_INVALID_HANDLE;
    w->loc_model   = w->loc_view = w->loc_proj = -1;
    w->loc_fog_color = w->loc_fog_density = w->loc_texture = -1;
    strncpy(w->save_dir, save_dir, sizeof(w->save_dir)-1);
    ensure_dirs(save_dir);
    return w;
}

void kv_world_destroy(kv_world_t *world) {
    for (int i=0;i<world->cap;i++) if(world->slots[i].active) unload_slot(world,i);
    free(world->slots);
    free(world);
}

void kv_world_update(kv_world_t *world, vec3_t camera_pos) {
    world->last_cam = camera_pos;
    int cam_cx = chunk_coord((int)floorf(camera_pos.x));
    int cam_cy = chunk_coord((int)floorf(camera_pos.y));
    int cam_cz = chunk_coord((int)floorf(camera_pos.z));
    int ld = world->horiz_dist + 1;
    int lv = world->vert_radius + 1;

    for (int cx=cam_cx-ld; cx<=cam_cx+ld; cx++)
        for (int cy=cam_cy-lv; cy<=cam_cy+lv; cy++)
            for (int cz=cam_cz-ld; cz<=cam_cz+ld; cz++)
                if (!find_slot(world,cx,cy,cz)) add_slot(world,cx,cy,cz);

    for (int i=0;i<world->cap;i++) {
        if (!world->slots[i].active) continue;
        KvChunk *c = world->slots[i].chunk;
        if (abs(c->cx-cam_cx)>world->horiz_dist+2 ||
            abs(c->cy-cam_cy)>world->vert_radius+2 ||
            abs(c->cz-cam_cz)>world->horiz_dist+2)
            unload_slot(world,i);
    }

    const int steps[KV_LOD_LEVELS] = LOD_STEPS;
    for (int i=0;i<world->cap;i++) {
        KvSlot *sl = &world->slots[i]; if (!sl->active || !sl->dirty) continue;
        for (int l=0;l<KV_LOD_LEVELS;l++) {
            kv_mesh_generate_lod(&sl->meshes[l], sl->chunk->blocks,
                                 sl->chunk->cx, sl->chunk->cy, sl->chunk->cz, steps[l]);
            kv_mesh_upload(&sl->meshes[l]);
        }
        sl->dirty=false;
    }
}

void kv_world_flush_saves(kv_world_t *world) {
    for (int i=0;i<world->cap;i++) {
        KvSlot *sl = &world->slots[i];
        if (sl->active && sl->save_dirty && save_chunk(sl, world->save_dir))
            sl->save_dirty=false;
    }
}

uint16_t kv_world_get_block(const kv_world_t *world, int x, int y, int z) {
    KvSlot *sl = find_slot(world, chunk_coord(x), chunk_coord(y), chunk_coord(z));
    if (!sl) return KV_BLOCK_AIR;
    int lx=x-sl->chunk->cx*KV_CHUNK_SIZE, ly=y-sl->chunk->cy*KV_CHUNK_SIZE, lz=z-sl->chunk->cz*KV_CHUNK_SIZE;
    if (lx<0||lx>=KV_CHUNK_SIZE||ly<0||ly>=KV_CHUNK_SIZE||lz<0||lz>=KV_CHUNK_SIZE) return KV_BLOCK_AIR;
    return sl->chunk->blocks[lx][ly][lz];
}

uint16_t kv_world_get_meta(const kv_world_t *world, int x, int y, int z) {
    KvSlot *sl = find_slot(world, chunk_coord(x), chunk_coord(y), chunk_coord(z));
    if (!sl) return 0;
    int lx=x-sl->chunk->cx*KV_CHUNK_SIZE, ly=y-sl->chunk->cy*KV_CHUNK_SIZE, lz=z-sl->chunk->cz*KV_CHUNK_SIZE;
    if (lx<0||lx>=KV_CHUNK_SIZE||ly<0||ly>=KV_CHUNK_SIZE||lz<0||lz>=KV_CHUNK_SIZE) return 0;
    return sl->chunk->meta[lx][ly][lz];
}

void kv_world_set_block(kv_world_t *world, int x, int y, int z, uint16_t type) {
    int32_t cx=chunk_coord(x), cy=chunk_coord(y), cz=chunk_coord(z);
    KvSlot *sl = find_slot(world,cx,cy,cz);
    if (!sl && world->count < world->cap) { add_slot(world,cx,cy,cz); sl=find_slot(world,cx,cy,cz); }
    if (!sl) { LOG_WARN(LOG_CAT_WORLD,"set_block: chunk %d,%d,%d not loaded",(int)cx,(int)cy,(int)cz); return; }
    int lx=x-cx*KV_CHUNK_SIZE, ly=y-cy*KV_CHUNK_SIZE, lz=z-cz*KV_CHUNK_SIZE;
    sl->chunk->blocks[lx][ly][lz]=type;
    sl->dirty=sl->save_dirty=true;
}

void kv_world_set_meta(kv_world_t *world, int x, int y, int z, uint16_t meta) {
    int32_t cx=chunk_coord(x), cy=chunk_coord(y), cz=chunk_coord(z);
    KvSlot *sl = find_slot(world,cx,cy,cz);
    if (!sl) return;
    int lx=x-cx*KV_CHUNK_SIZE, ly=y-cy*KV_CHUNK_SIZE, lz=z-cz*KV_CHUNK_SIZE;
    sl->chunk->meta[lx][ly][lz]=meta;
    sl->save_dirty=true;
}

bool kv_world_is_solid(const kv_world_t *world, int x, int y, int z) {
    return kv_block_is_solid(kv_world_get_block(world, x, y, z));
}

bool kv_solid_query(void *world_opaque, int x, int y, int z) {
    return kv_world_is_solid((kv_world_t *)world_opaque, x, y, z);
}

/* ── Rendering ───────────────────────────────────────────────────────────── */

static void ensure_shader(kv_world_t *w) {
    if (w->shader != R_INVALID_HANDLE) return;
    w->shader = renderer_create_program("shaders/voxel.vert", "shaders/voxel.frag");
    if (w->shader == R_INVALID_HANDLE) return;
    w->loc_model       = renderer_uniform_location(w->shader, "model");
    w->loc_view        = renderer_uniform_location(w->shader, "view");
    w->loc_proj        = renderer_uniform_location(w->shader, "projection");
    w->loc_fog_color   = renderer_uniform_location(w->shader, "uFogColor");
    w->loc_fog_density = renderer_uniform_location(w->shader, "uFogDensity");
    w->loc_texture     = renderer_uniform_location(w->shader, "uTexture");
}

void kv_world_draw(kv_world_t *world, R_Texture tex_array,
                   mat4_t view, mat4_t proj,
                   vec3_t fog_color, float fog_density) {
    ensure_shader(world);
    if (world->shader == R_INVALID_HANDLE) return;

    frustum_t frustum;
    frustum_extract(&frustum, mat4_mul(proj, view));

    int cam_cx = chunk_coord((int)floorf(world->last_cam.x));
    int cam_cy = chunk_coord((int)floorf(world->last_cam.y));
    int cam_cz = chunk_coord((int)floorf(world->last_cam.z));

    renderer_use_program(world->shader);
    renderer_active_texture(0);
    renderer_bind_texture(R_TEX_2D, tex_array);
    renderer_uniform_int(world->loc_texture, 0);
    renderer_uniform_mat4(world->loc_view, view.m);
    renderer_uniform_mat4(world->loc_proj, proj.m);
    renderer_uniform_vec3(world->loc_fog_color, fog_color.x, fog_color.y, fog_color.z);
    renderer_uniform_float(world->loc_fog_density, fog_density);

    mat4_t identity = mat4_identity();
    renderer_uniform_mat4(world->loc_model, identity.m);

    for (int i=0;i<world->cap;i++) {
        KvSlot *sl = &world->slots[i]; if (!sl->active) continue;
        KvChunk *c = sl->chunk;
        if (!frustum_intersects_aabb(&frustum, c->aabb_min, c->aabb_max)) continue;
        int dx = abs(c->cx-cam_cx), dy = abs(c->cy-cam_cy), dz = abs(c->cz-cam_cz);
        int dist = dx > dz ? dx : dz; if (dy > 0) dist++;
        int lod = dist<=2 ? 0 : dist<=5 ? 1 : 2;
        KvMesh *m = &sl->meshes[lod]; if (m->count==0) continue;
        renderer_bind_vao(m->vao);
        renderer_draw_arrays(R_PRIM_TRIANGLES, 0, (int)m->count);
    }
    renderer_bind_vao(R_INVALID_HANDLE);
}
