#include "app.h"

#include "camera.h"
#include "core.h"
#include "gizmo.h"
#include "image.h"
#include "linalg.h"
#include "mesh.h"
#include "obj.h"
#include "render.h"
#include "ui.h"
#include "assets.h"
#include "scene.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    if (!obj_load(path, &m)) {
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

    app->world = world_create();
    app->transform_id = component_register(app->world, "transform",
                                           sizeof(transform_t),
                                           _Alignof(transform_t));
    app->renderable_id =
        component_register(app->world, "renderable", sizeof(renderable_t),
                           _Alignof(renderable_t));

    app->window = window_create("Kiln", 1280, 720);
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
    app->bg_color[0] = 0.02f;
    app->bg_color[1] = 0.02f;
    app->bg_color[2] = 0.05f;

    build_scene(app); /* may set an initial selection */

    camera_init(&app->camera);
    app->camera.distance = 16.0f;
    app->camera.pitch = kln_radians(18.0f);

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

    render_set_camera(camera_view(&app->camera),
                      camera_proj(&app->camera, aspect));

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
    app->gizmo_capture =
        gizmo_update(&app->gizmo, &app->camera, (float)w, (float)h, &in,
                     &t->position, &t->rotation, &t->scale);
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

/* Slab test: does the ray reach the AABB at some t >= 0? */
static bool ray_aabb(vec3_t o, vec3_t dir, vec3_t mn, vec3_t mx) {
    float ro[3] = {o.x, o.y, o.z};
    float rd[3] = {dir.x, dir.y, dir.z};
    float lo[3] = {mn.x, mn.y, mn.z};
    float hi[3] = {mx.x, mx.y, mx.z};
    float tmin = 0.0f, tmax = 1e30f;
    for (int i = 0; i < 3; i++) {
        if (rd[i] > -1e-8f && rd[i] < 1e-8f) {
            if (ro[i] < lo[i] || ro[i] > hi[i]) {
                return false; /* parallel and outside the slab */
            }
        } else {
            float inv = 1.0f / rd[i];
            float t1 = (lo[i] - ro[i]) * inv;
            float t2 = (hi[i] - ro[i]) * inv;
            if (t1 > t2) {
                float tmp = t1;
                t1 = t2;
                t2 = tmp;
            }
            tmin = t1 > tmin ? t1 : tmin;
            tmax = t2 < tmax ? t2 : tmax;
            if (tmin > tmax) {
                return false;
            }
        }
    }
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
        if (!ray_aabb(lo, ld, p->pick_min, p->pick_max)) {
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

static const char *SCENE_PATH = "scene.kln";

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
    };

    ui_begin(&app->ui, &in, (float)w, (float)h);
    ui_panel_begin(&app->ui, 12.0f, 12.0f, 300.0f);

    ui_text(&app->ui, "KILN  %.0f FPS  %.2f MS", (double)app->fps,
            app->fps > 0.0f ? 1000.0 / (double)app->fps : 0.0);
    ui_text(&app->ui, "DRAWS %u", app->draw_count);
    ui_text(&app->ui, "CAM  Y%.0f P%.0f D%.1f",
            (double)kln_degrees(app->camera.yaw),
            (double)kln_degrees(app->camera.pitch),
            (double)app->camera.distance);

    ui_checkbox(&app->ui, "AUTO ROTATE", &app->auto_rotate);
    ui_slider_float(&app->ui, "SPIN", &app->spin_speed, 0.0f, 3.0f);

    ui_slider_float(&app->ui, "BG R", &app->bg_color[0], 0.0f, 1.0f);
    ui_slider_float(&app->ui, "BG G", &app->bg_color[1], 0.0f, 1.0f);
    ui_slider_float(&app->ui, "BG B", &app->bg_color[2], 0.0f, 1.0f);

    if (ui_button(&app->ui, "RESET CAMERA")) {
        camera_init(&app->camera);
        app->camera.distance = 16.0f;
        app->camera.pitch = kln_radians(18.0f);
    }

    /* --- crude level editor: add / select / remove --- */
    if (app->selected != ECS_ENTITY_NULL &&
        !entity_is_alive(app->world, app->selected)) {
        app->selected = ECS_ENTITY_NULL;
    }
    entity_t ents[256];
    int ecount = collect_entities(app, ents, 256);

    ui_text(&app->ui, "ENTITIES %d", ecount);
    if (app->prototype_count > 0) {
        ui_text(&app->ui, "SPAWN: %s",
                app->prototypes[app->sel_prototype].name);
        if (ui_button(&app->ui, "CYCLE MESH")) {
            app->sel_prototype =
                (app->sel_prototype + 1) % app->prototype_count;
        }
        /* Spawn at the orbit target — pan the camera to place where it lands. */
        if (ui_button(&app->ui, "ADD AT TARGET")) {
            app->selected = spawn(app, app->sel_prototype, app->camera.target);
        }
    }

    ui_text(&app->ui, "SEL: %s", selected_name(app));
    if (app->selected != ECS_ENTITY_NULL) {
        static const char *modes[] = {"MOVE", "ROTATE", "SCALE"};
        char label[32];
        snprintf(label, sizeof(label), "GIZMO: %s", modes[app->gizmo.mode]);
        if (ui_button(&app->ui, label)) {
            app->gizmo.mode = (gizmo_mode_t)(((int)app->gizmo.mode + 1) % 3);
        }
    }
    if (ui_button(&app->ui, "SELECT NEXT") && ecount > 0) {
        int cur = -1;
        for (int i = 0; i < ecount; i++) {
            if (ents[i] == app->selected) {
                cur = i;
                break;
            }
        }
        app->selected = ents[(cur + 1) % ecount];
    }
    if (ui_button(&app->ui, "REMOVE") &&
        app->selected != ECS_ENTITY_NULL) {
        entity_destroy(app->world, app->selected);
        app->selected = ECS_ENTITY_NULL;
    }

    /* --- scene persistence --- */
    if (ui_button(&app->ui, "SAVE SCENE")) {
        scene_do_save(app);
    }
    if (ui_button(&app->ui, "LOAD SCENE")) {
        scene_do_load(app);
    }
    if (app->scene_status[0]) {
        ui_text(&app->ui, "%s", app->scene_status);
    }

    ui_panel_end(&app->ui);
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

    struct timespec prev;
    clock_gettime(CLOCK_MONOTONIC, &prev);

    bool running = true;
    while (running) {
        event_t event;
        while (window_poll_event(app->window, &event)) {
            if (event.type == EVENT_QUIT ||
                (event.type == EVENT_KEY_DOWN &&
                 event.key.code == KEY_ESCAPE)) {
                running = false;
                continue;
            }

            /* Track pointer state for the UI regardless of who consumes it. */
            switch (event.type) {
            case EVENT_MOUSE_MOVE:
                app->mouse_x = (float)event.motion.x;
                app->mouse_y = (float)event.motion.y;
                /* Accumulate motion via deltas (valid even under cursor capture)
                   so a click can be told apart from an orbit drag. */
                if (app->pick_armed) {
                    app->pick_drag += (float)(abs(event.motion.dx) +
                                              abs(event.motion.dy));
                }
                break;
            case EVENT_BUTTON_DOWN:
                if (event.button.button == MOUSE_BUTTON_LEFT) {
                    app->mouse_left = true;
                    /* Arm a pick only when nothing else owns the mouse; record
                       the press pixel while the cursor is still un-warped. */
                    if (!app->ui_capture && !app->gizmo_capture) {
                        app->pick_armed = true;
                        app->pick_down_x = app->mouse_x;
                        app->pick_down_y = app->mouse_y;
                        app->pick_drag = 0.0f;
                    }
                }
                break;
            case EVENT_BUTTON_UP:
                if (event.button.button == MOUSE_BUTTON_LEFT) {
                    app->mouse_left = false;
                    /* A near-stationary release that nothing else grabbed is a
                       selection click. */
                    if (app->pick_armed && app->pick_drag < 5.0f &&
                        !app->ui_capture && !app->gizmo_capture) {
                        app->pick_request = true;
                    }
                    app->pick_armed = false;
                }
                break;
            default:
                break;
            }

            /* The camera gets mouse events only when neither the UI nor the
               gizmo owned the mouse last frame; non-mouse events always pass
               through. */
            bool mouse_ev = event.type == EVENT_MOUSE_MOVE ||
                            event.type == EVENT_BUTTON_DOWN ||
                            event.type == EVENT_BUTTON_UP ||
                            event.type == EVENT_SCROLL;
            if (!(mouse_ev && (app->ui_capture || app->gizmo_capture))) {
                camera_handle_event(&app->camera, &event);
            }
        }

        /* Capture the cursor while dragging so orbit/pan can run unbounded and
           never slip off the window; release it the moment the drag ends. */
        window_set_cursor_mode(app->window,
                               camera_is_navigating(&app->camera)
                                   ? CURSOR_DISABLED
                                   : CURSOR_NORMAL);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        float dt = (float)(now.tv_sec - prev.tv_sec) +
                   (float)(now.tv_nsec - prev.tv_nsec) * 1e-9f;
        prev = now;
        if (dt > 0.0f) {
            float inst = 1.0f / dt;
            app->fps = (app->fps > 0.0f) ? app->fps * 0.9f + inst * 0.1f : inst;
        }

        if (app->auto_rotate) {
            rotate_system(app, dt);
        }

        if (app->pick_request) {
            app->pick_request = false;
            pick_at(app, app->pick_down_x, app->pick_down_y);
        }

        render_scene(app);
        update_gizmo(app);
        build_ui(app);
        render_draw();

        if (max_frames && ++frames >= max_frames) {
            running = false;
        }
    }
}

void app_shutdown(app_t *app) {
    for (int i = 0; i < app->prototype_count; i++) {
        free(app->prototypes[i].pick_pos);
        free(app->prototypes[i].pick_idx);
    }
    render_shutdown();
    window_destroy(app->window);
    world_destroy(app->world);
}
