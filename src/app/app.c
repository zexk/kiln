#include "app.h"

#include "camera.h"
#include "core.h"
#include "linalg.h"
#include "mesh.h"
#include "obj.h"
#include "render.h"
#include "assets.h"

#include <stdint.h>
#include <stdlib.h>

#define MODEL_PATH(name) KILN_ASSET_DIR "/models/" name

/* Spawn an entity for an already-uploaded mesh, sized and centred so its
   longest axis spans ~2 units and it sits at slot_x on the X axis — lets test
   models of wildly different native scales line up comparably. */
static void place_mesh(app_t *app, mesh_handle_t mesh, vec3_t bmin, vec3_t bmax,
                       float slot_x) {
    if (mesh == RENDER_MESH_INVALID) {
        return;
    }
    vec3_t center = vec3_scale(vec3_add(bmin, bmax), 0.5f);
    vec3_t extent = vec3_sub(bmax, bmin);
    float max_dim = extent.x;
    if (extent.y > max_dim) {
        max_dim = extent.y;
    }
    if (extent.z > max_dim) {
        max_dim = extent.z;
    }
    float s = (max_dim > 1e-6f) ? 2.0f / max_dim : 1.0f;

    entity_t e = entity_create(app->world);
    transform_t *t = entity_add_component(app->world, e, app->transform_id);
    t->rotation = quat_identity();
    t->scale = (vec3_t){s, s, s};
    /* position = slot - s*center, so scale*v + position recentres the model. */
    t->position =
        (vec3_t){slot_x - s * center.x, -s * center.y, -s * center.z};

    mesh_handle_t *mh = entity_add_component(app->world, e, app->mesh_id);
    *mh = mesh;
}

static void place_cpu_mesh(app_t *app, cpu_mesh_t *m, float slot_x) {
    vec3_t bmin, bmax;
    if (!cpu_mesh_bounds(m, &bmin, &bmax)) {
        return;
    }
    place_mesh(app, render_upload_mesh(m), bmin, bmax, slot_x);
}

static void load_model(app_t *app, const char *path, float slot_x) {
    cpu_mesh_t m;
    if (!obj_load(path, &m)) {
        return;
    }
    place_cpu_mesh(app, &m, slot_x);
    cpu_mesh_free(&m);
}

/* The scene: a procedural cube plus the OBJ test subjects, in a row. Requires
   the renderer to be up (meshes are uploaded immediately). */
static void build_scene(app_t *app) {
    const char *models[] = {
        MODEL_PATH("cow.obj"),    MODEL_PATH("spot.obj"),
        MODEL_PATH("fandisk.obj"), MODEL_PATH("cheburashka.obj"),
        MODEL_PATH("armadillo.obj"),
    };
    const int model_count = (int)(sizeof(models) / sizeof(models[0]));
    const float spacing = 2.8f;
    const int slots = model_count + 1; /* +1 for the procedural cube */
    float x0 = -spacing * (float)(slots - 1) * 0.5f;

    cpu_mesh_t cube;
    if (cpu_mesh_cube(&cube)) {
        place_cpu_mesh(app, &cube, x0);
        cpu_mesh_free(&cube);
    }
    for (int i = 0; i < model_count; i++) {
        load_model(app, models[i], x0 + spacing * (float)(i + 1));
    }
}

bool app_init(app_t *app) {
    core_init();

    app->world = world_create();
    app->transform_id = component_register(app->world, "transform",
                                           sizeof(transform_t),
                                           _Alignof(transform_t));
    app->mesh_id = component_register(app->world, "mesh", sizeof(mesh_handle_t),
                                      _Alignof(mesh_handle_t));

    app->window = window_create("Kiln", 1280, 720);
    if (!app->window) {
        return false;
    }
    if (!render_init(app->window)) {
        return false;
    }
    assets_init();

    build_scene(app);

    camera_init(&app->camera);
    app->camera.distance = 16.0f;
    app->camera.pitch = kln_radians(18.0f);

    return true;
}

static query_iter_t renderable_query(app_t *app) {
    signature_t require;
    signature_clear(&require);
    signature_set(&require, app->transform_id);
    signature_set(&require, app->mesh_id);
    return query_iter(app->world, (query_desc_t){.require = require});
}

/* Aim the camera, then queue one draw per renderable entity. */
static void render_scene(app_t *app) {
    uint32_t w, h;
    window_size(app->window, &w, &h);
    float aspect = h ? (float)w / (float)h : 1.0f;

    render_set_camera(camera_view(&app->camera),
                      camera_proj(&app->camera, aspect));

    query_iter_t it = renderable_query(app);
    while (query_next(&it)) {
        transform_t *t = query_get(&it, app->transform_id);
        mesh_handle_t *mesh = query_get(&it, app->mesh_id);
        render_mesh(*mesh, mat4_from_trs(t->position, t->rotation, t->scale));
    }
}

void app_run(app_t *app) {
    /* KILN_MAX_FRAMES=N runs N frames then exits cleanly — lets a headless or
       bounded run capture validation output without a hanging window. */
    const char *cap = getenv("KILN_MAX_FRAMES");
    uint64_t max_frames = cap ? strtoull(cap, NULL, 10) : 0;
    uint64_t frames = 0;

    bool running = true;
    while (running) {
        event_t event;
        while (window_poll_event(app->window, &event)) {
            switch (event.type) {
            case EVENT_QUIT:
                running = false;
                break;
            case EVENT_KEY_DOWN:
                if (event.key.code == KEY_ESCAPE) {
                    running = false;
                }
                break;
            default:
                camera_handle_event(&app->camera, &event);
                break;
            }
        }

        /* Capture the cursor while dragging so orbit/pan can run unbounded and
           never slip off the window; release it the moment the drag ends. */
        window_set_cursor_mode(app->window,
                               camera_is_navigating(&app->camera)
                                   ? CURSOR_DISABLED
                                   : CURSOR_NORMAL);

        render_scene(app);

        render_text(16.0f, 16.0f, 3.0f, 0.9f, 0.9f, 0.2f, "KILN DEBUG");
        render_text(16.0f, 48.0f, 2.0f, 0.6f, 0.8f, 1.0f,
                    "LMB ORBIT  RMB PAN  WHEEL ZOOM");
        render_draw();

        if (max_frames && ++frames >= max_frames) {
            running = false;
        }
    }
}

void app_shutdown(app_t *app) {
    render_shutdown();
    window_destroy(app->window);
    world_destroy(app->world);
}
