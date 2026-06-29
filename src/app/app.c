#include "app.h"

#include "aabb.h"
#include "camera.h"
#include "core.h"
#include "gizmo.h"
#include "image.h"
#include "mesh.h"
#include "obj.h"
#include "render.h"
#include "ui.h"
#include "assets.h"
#include "kmesh.h"
#include "scene.h"

#include "settings.h"
#include "timer.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *SETTINGS_PATH = "settings.ini";

/* Where models/textures live, resolved at runtime (env override, else the
   installed share/kiln/assets, else the build-time source dir). */
static char g_asset_dir[1024];

static const char *model_path(char *buf, size_t cap, const char *file) {
    snprintf(buf, cap, "%s/models/%s", g_asset_dir, file);
    return buf;
}

static material_handle_t solid(float r, float g, float b) {
    return render_create_material((vec4_t){r, g, b, 1.0f},
                                  RENDER_TEXTURE_INVALID);
}

/* Load an image file as an albedo texture and wrap it in a white-base material
   (so the texture shows unmodulated). Returns INVALID if the image won't load. */
static material_handle_t textured(const char *path) {
    uint8_t *pixels;
    int w, h;
    if (!image_load(path, &pixels, &w, &h)) {
        return RENDER_MATERIAL_INVALID;
    }
    texture_handle_t tex =
        render_upload_texture(pixels, (uint32_t)w, (uint32_t)h);
    image_free(pixels);
    return render_create_material((vec4_t){1.0f, 1.0f, 1.0f, 1.0f}, tex);
}

/* Register a spawnable prototype from a CPU mesh: centre it on the origin
   (so an entity's translation is its pivot), upload it, and record the
   normalize-to-~2-units scale. Returns its index or -1. */
static int add_prototype(app_t *app, const char *name, cpu_mesh_t *m,
                         material_handle_t mat) {
    vec3_t bmin, bmax;
    if (app->prototype_count >= APP_MAX_PROTOTYPES || mat == RENDER_MATERIAL_INVALID ||
        !cpu_mesh_bounds(m, &bmin, &bmax)) {
        return -1;
    }
    vec3_t extent = vec3_sub(bmax, bmin);
    float max_dim = extent.x;
    if (extent.y > max_dim) {
        max_dim = extent.y;
    }
    if (extent.z > max_dim) {
        max_dim = extent.z;
    }

    cpu_mesh_recenter(m);
    mesh_handle_t mesh = render_upload_mesh(m);
    if (mesh == RENDER_MESH_INVALID) {
        return -1;
    }

    prototype_t *p = &app->prototypes[app->prototype_count];
    p->name = name;
    p->mesh = mesh;
    p->material = mat;
    p->scale = (max_dim > 1e-6f) ? 2.0f / max_dim : 1.0f;

    /* Keep a local-space triangle copy for picking (positions + indices), plus
       the recentred AABB for broadphase. */
    p->pick_vcount = m->vertex_count;
    p->pick_pos = malloc(sizeof(vec3_t) * m->vertex_count);
    for (uint32_t i = 0; i < m->vertex_count; i++) {
        p->pick_pos[i] = m->vertices[i].position;
    }
    p->pick_icount = m->index_count;
    p->pick_idx = malloc(sizeof(uint32_t) * m->index_count);
    memcpy(p->pick_idx, m->indices, sizeof(uint32_t) * m->index_count);
    cpu_mesh_bounds(m, &p->pick_min, &p->pick_max); /* recentred bounds */
    return app->prototype_count++;
}

static int load_prototype(app_t *app, const char *name, const char *path,
                          material_handle_t mat) {
    cpu_mesh_t m;

    /* Prefer a pre-baked .kmesh alongside the .obj; fall back to OBJ parsing. */
    char kpath[1200];
    size_t plen = strlen(path);
    bool loaded = false;
    if (plen > 4 && path[plen - 4] == '.' && path[plen - 3] == 'o' &&
        path[plen - 2] == 'b' && path[plen - 1] == 'j') {
        snprintf(kpath, sizeof(kpath), "%.*s.kmesh", (int)(plen - 4), path);
        loaded = kmesh_load(kpath, &m);
    }
    if (!loaded && !obj_load(path, &m)) {
        return -1;
    }

    int idx = add_prototype(app, name, &m, mat);
    cpu_mesh_free(&m);
    return idx;
}

/* Instantiate a prototype as an entity, centred on `at`. Returns the entity. */
static entity_t spawn(app_t *app, int proto, vec3_t at) {
    if (proto < 0 || proto >= app->prototype_count) {
        return ECS_ENTITY_NULL;
    }
    prototype_t *p = &app->prototypes[proto];
    entity_t e = entity_create(app->world);
    transform_t *t = entity_add_component(app->world, e, app->transform_id);
    t->rotation = quat_identity();
    t->scale = (vec3_t){p->scale, p->scale, p->scale};
    t->position = at; /* mesh is pre-centred, so this is its pivot/visual centre */

    renderable_t *r = entity_add_component(app->world, e, app->renderable_id);
    r->mesh = p->mesh;
    r->material = p->material;
    return e;
}

/* Register the prototypes and lay one of each out in a starting row. Models
   that fail to load (e.g. not fetched) are skipped; the procedural cube always
   works, so the app runs even with no asset files present. */
static void build_scene(app_t *app) {
    core_resource_dir(g_asset_dir, sizeof(g_asset_dir), "KILN_ASSET_DIR",
                      "assets", KILN_ASSET_DIR);

    char path[1200];
    material_handle_t spot_mat =
        textured(model_path(path, sizeof(path), "spot.png"));
    if (spot_mat == RENDER_MATERIAL_INVALID) {
        spot_mat = solid(0.90f, 0.85f, 0.80f);
    }
    app->highlight_material = solid(1.0f, 0.80f, 0.15f);

    cpu_mesh_t cube;
    if (cpu_mesh_cube(&cube)) {
        add_prototype(app, "CUBE", &cube, solid(0.85f, 0.55f, 0.25f));
        cpu_mesh_free(&cube);
    }
    load_prototype(app, "COW", model_path(path, sizeof(path), "cow.obj"),
                   solid(0.80f, 0.30f, 0.30f));
    load_prototype(app, "SPOT", model_path(path, sizeof(path), "spot.obj"),
                   spot_mat);
    load_prototype(app, "FANDISK", model_path(path, sizeof(path), "fandisk.obj"),
                   solid(0.45f, 0.65f, 0.85f));
    load_prototype(app, "CHEBURASHKA",
                   model_path(path, sizeof(path), "cheburashka.obj"),
                   solid(0.55f, 0.45f, 0.70f));
    load_prototype(app, "ARMADILLO",
                   model_path(path, sizeof(path), "armadillo.obj"),
                   solid(0.55f, 0.75f, 0.45f));

    const float spacing = 2.8f;
    float x0 = -spacing * (float)(app->prototype_count - 1) * 0.5f;
    for (int i = 0; i < app->prototype_count; i++) {
        entity_t e = spawn(app, i, (vec3_t){x0 + spacing * (float)i, 0.0f, 0.0f});
        if (i == 0) {
            app->selected = e; /* start with a selection so the gizmo shows */
        }
    }
}

bool app_init(app_t *app) {
    core_init();

    static const key_binding_t fly_defaults[] = {
        {"fly_forward",  KEY_W},
        {"fly_backward", KEY_S},
        {"fly_left",     KEY_A},
        {"fly_right",    KEY_D},
        {"fly_up",       KEY_E},
        {"fly_down",     KEY_Q},
    };
    settings_init(&app->settings, fly_defaults,
                  (int)(sizeof(fly_defaults) / sizeof(fly_defaults[0])));
    settings_load(&app->settings, SETTINGS_PATH);

    app->world = world_create();
    app->transform_id = component_register(app->world, "transform",
                                           sizeof(transform_t),
                                           _Alignof(transform_t));
    app->renderable_id =
        component_register(app->world, "renderable", sizeof(renderable_t),
                           _Alignof(renderable_t));

    app->window = window_create("Kiln",
                                app->settings.engine.width,
                                app->settings.engine.height);
    if (!app->window) {
        return false;
    }
    if (!render_init(app->window)) {
        return false;
    }
    assets_init();

    ui_init(&app->ui);
    gizmo_init(&app->gizmo);
    app->selected = ECS_ENTITY_NULL;
    app->sel_prototype = 0;
    app->spin_speed = 0.6f;
    app->vsync      = app->settings.engine.vsync;
    app->fps_limit  = app->settings.engine.fps_limit;
    render_set_vsync(app->vsync);
    app->bg_color[0] = 0.02f;
    app->bg_color[1] = 0.02f;
    app->bg_color[2] = 0.05f;
    app->light_yaw        = 49.0f;  /* reproduces the former hardcoded direction */
    app->light_pitch      = 58.0f;
    app->light_intensity  = 1.0f;
    app->ambient_intensity = 1.0f;
    app->show_grid        = true;

    build_scene(app); /* may set an initial selection */

    /* Instanced cubes on the ground plane — exercises GPU cull + indirect draw. */
    app->inst_mesh = RENDER_MESH_INVALID;
    app->inst_mat  = RENDER_MATERIAL_INVALID;
    {
        cpu_mesh_t cube = {0};
        if (cpu_mesh_cube(&cube)) {
            app->inst_mesh = render_upload_mesh(&cube);
            cpu_mesh_free(&cube);
            app->inst_mat = render_create_material(
                (vec4_t){0.2f, 0.7f, 0.9f, 1.0f}, RENDER_TEXTURE_INVALID);
        }
    }

    /* GPU particle emitter: small cubes falling under gravity. */
    app->particle_emitter = RENDER_GPU_EMITTER_INVALID;
    {
        cpu_mesh_t cube = {0};
        if (cpu_mesh_cube(&cube)) {
            mesh_handle_t m = render_upload_mesh(&cube);
            cpu_mesh_free(&cube);
            if (m != RENDER_MESH_INVALID) {
                material_handle_t mat = render_create_material(
                    (vec4_t){1.0f, 0.6f, 0.1f, 1.0f}, RENDER_TEXTURE_INVALID);
                if (mat != RENDER_MATERIAL_INVALID) {
                    app->particle_emitter = render_create_gpu_emitter(
                        m, mat, 4096, (vec3_t){0.0f, -9.8f, 0.0f});
                }
            }
        }
    }

    camera_init(&app->camera);
    app->camera.distance = 16.0f;
    app->camera.pitch = kln_radians(18.0f);

    fps_camera_init(&app->fly_cam);
    app->fly_pos = camera_eye(&app->camera);

    return true;
}

static query_iter_t renderable_query(app_t *app) {
    signature_t require;
    signature_clear(&require);
    signature_set(&require, app->transform_id);
    signature_set(&require, app->renderable_id);
    return query_iter(app->world, (query_desc_t){.require = require});
}

/* Spin every model about its local Y axis — an opt-in ECS system the UI drives,
   so toggling the checkbox visibly does something. */
static void rotate_system(app_t *app, float dt) {
    quat_t spin = quat_from_axis_angle((vec3_t){0, 1, 0}, app->spin_speed * dt);
    query_iter_t it = renderable_query(app);
    while (query_next(&it)) {
        transform_t *t = query_get(&it, app->transform_id);
        t->rotation = quat_normalize(quat_mul(spin, t->rotation));
    }
}

/* Aim the camera, then queue one draw per renderable entity. */
static void render_scene(app_t *app) {
    uint32_t w, h;
    window_size(app->window, &w, &h);
    float aspect = h ? (float)w / (float)h : 1.0f;

    mat4_t view = app->fly_mode
        ? fps_camera_view(&app->fly_cam, app->fly_pos)
        : camera_view(&app->camera);
    render_set_camera(view, camera_proj(&app->camera, aspect));

    float ly = kln_radians(app->light_yaw);
    float lp = kln_radians(app->light_pitch);
    float cp = cosf(lp);
    vec3_t light_dir = {cp * sinf(ly), sinf(lp), cp * cosf(ly)};
    float ki = app->light_intensity;
    float ai = app->ambient_intensity;
    render_set_light(light_dir,
                     (vec3_t){ki, ki, ki},
                     (vec3_t){0.16f * ai, 0.18f * ai, 0.24f * ai});

    uint32_t drawn = 0;
    query_iter_t it = renderable_query(app);
    while (query_next(&it)) {
        transform_t *t = query_get(&it, app->transform_id);
        renderable_t *r = query_get(&it, app->renderable_id);
        material_handle_t mat = (query_entity(&it) == app->selected)
                                    ? app->highlight_material
                                    : r->material;
        render_mesh(r->mesh, mat,
                    mat4_from_trs(t->position, t->rotation, t->scale));
        drawn++;
    }
    app->draw_count = drawn;

    /* Instanced ring of blue cubes — exercises the GPU cull + indirect path. */
    if (app->inst_mesh != RENDER_MESH_INVALID &&
        app->inst_mat  != RENDER_MATERIAL_INVALID) {
        static mat4_t ring[16];
        static bool ring_built = false;
        if (!ring_built) {
            ring_built = true;
            for (int i = 0; i < 16; i++) {
                float a = (float)i * (6.2831853f / 16.0f);
                float x = cosf(a) * 8.0f, z = sinf(a) * 8.0f;
                ring[i] = mat4_from_trs((vec3_t){x, -1.0f, z},
                                        quat_identity(), (vec3_t){0.4f, 0.4f, 0.4f});
            }
        }
        render_mesh_instanced(app->inst_mesh, app->inst_mat, ring, 16);
    }
}

/* ---- Euler angle helpers (XYZ intrinsic, degrees displayed in inspector) ---- */

static vec3_t quat_to_euler_deg(quat_t q) {
    float sinr = 2.0f * (q.w * q.x + q.y * q.z);
    float cosr = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    float rx = atan2f(sinr, cosr);

    float sinp = 2.0f * (q.w * q.y - q.z * q.x);
    float ry = fabsf(sinp) >= 1.0f ? copysignf(KLN_PI * 0.5f, sinp) : asinf(sinp);

    float siny = 2.0f * (q.w * q.z + q.x * q.y);
    float cosy = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    float rz = atan2f(siny, cosy);

    return (vec3_t){kln_degrees(rx), kln_degrees(ry), kln_degrees(rz)};
}

static quat_t euler_deg_to_quat(vec3_t deg) {
    float hx = kln_radians(deg.x) * 0.5f;
    float hy = kln_radians(deg.y) * 0.5f;
    float hz = kln_radians(deg.z) * 0.5f;
    float cx = cosf(hx), sx = sinf(hx);
    float cy = cosf(hy), sy = sinf(hy);
    float cz = cosf(hz), sz = sinf(hz);
    return quat_normalize((quat_t){
        sx * cy * cz + cx * sy * sz,
        cx * sy * cz - sx * cy * sz,
        cx * cy * sz + sx * sy * cz,
        cx * cy * cz - sx * sy * sz,
    });
}

/* ---- viewport grid ---- */

/* Draw a world-space line segment using the 2D overlay, clipping against the
   camera near plane so segments that straddle or cross behind the eye are
   handled correctly.  Clipping is done in homogeneous clip space: points with
   w < EPS are behind the camera; we linearly interpolate to find the clip
   boundary and project only the visible portion. */
static void grid_line(mat4_t vp, vec3_t a, vec3_t b,
                      float sw, float sh,
                      float thick, float r, float g, float bl) {
    vec4_t ca = mat4_mul_vec4(vp, (vec4_t){a.x, a.y, a.z, 1.0f});
    vec4_t cb = mat4_mul_vec4(vp, (vec4_t){b.x, b.y, b.z, 1.0f});
    const float EPS = 1e-4f;

    if (ca.w < EPS && cb.w < EPS) return;

    if (ca.w < EPS) {
        float t = (EPS - ca.w) / (cb.w - ca.w);
        ca = (vec4_t){ca.x + t * (cb.x - ca.x),
                      ca.y + t * (cb.y - ca.y),
                      ca.z + t * (cb.z - ca.z),
                      EPS};
    } else if (cb.w < EPS) {
        float t = (EPS - cb.w) / (ca.w - cb.w);
        cb = (vec4_t){cb.x + t * (ca.x - cb.x),
                      cb.y + t * (ca.y - cb.y),
                      cb.z + t * (ca.z - cb.z),
                      EPS};
    }

    float ax = (ca.x / ca.w * 0.5f + 0.5f) * sw;
    float ay = (1.0f - (ca.y / ca.w * 0.5f + 0.5f)) * sh;
    float bx = (cb.x / cb.w * 0.5f + 0.5f) * sw;
    float by = (1.0f - (cb.y / cb.w * 0.5f + 0.5f)) * sh;
    render_line(ax, ay, bx, by, thick, r, g, bl);
}

static void draw_grid(app_t *app) {
    uint32_t wu, hu;
    window_size(app->window, &wu, &hu);
    float sw = (float)wu;
    float sh = (float)hu;
    float aspect = hu > 0 ? sw / sh : 1.0f;

    mat4_t view = app->fly_mode
        ? fps_camera_view(&app->fly_cam, app->fly_pos)
        : camera_view(&app->camera);
    mat4_t vp = mat4_mul(camera_proj(&app->camera, aspect), view);

#define GRID_EXTENT 20.0f
#define GRID_STEPS  20

    for (int i = -GRID_STEPS; i <= GRID_STEPS; i++) {
        float f = (float)i;
        bool  axis   = (i == 0);
        bool  coarse = (!axis && i % 5 == 0);
        float thick  = axis ? 2.0f : (coarse ? 1.5f : 1.0f);
        float gray   = coarse ? 0.35f : 0.20f;

        /* Lines running along Z at constant X — when i==0 this is the Z axis. */
        float zr = axis ? 0.20f : gray;
        float zg = axis ? 0.20f : gray;
        float zb = axis ? 0.65f : gray;
        grid_line(vp, (vec3_t){f, 0.0f, -GRID_EXTENT},
                      (vec3_t){f, 0.0f,  GRID_EXTENT},
                  sw, sh, thick, zr, zg, zb);

        /* Lines running along X at constant Z — when i==0 this is the X axis. */
        float xr = axis ? 0.65f : gray;
        float xg = axis ? 0.20f : gray;
        float xb = axis ? 0.20f : gray;
        grid_line(vp, (vec3_t){-GRID_EXTENT, 0.0f, f},
                      (vec3_t){ GRID_EXTENT, 0.0f, f},
                  sw, sh, thick, xr, xg, xb);
    }
#undef GRID_EXTENT
#undef GRID_STEPS
}

/* ---- undo/redo history ---- */

static bool xforms_equal(const saved_xform_t *a, const saved_xform_t *b) {
    return vec3_distance(a->position, b->position) < 1e-5f &&
           vec3_distance(a->scale,    b->scale)    < 1e-5f &&
           fabsf(quat_dot(a->rotation, b->rotation)) > 1.0f - 1e-6f;
}

static void history_push(history_t *h, cmd_t c) {
    h->top = h->pos; /* discard redo tail */
    if (h->top == HISTORY_CAP) {
        memmove(&h->entries[0], &h->entries[1],
                (HISTORY_CAP - 1) * sizeof(cmd_t));
        h->top--;
        h->pos--;
    }
    h->entries[h->top++] = c;
    h->pos = h->top;
}

/* When an entity is re-created (undo DELETE / redo SPAWN) its handle changes.
   Update every history entry that references the old handle. */
static void history_update_entity(history_t *h, entity_t old_e, entity_t new_e) {
    for (int i = 0; i < h->top; i++) {
        if (h->entries[i].entity == old_e)
            h->entries[i].entity = new_e;
    }
}

static void history_undo(app_t *app) {
    history_t *h = &app->history;
    if (h->pos == 0) return;
    h->pos--;
    cmd_t *c = &h->entries[h->pos];
    switch (c->type) {
    case CMD_SPAWN:
        if (entity_is_alive(app->world, c->entity)) {
            if (app->selected == c->entity) app->selected = ECS_ENTITY_NULL;
            entity_destroy(app->world, c->entity);
        }
        break;
    case CMD_DELETE: {
        entity_t old_e = c->entity;
        entity_t e = spawn(app, c->proto, (vec3_t){0.0f, 0.0f, 0.0f});
        if (e != ECS_ENTITY_NULL) {
            transform_t *t =
                entity_get_component(app->world, e, app->transform_id);
            if (t) {
                t->position = c->xform.position;
                t->rotation = c->xform.rotation;
                t->scale    = c->xform.scale;
            }
            history_update_entity(h, old_e, e);
            app->selected = e;
        }
        break;
    }
    case CMD_TRANSFORM: {
        transform_t *t =
            entity_get_component(app->world, c->entity, app->transform_id);
        if (t) {
            t->position = c->xform.position;
            t->rotation = c->xform.rotation;
            t->scale    = c->xform.scale;
        }
        app->selected = c->entity;
        break;
    }
    }
}

static void history_redo(app_t *app) {
    history_t *h = &app->history;
    if (h->pos == h->top) return;
    cmd_t *c = &h->entries[h->pos];
    h->pos++;
    switch (c->type) {
    case CMD_SPAWN: {
        entity_t old_e = c->entity;
        entity_t e = spawn(app, c->proto, (vec3_t){0.0f, 0.0f, 0.0f});
        if (e != ECS_ENTITY_NULL) {
            transform_t *t =
                entity_get_component(app->world, e, app->transform_id);
            if (t) {
                t->position = c->xform.position;
                t->rotation = c->xform.rotation;
                t->scale    = c->xform.scale;
            }
            history_update_entity(h, old_e, e);
            app->selected = e;
        }
        break;
    }
    case CMD_DELETE:
        if (entity_is_alive(app->world, c->entity)) {
            if (app->selected == c->entity) app->selected = ECS_ENTITY_NULL;
            entity_destroy(app->world, c->entity);
        }
        break;
    case CMD_TRANSFORM: {
        transform_t *t =
            entity_get_component(app->world, c->entity, app->transform_id);
        if (t) {
            t->position = c->xform2.position;
            t->rotation = c->xform2.rotation;
            t->scale    = c->xform2.scale;
        }
        app->selected = c->entity;
        break;
    }
    }
}

/* Drive the transform gizmo for the selected entity. Runs after the scene's
   query has closed, so editing the transform in place is safe. */
static void update_gizmo(app_t *app) {
    app->gizmo_capture = false;
    if (app->selected == ECS_ENTITY_NULL ||
        !entity_is_alive(app->world, app->selected)) {
        return;
    }
    transform_t *t =
        entity_get_component(app->world, app->selected, app->transform_id);
    if (!t) {
        return;
    }

    uint32_t w, h;
    window_size(app->window, &w, &h);
    gizmo_input_t in = {
        .mouse_x = app->mouse_x,
        .mouse_y = app->mouse_y,
        .mouse_down = app->mouse_left,
        /* The gizmo may grab only when neither the camera nor the UI owns the
           mouse. */
        .pointer_valid =
            !camera_is_navigating(&app->camera) && !app->ui_capture,
    };
    bool was_dragging = app->gizmo.dragging;
    saved_xform_t pre = {t->position, t->rotation, t->scale};
    app->gizmo_capture =
        gizmo_update(&app->gizmo, &app->camera, (float)w, (float)h, &in,
                     &t->position, &t->rotation, &t->scale);
    bool now_dragging = app->gizmo.dragging;

    if (!was_dragging && now_dragging)
        app->gizmo_drag_start = pre;

    if (was_dragging && !now_dragging) {
        saved_xform_t post = {t->position, t->rotation, t->scale};
        if (!xforms_equal(&app->gizmo_drag_start, &post))
            history_push(&app->history, (cmd_t){
                .type   = CMD_TRANSFORM,
                .entity = app->selected,
                .xform  = app->gizmo_drag_start,
                .xform2 = post,
            });
    }
}

/* Snapshot the live renderable entities into `out` (so the world isn't mutated
   mid-iteration when the UI then adds/removes). Returns the count. */
static int collect_entities(app_t *app, entity_t *out, int max) {
    int n = 0;
    query_iter_t it = renderable_query(app);
    while (query_next(&it) && n < max) {
        out[n++] = query_entity(&it);
    }
    return n;
}

static prototype_t *prototype_for_mesh(app_t *app, mesh_handle_t mesh) {
    for (int i = 0; i < app->prototype_count; i++) {
        if (app->prototypes[i].mesh == mesh) {
            return &app->prototypes[i];
        }
    }
    return NULL;
}

static const char *selected_name(app_t *app) {
    if (app->selected == ECS_ENTITY_NULL ||
        !entity_is_alive(app->world, app->selected)) {
        return "NONE";
    }
    renderable_t *r =
        entity_get_component(app->world, app->selected, app->renderable_id);
    prototype_t *p = r ? prototype_for_mesh(app, r->mesh) : NULL;
    return p ? p->name : "?";
}

/* Möller-Trumbore, double-sided. Returns the ray parameter t (>0) at the hit.
   The ray need not be unit length; t is then measured in units of |dir|. */
static bool ray_triangle(vec3_t o, vec3_t dir, vec3_t a, vec3_t b, vec3_t c,
                         float *out_t) {
    vec3_t e1 = vec3_sub(b, a);
    vec3_t e2 = vec3_sub(c, a);
    vec3_t pv = vec3_cross(dir, e2);
    float det = vec3_dot(e1, pv);
    if (det > -1e-8f && det < 1e-8f) {
        return false; /* ray parallel to the triangle plane */
    }
    float inv_det = 1.0f / det;
    vec3_t tv = vec3_sub(o, a);
    float u = vec3_dot(tv, pv) * inv_det;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    vec3_t qv = vec3_cross(tv, e1);
    float v = vec3_dot(dir, qv) * inv_det;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    float t = vec3_dot(e2, qv) * inv_det;
    if (t < 0.0f) {
        return false;
    }
    *out_t = t;
    return true;
}


/* Cast a world-space ray against every renderable's triangles and return the
   nearest hit entity, or ECS_ENTITY_NULL if the ray misses everything. The ray
   is pushed into each entity's local space (so the test runs against the stored
   geometry); the local direction is left un-normalized so the returned t stays
   in world units and is comparable across entities. */
static entity_t pick_entity(app_t *app, vec3_t origin, vec3_t dir) {
    entity_t best = ECS_ENTITY_NULL;
    float best_t = 1e30f;
    query_iter_t it = renderable_query(app);
    while (query_next(&it)) {
        transform_t *t = query_get(&it, app->transform_id);
        renderable_t *r = query_get(&it, app->renderable_id);
        prototype_t *p = prototype_for_mesh(app, r->mesh);
        if (!p || !p->pick_pos) {
            continue;
        }
        mat4_t inv =
            mat4_inverse(mat4_from_trs(t->position, t->rotation, t->scale));
        vec3_t lo = mat4_transform_point(inv, origin);
        vec3_t ld = mat4_transform_dir(inv, dir);
        aabb_t pick_box = { p->pick_min, p->pick_max };
        float pick_t;
        if (!aabb_ray_intersect(&pick_box, lo, ld, &pick_t)) {
            continue;
        }
        entity_t e = query_entity(&it);
        for (uint32_t i = 0; i + 2 < p->pick_icount; i += 3) {
            vec3_t a = p->pick_pos[p->pick_idx[i]];
            vec3_t b = p->pick_pos[p->pick_idx[i + 1]];
            vec3_t c = p->pick_pos[p->pick_idx[i + 2]];
            float ht;
            if (ray_triangle(lo, ld, a, b, c, &ht) && ht < best_t) {
                best_t = ht;
                best = e;
            }
        }
    }
    return best;
}

/* Cast a ray through a screen pixel and make the hit entity the selection
   (clicking empty space clears it). */
static void pick_at(app_t *app, float px, float py) {
    uint32_t w, h;
    window_size(app->window, &w, &h);
    float aspect = h ? (float)w / (float)h : 1.0f;
    vec3_t origin, dir;
    camera_ray(&app->camera, aspect, px, py, (float)w, (float)h, &origin,
               &dir);
    app->selected = pick_entity(app, origin, dir);
}

/* Headless check that camera_ray round-trips the projection: project each
   entity's centre to a pixel, cast a ray back through it, and confirm the pick
   lands on that entity. Catches a flipped axis or a bad inverse before any
   click. Runs when KILN_PICK_TEST is set. */
static void run_pick_selftest(app_t *app) {
    uint32_t w, h;
    window_size(app->window, &w, &h);
    float aspect = h ? (float)w / (float)h : 1.0f;

    int total = 0, pass = 0;
    query_iter_t it = renderable_query(app);
    while (query_next(&it)) {
        transform_t *t = query_get(&it, app->transform_id);
        entity_t e = query_entity(&it);
        float sx, sy;
        if (!camera_project(&app->camera, aspect, (float)w, (float)h,
                            t->position, &sx, &sy)) {
            continue;
        }
        vec3_t origin, dir;
        camera_ray(&app->camera, aspect, sx, sy, (float)w, (float)h, &origin,
                   &dir);
        entity_t hit = pick_entity(app, origin, dir);
        total++;
        if (hit == e) {
            pass++;
        } else {
            printf("  PICK MISS: entity %lu at px (%.1f, %.1f) -> %lu\n",
                   (unsigned long)e, (double)sx, (double)sy,
                   (unsigned long)hit);
        }
    }

    /* A corner pixel should hit nothing — exercises the miss / deselect path. */
    vec3_t corner_o, corner_d;
    camera_ray(&app->camera, aspect, 5.0f, 5.0f, (float)w, (float)h, &corner_o,
               &corner_d);
    bool corner_miss = pick_entity(app, corner_o, corner_d) == ECS_ENTITY_NULL;

    printf("PICK selftest: %d/%d entities round-tripped, corner-miss %s\n", pass,
           total, corner_miss ? "OK" : "FAIL");
}

static const char *SCENE_PATH = "scene.kscn";

/* Serialize every renderable entity to a scene file. */
static void scene_do_save(app_t *app) {
    scene_entity_t buf[256];
    int count = 0;

    query_iter_t it = renderable_query(app);
    while (query_next(&it) && count < (int)(sizeof(buf) / sizeof(buf[0]))) {
        transform_t *t = query_get(&it, app->transform_id);
        renderable_t *r = query_get(&it, app->renderable_id);
        prototype_t *p = prototype_for_mesh(app, r->mesh);
        if (!p) {
            continue;
        }
        scene_entity_t *e = &buf[count++];
        strncpy(e->name, p->name, SCENE_NAME_MAX - 1);
        e->name[SCENE_NAME_MAX - 1] = '\0';
        e->position = t->position;
        e->rotation = t->rotation;
        e->scale = t->scale;
    }

    if (scene_save(SCENE_PATH, buf, count)) {
        snprintf(app->scene_status, sizeof(app->scene_status),
                 "SAVED %d ENTITIES", count);
    } else {
        snprintf(app->scene_status, sizeof(app->scene_status), "SAVE FAILED");
    }
}

/* Replace the live scene with the contents of the scene file. */
static void scene_do_load(app_t *app) {
    scene_entity_t buf[256];
    int count = scene_load(SCENE_PATH, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    if (count < 0) {
        snprintf(app->scene_status, sizeof(app->scene_status), "LOAD FAILED");
        return;
    }

    /* Destroy all live entities first. */
    entity_t ents[256];
    int ecount = collect_entities(app, ents, (int)(sizeof(ents) / sizeof(ents[0])));
    for (int i = 0; i < ecount; i++) {
        entity_destroy(app->world, ents[i]);
    }
    app->selected = ECS_ENTITY_NULL;
    app->history = (history_t){0}; /* load replaces the scene; history is stale */

    /* Spawn from file, overriding the default transform with the saved one. */
    int spawned = 0;
    for (int i = 0; i < count; i++) {
        int proto = -1;
        for (int j = 0; j < app->prototype_count; j++) {
            if (strcmp(app->prototypes[j].name, buf[i].name) == 0) {
                proto = j;
                break;
            }
        }
        if (proto < 0) {
            fprintf(stderr, "[scene] unknown prototype '%s', skipping\n",
                    buf[i].name);
            continue;
        }

        entity_t e = spawn(app, proto, (vec3_t){0.0f, 0.0f, 0.0f});
        if (e == ECS_ENTITY_NULL) {
            continue;
        }
        transform_t *t =
            entity_get_component(app->world, e, app->transform_id);
        if (t) {
            t->position = buf[i].position;
            t->rotation = buf[i].rotation;
            t->scale    = buf[i].scale;
        }
        spawned++;
    }

    snprintf(app->scene_status, sizeof(app->scene_status), "LOADED %d ENTITIES",
             spawned);
}

static void app_ui_rect(void *ud, float x, float y, float w, float h,
                        float r, float g, float b, float a) {
    (void)ud; (void)a; render_rect(x, y, w, h, r, g, b);
}
static void app_ui_text(void *ud, float x, float y, float scale,
                        float r, float g, float b, const char *s) {
    (void)ud; render_text(x, y, scale, r, g, b, s);
}
static const ui_draw_t g_kln_ui_draw = {app_ui_rect, app_ui_text, NULL};

/* Build the diagnostics / tamper panel and record whether it owns the mouse. */
static void build_ui(app_t *app) {
    uint32_t w, h;
    window_size(app->window, &w, &h);

    ui_input_t in = {
        .mouse_x = app->mouse_x,
        .mouse_y = app->mouse_y,
        .mouse_down = app->mouse_left,
        /* While orbiting/panning the real cursor is captured and warped, so its
           position is meaningless to the UI — treat it as off-screen. */
        .pointer_valid = !camera_is_navigating(&app->camera),
        .key_pressed = (int)app->pressed_key,
    };

    ui_begin(&app->ui, &in, (float)w, (float)h, &g_kln_ui_draw);
    ui_panel_begin(&app->ui, 12.0f, 12.0f, 300.0f);

    float target_ms = app->fps_limit > 0.0f
                      ? 1000.0f / app->fps_limit : 1000.0f / 60.0f;
    ui_graph(&app->ui, "frame ms",
             app->frame_ms, APP_FRAME_SAMPLES, app->frame_ms_head,
             50.0f, target_ms);
    ui_text(&app->ui, "%.0f fps  draws %u", (double)app->fps, app->draw_count);

    bool prev_vsync = app->vsync;
    ui_checkbox(&app->ui, "vsync", &app->vsync);
    if (app->vsync != prev_vsync) render_set_vsync(app->vsync);
    ui_slider_float(&app->ui, "fps limit", &app->fps_limit, 0.0f, 240.0f);
    bool prev_wire = app->wireframe;
    ui_checkbox(&app->ui, "wireframe", &app->wireframe);
    if (app->wireframe != prev_wire) render_set_wireframe(app->wireframe);
    ui_checkbox(&app->ui, "grid", &app->show_grid);
    if (app->fly_mode) {
        ui_text(&app->ui, "FLY MODE  (TAB to exit)");
        ui_text(&app->ui, "pos  %.1f %.1f %.1f",
                (double)app->fly_pos.x, (double)app->fly_pos.y,
                (double)app->fly_pos.z);
    } else {
        ui_text(&app->ui, "cam  y%.0f p%.0f d%.1f",
                (double)kln_degrees(app->camera.yaw),
                (double)kln_degrees(app->camera.pitch),
                (double)app->camera.distance);
    }

    ui_separator(&app->ui);

    ui_checkbox(&app->ui, "auto rotate", &app->auto_rotate);
    ui_slider_float(&app->ui, "spin", &app->spin_speed, 0.0f, 3.0f);
    ui_slider_float(&app->ui, "bg r", &app->bg_color[0], 0.0f, 1.0f);
    ui_slider_float(&app->ui, "bg g", &app->bg_color[1], 0.0f, 1.0f);
    ui_slider_float(&app->ui, "bg b", &app->bg_color[2], 0.0f, 1.0f);
    if (ui_button(&app->ui, "reset camera")) {
        camera_init(&app->camera);
        app->camera.distance = 16.0f;
        app->camera.pitch = kln_radians(18.0f);
    }

    ui_separator(&app->ui);

    ui_slider_float(&app->ui, "sun yaw",   &app->light_yaw,        0.0f, 360.0f);
    ui_slider_float(&app->ui, "sun pitch", &app->light_pitch,       0.0f,  90.0f);
    ui_slider_float(&app->ui, "light",     &app->light_intensity,   0.0f,   2.0f);
    ui_slider_float(&app->ui, "ambient",   &app->ambient_intensity, 0.0f,   2.0f);

    ui_separator(&app->ui);

    /* --- crude level editor: add / select / remove --- */
    if (app->selected != ECS_ENTITY_NULL &&
        !entity_is_alive(app->world, app->selected)) {
        app->selected = ECS_ENTITY_NULL;
    }
    entity_t ents[256];
    int ecount = collect_entities(app, ents, 256);

    ui_progress(&app->ui, "entities", (float)ecount, 256.0f);
    if (app->prototype_count > 0) {
        ui_text(&app->ui, "spawn: %s",
                app->prototypes[app->sel_prototype].name);
        if (ui_button(&app->ui, "cycle mesh")) {
            app->sel_prototype =
                (app->sel_prototype + 1) % app->prototype_count;
        }
        /* Spawn at the orbit target — pan the camera to place where it lands. */
        if (ui_button(&app->ui, "add at target")) {
            entity_t e = spawn(app, app->sel_prototype, app->camera.target);
            if (e != ECS_ENTITY_NULL) {
                app->selected = e;
                transform_t *t =
                    entity_get_component(app->world, e, app->transform_id);
                history_push(&app->history, (cmd_t){
                    .type  = CMD_SPAWN,
                    .entity = e,
                    .proto = app->sel_prototype,
                    .xform = {t->position, t->rotation, t->scale},
                });
            }
        }
    }

    ui_text(&app->ui, "sel: %s", selected_name(app));
    if (app->selected != ECS_ENTITY_NULL) {
        static const char *modes[] = {"move", "rotate", "scale"};
        char label[32];
        snprintf(label, sizeof(label), "gizmo: %s", modes[app->gizmo.mode]);
        if (ui_button(&app->ui, label)) {
            app->gizmo.mode = (gizmo_mode_t)(((int)app->gizmo.mode + 1) % 3);
        }
    }
    if (ui_button(&app->ui, "select next") && ecount > 0) {
        int cur = -1;
        for (int i = 0; i < ecount; i++) {
            if (ents[i] == app->selected) {
                cur = i;
                break;
            }
        }
        app->selected = ents[(cur + 1) % ecount];
    }
    if (ui_button(&app->ui, "remove") &&
        app->selected != ECS_ENTITY_NULL) {
        entity_t dying = app->selected;
        transform_t *dt = entity_get_component(app->world, dying, app->transform_id);
        renderable_t *dr = entity_get_component(app->world, dying, app->renderable_id);
        if (dt && dr) {
            prototype_t *p = prototype_for_mesh(app, dr->mesh);
            if (p)
                history_push(&app->history, (cmd_t){
                    .type  = CMD_DELETE,
                    .entity = dying,
                    .proto = (int)(p - app->prototypes),
                    .xform = {dt->position, dt->rotation, dt->scale},
                });
        }
        entity_destroy(app->world, dying);
        app->selected = ECS_ENTITY_NULL;
    }

    if (ui_button(&app->ui, "undo (C-z)")) history_undo(app);
    if (ui_button(&app->ui, "redo (C-y)")) history_redo(app);

    ui_separator(&app->ui);

    if (ui_button(&app->ui, "save scene")) { scene_do_save(app); }
    if (ui_button(&app->ui, "load scene")) { scene_do_load(app); }
    if (app->scene_status[0]) {
        ui_text(&app->ui, "%s", app->scene_status);
        app->scene_status[0] = '\0';
    }

    ui_separator(&app->ui);

    /* --- controls (fly key bindings) --- */
    if (app->fly_mode)
        ui_text(&app->ui, "CONTROLS  (click to rebind)");
    else
        ui_text(&app->ui, "CONTROLS  (enter fly mode first)");

    static const struct { const char *action; const char *label; } FLY_ACTIONS[] = {
        {"fly_forward",  "forward"},
        {"fly_backward", "backward"},
        {"fly_left",     "left"},
        {"fly_right",    "right"},
        {"fly_up",       "up"},
        {"fly_down",     "down"},
    };
    for (int i = 0; i < 6; i++) {
        int k = (int)settings_get_key(&app->settings, FLY_ACTIONS[i].action);
        if (ui_keybind(&app->ui, FLY_ACTIONS[i].label, &k, key_code_to_name((keycode_t)k))) {
            settings_set_key(&app->settings, FLY_ACTIONS[i].action, (keycode_t)k);
        }
    }

    ui_panel_end(&app->ui);

    /* --- outliner panel (right side) --- */
    {
        const float OUTLINER_W = 220.0f;
        const int   VISIBLE    = 10;
        float ox = (float)w - OUTLINER_W - 12.0f;

        /* Collect entities fresh (cheap ECS query). */
        entity_t oents[256];
        int oecount = collect_entities(app, oents, 256);

        /* Apply wheel scroll and clamp. */
        app->outliner_scroll += app->outliner_pending_scroll;
        app->outliner_pending_scroll = 0;
        int max_scroll = oecount - VISIBLE;
        if (max_scroll < 0) max_scroll = 0;
        if (app->outliner_scroll < 0) app->outliner_scroll = 0;
        if (app->outliner_scroll > max_scroll) app->outliner_scroll = max_scroll;

        /* Auto-scroll the selected entity into view. */
        if (app->selected != ECS_ENTITY_NULL) {
            for (int i = 0; i < oecount; i++) {
                if (oents[i] == app->selected) {
                    if (i < app->outliner_scroll)
                        app->outliner_scroll = i;
                    if (i >= app->outliner_scroll + VISIBLE)
                        app->outliner_scroll = i - VISIBLE + 1;
                    break;
                }
            }
        }

        ui_panel_begin(&app->ui, ox, 12.0f, OUTLINER_W);
        ui_text(&app->ui, "OUTLINER  %d", oecount);
        ui_separator(&app->ui);

        int vis_end = app->outliner_scroll + VISIBLE;
        if (vis_end > oecount) vis_end = oecount;

        for (int i = app->outliner_scroll; i < vis_end; i++) {
            entity_t e = oents[i];
            renderable_t *r =
                entity_get_component(app->world, e, app->renderable_id);
            prototype_t *p = r ? prototype_for_mesh(app, r->mesh) : NULL;
            char row[64];
            snprintf(row, sizeof(row), "%s  #%d", p ? p->name : "?", i);
            if (ui_selectable(&app->ui, row, e == app->selected))
                app->selected = e;
        }

        if (oecount > VISIBLE) {
            ui_separator(&app->ui);
            ui_text(&app->ui, "%d-%d / %d  (scroll)",
                    app->outliner_scroll + 1, vis_end, oecount);
        }

        ui_panel_end(&app->ui);
    }

    /* --- properties panel (below the outliner) --- */
    if (app->selected != ECS_ENTITY_NULL &&
        entity_is_alive(app->world, app->selected)) {
        transform_t *t =
            entity_get_component(app->world, app->selected, app->transform_id);
        if (t) {
            const float PROP_W  = 220.0f;
            const int   OUTLINER_IDX = 1; /* outliner is the second panel (index 1) */
            float px = (float)w - PROP_W - 12.0f;
            float py = 12.0f + app->ui.panel_height[OUTLINER_IDX] + 8.0f;

            saved_xform_t before = {t->position, t->rotation, t->scale};
            bool any = false;

            ui_panel_begin(&app->ui, px, py, PROP_W);
            ui_text(&app->ui, "TRANSFORM  %s", selected_name(app));
            ui_separator(&app->ui);

            any |= ui_drag_float(&app->ui, "pos X", &t->position.x, 0.02f);
            any |= ui_drag_float(&app->ui, "pos Y", &t->position.y, 0.02f);
            any |= ui_drag_float(&app->ui, "pos Z", &t->position.z, 0.02f);
            ui_separator(&app->ui);

            vec3_t eu = quat_to_euler_deg(t->rotation);
            bool rot = false;
            rot |= ui_drag_float(&app->ui, "rot X", &eu.x, 0.5f);
            rot |= ui_drag_float(&app->ui, "rot Y", &eu.y, 0.5f);
            rot |= ui_drag_float(&app->ui, "rot Z", &eu.z, 0.5f);
            if (rot) t->rotation = euler_deg_to_quat(eu);
            any |= rot;
            ui_separator(&app->ui);

            any |= ui_drag_float(&app->ui, "scl X", &t->scale.x, 0.005f);
            any |= ui_drag_float(&app->ui, "scl Y", &t->scale.y, 0.005f);
            any |= ui_drag_float(&app->ui, "scl Z", &t->scale.z, 0.005f);

            ui_panel_end(&app->ui);

            /* Gesture-based history: snapshot on first motion, commit on release. */
            if (any && !app->prop_editing) {
                app->prop_editing   = true;
                app->prop_edit_start = before;
            }
            if (app->prop_editing && app->ui.went_up) {
                saved_xform_t after = {t->position, t->rotation, t->scale};
                if (!xforms_equal(&app->prop_edit_start, &after))
                    history_push(&app->history, (cmd_t){
                        .type   = CMD_TRANSFORM,
                        .entity = app->selected,
                        .xform  = app->prop_edit_start,
                        .xform2 = after,
                    });
                app->prop_editing = false;
            }
        }
    }

    ui_end(&app->ui);

    app->ui_capture = ui_wants_mouse(&app->ui);
    render_set_clear_color(app->bg_color[0], app->bg_color[1],
                           app->bg_color[2]);
}

void app_run(app_t *app) {
    /* KILN_MAX_FRAMES=N runs N frames then exits cleanly — lets a headless or
       bounded run capture validation output without a hanging window. */
    const char *cap = getenv("KILN_MAX_FRAMES");
    uint64_t max_frames = cap ? strtoull(cap, NULL, 10) : 0;
    uint64_t frames = 0;

    if (getenv("KILN_PICK_TEST")) {
        run_pick_selftest(app);
    }

    kln_timer_t frame_timer;
    kln_timer_init(&frame_timer);

    bool running = true;
    while (running) {
        app->pressed_key = KEY_UNKNOWN;
        event_t event;
        while (window_poll_event(app->window, &event)) {
            if (event.type == EVENT_QUIT ||
                (event.type == EVENT_KEY_DOWN &&
                 event.key.code == KEY_ESCAPE)) {
                running = false;
                continue;
            }

            /* Ctrl+Z undo / Ctrl+Y redo / Ctrl+D duplicate. */
            if (event.type == EVENT_KEY_DOWN && event.key.ctrl) {
                if (event.key.code == KEY_Z) { history_undo(app); continue; }
                if (event.key.code == KEY_Y) { history_redo(app); continue; }
                if (event.key.code == KEY_D &&
                    app->selected != ECS_ENTITY_NULL &&
                    entity_is_alive(app->world, app->selected)) {
                    renderable_t *dr =
                        entity_get_component(app->world, app->selected,
                                             app->renderable_id);
                    transform_t *dt =
                        entity_get_component(app->world, app->selected,
                                             app->transform_id);
                    if (dr && dt) {
                        prototype_t *dp = prototype_for_mesh(app, dr->mesh);
                        int pidx = dp ? (int)(dp - app->prototypes) : -1;
                        if (pidx >= 0) {
                            /* Copy before spawn — ECS may reallocate on entity_create. */
                            vec3_t dpos = dt->position;
                            quat_t drot = dt->rotation;
                            vec3_t dscl = dt->scale;
                            vec3_t at = {dpos.x + 0.5f, dpos.y, dpos.z + 0.5f};
                            entity_t ne = spawn(app, pidx, at);
                            if (ne != ECS_ENTITY_NULL) {
                                transform_t *nt = entity_get_component(
                                    app->world, ne, app->transform_id);
                                if (nt) {
                                    nt->rotation = drot;
                                    nt->scale    = dscl;
                                    history_push(&app->history, (cmd_t){
                                        .type   = CMD_SPAWN,
                                        .entity = ne,
                                        .proto  = pidx,
                                        .xform  = {nt->position, drot, dscl},
                                    });
                                }
                                app->selected = ne;
                            }
                        }
                    }
                    continue;
                }
            }

            /* F focuses the orbit camera on the selected entity. */
            if (event.type == EVENT_KEY_DOWN && event.key.code == KEY_F &&
                !app->fly_mode &&
                app->selected != ECS_ENTITY_NULL &&
                entity_is_alive(app->world, app->selected)) {
                transform_t *ft =
                    entity_get_component(app->world, app->selected,
                                         app->transform_id);
                if (ft) {
                    app->camera.target = ft->position;
                    float size = ft->scale.x;
                    if (ft->scale.y > size) size = ft->scale.y;
                    if (ft->scale.z > size) size = ft->scale.z;
                    float d = size * 4.0f;
                    if (d < 0.5f)   d = 0.5f;
                    if (d > 100.0f) d = 100.0f;
                    app->camera.distance = d;
                }
                continue;
            }

            /* TAB toggles fly mode; sync position and look direction on entry. */
            if (event.type == EVENT_KEY_DOWN && event.key.code == KEY_TAB) {
                app->fly_mode = !app->fly_mode;
                if (app->fly_mode) {
                    app->fly_pos = camera_eye(&app->camera);
                    app->fly_cam.pitch = -app->camera.pitch;
                    app->fly_cam.yaw   = atan2f(-cosf(app->camera.yaw),
                                                -sinf(app->camera.yaw));
                    fps_camera_rotate(&app->fly_cam, 0.0f, 0.0f);
                    app->gizmo_capture = false;
                }
                continue;
            }

            /* Track held keys for fly-mode WASD. */
            if (event.type == EVENT_KEY_DOWN && event.key.code < KEY_COUNT) {
                app->fly_keys[event.key.code] = true;
                /* Record for keybind widget — take the first non-modifier key. */
                if (app->pressed_key == KEY_UNKNOWN)
                    app->pressed_key = event.key.code;
            }
            if (event.type == EVENT_KEY_UP && event.key.code < KEY_COUNT)
                app->fly_keys[event.key.code] = false;

            /* Track pointer state for the UI regardless of who consumes it. */
            switch (event.type) {
            case EVENT_MOUSE_MOTION:
                app->mouse_x = (float)event.motion.x;
                app->mouse_y = (float)event.motion.y;
                /* Accumulate motion via deltas (valid even under cursor capture)
                   so a click can be told apart from an orbit drag. */
                if (app->pick_armed) {
                    app->pick_drag += (float)(abs(event.motion.dx) +
                                              abs(event.motion.dy));
                }
                break;
            case EVENT_MOUSE_BUTTON:
                if (event.button.button == MOUSE_BUTTON_LEFT) {
                    if (event.button.down) {
                        app->mouse_left = true;
                        /* Arm a pick only when nothing else owns the mouse; record
                           the press pixel while the cursor is still un-warped. */
                        if (!app->ui_capture && !app->gizmo_capture) {
                            app->pick_armed = true;
                            app->pick_down_x = app->mouse_x;
                            app->pick_down_y = app->mouse_y;
                            app->pick_drag = 0.0f;
                        }
                    } else {
                        app->mouse_left = false;
                        /* A near-stationary release that nothing else grabbed is a
                           selection click. */
                        if (app->pick_armed && app->pick_drag < 5.0f &&
                            !app->ui_capture && !app->gizmo_capture) {
                            app->pick_request = true;
                        }
                        app->pick_armed = false;
                    }
                }
                break;
            default:
                break;
            }

            /* Scroll over the right-side outliner panel goes to the outliner. */
            if (event.type == EVENT_SCROLL && app->ui_capture) {
                uint32_t scroll_w, scroll_h;
                window_size(app->window, &scroll_w, &scroll_h);
                if (app->mouse_x > (float)scroll_w * 0.5f)
                    app->outliner_pending_scroll +=
                        (event.scroll.delta > 0.0f) ? -1 : 1;
            }

            /* In fly mode feed mouse deltas to the fps camera; in orbit mode
               pass events to the orbit camera (gated by UI/gizmo ownership). */
            bool mouse_ev = event.type == EVENT_MOUSE_MOTION ||
                            event.type == EVENT_MOUSE_BUTTON ||
                            event.type == EVENT_SCROLL;
            if (app->fly_mode) {
                if (event.type == EVENT_MOUSE_MOTION)
                    fps_camera_rotate(&app->fly_cam,
                                      (float)event.motion.dx,
                                      (float)event.motion.dy);
            } else if (!(mouse_ev && (app->ui_capture || app->gizmo_capture))) {
                camera_handle_event(&app->camera, &event);
            }
        }

        /* Fly mode always captures; orbit captures while dragging. */
        window_set_cursor_mode(app->window,
                               (app->fly_mode ||
                                camera_is_navigating(&app->camera))
                                   ? CURSOR_DISABLED
                                   : CURSOR_NORMAL);

        float dt = (float)kln_timer_reset(&frame_timer);
        if (dt > 0.0f) {
            float inst = 1.0f / dt;
            app->fps = (app->fps > 0.0f) ? app->fps * 0.9f + inst * 0.1f : inst;
            app->frame_ms[app->frame_ms_head] = dt * 1000.0f;
            app->frame_ms_head = (app->frame_ms_head + 1) % APP_FRAME_SAMPLES;
        }

        /* WASD+QE fly movement (keys resolved from settings). */
        if (app->fly_mode && dt > 0.0f) {
            const float speed = 5.0f;
            vec3_t right, up_fly;
            fps_camera_basis(&app->fly_cam, &right, &up_fly);
            vec3_t front = app->fly_cam.front;
            float  d = speed * dt;
            settings_t *s = &app->settings;
#define FLY(action, v, scale) \
    do { keycode_t k = settings_get_key(s, action); \
         if (k != KEY_UNKNOWN && app->fly_keys[k]) \
             app->fly_pos = vec3_add(app->fly_pos, vec3_scale(v, scale)); \
    } while (0)
            FLY("fly_forward",  front,   d);
            FLY("fly_backward", front,  -d);
            FLY("fly_left",     right,  -d);
            FLY("fly_right",    right,   d);
            FLY("fly_down",     up_fly, -d);
            FLY("fly_up",       up_fly,  d);
#undef FLY
        }

        if (app->auto_rotate) {
            rotate_system(app, dt);
        }

        if (app->pick_request) {
            app->pick_request = false;
            if (!app->fly_mode)
                pick_at(app, app->pick_down_x, app->pick_down_y);
        }

        render_scene(app);
        if (app->show_grid) draw_grid(app);
        if (!app->fly_mode)
            update_gizmo(app);
        build_ui(app);

        /* GPU particles: spray a burst of cubes from above the scene origin. */
        if (app->particle_emitter != RENDER_GPU_EMITTER_INVALID && dt > 0.0f) {
            static unsigned rng = 1;
            for (int k = 0; k < 4; k++) {
                rng = rng * 1664525u + 1013904223u;
                float rx = ((float)(rng & 0xFFFF) / 65535.0f) * 2.0f - 1.0f;
                rng = rng * 1664525u + 1013904223u;
                float rz = ((float)(rng & 0xFFFF) / 65535.0f) * 2.0f - 1.0f;
                rng = rng * 1664525u + 1013904223u;
                float vy = ((float)(rng & 0xFFFF) / 65535.0f) * 3.0f + 2.0f;
                render_gpu_emitter_emit(app->particle_emitter,
                    (vec3_t){rx * 3.0f, 6.0f, rz * 3.0f},
                    (vec3_t){rx * 0.5f, vy,   rz * 0.5f},
                    2.5f, 0.15f);
            }
            render_gpu_emitter_update(app->particle_emitter, dt);
        }

        render_draw();

        if (app->fps_limit > 0.0f) {
            double target  = 1.0 / (double)app->fps_limit;
            double elapsed = kln_timer_elapsed(&frame_timer);
            if (elapsed < target) kln_timer_sleep(target - elapsed);
        }

        ++frames;
        if (max_frames && frames == max_frames - 1) {
            /* Take a screenshot on second-to-last frame for verification. */
            const char *ss = getenv("KILN_SCREENSHOT");
            if (ss) render_save_screenshot(ss);
        }
        if (max_frames && frames >= max_frames) {
            running = false;
        }
    }
}

void app_shutdown(app_t *app) {
    for (int i = 0; i < app->prototype_count; i++) {
        free(app->prototypes[i].pick_pos);
        free(app->prototypes[i].pick_idx);
    }
    if (app->particle_emitter != RENDER_GPU_EMITTER_INVALID)
        render_destroy_gpu_emitter(app->particle_emitter);

    /* Persist live graphical state so the next launch picks it up. */
    uint32_t sw, sh;
    window_size(app->window, &sw, &sh);
    app->settings.engine.width     = sw;
    app->settings.engine.height    = sh;
    app->settings.engine.vsync     = app->vsync;
    app->settings.engine.fps_limit = app->fps_limit;
    settings_save(&app->settings, SETTINGS_PATH);

    render_shutdown();
    window_destroy(app->window);
    world_destroy(app->world);
}
