#include "app.h"

#include "camera.h"
#include "core.h"
#include "image.h"
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
static void place_mesh(app_t *app, mesh_handle_t mesh, material_handle_t mat,
                       vec3_t bmin, vec3_t bmax, float slot_x) {
    if (mesh == RENDER_MESH_INVALID || mat == RENDER_MATERIAL_INVALID) {
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

    renderable_t *r = entity_add_component(app->world, e, app->renderable_id);
    r->mesh = mesh;
    r->material = mat;
}

static void place_cpu_mesh(app_t *app, cpu_mesh_t *m, material_handle_t mat,
                           float slot_x) {
    vec3_t bmin, bmax;
    if (!cpu_mesh_bounds(m, &bmin, &bmax)) {
        return;
    }
    place_mesh(app, render_upload_mesh(m), mat, bmin, bmax, slot_x);
}

static void load_model(app_t *app, const char *path, material_handle_t mat,
                       float slot_x) {
    cpu_mesh_t m;
    if (!obj_load(path, &m)) {
        return;
    }
    place_cpu_mesh(app, &m, mat, slot_x);
    cpu_mesh_free(&m);
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

/* The scene: a procedural cube plus the OBJ test subjects in a row, each with
   its own flat-colour material. Requires the renderer to be up (meshes and
   materials are created immediately). */
static void build_scene(app_t *app) {
    /* spot ships with a texture; the rest are flat-coloured. Fall back to a
       solid colour if the image can't be loaded. */
    material_handle_t spot_mat = textured(MODEL_PATH("spot.png"));
    if (spot_mat == RENDER_MATERIAL_INVALID) {
        spot_mat = solid(0.90f, 0.85f, 0.80f);
    }

    struct {
        const char *path; /* NULL = procedural cube */
        material_handle_t mat;
    } items[] = {
        {NULL, solid(0.85f, 0.55f, 0.25f)},
        {MODEL_PATH("cow.obj"), solid(0.80f, 0.30f, 0.30f)},
        {MODEL_PATH("spot.obj"), spot_mat},
        {MODEL_PATH("fandisk.obj"), solid(0.45f, 0.65f, 0.85f)},
        {MODEL_PATH("cheburashka.obj"), solid(0.55f, 0.45f, 0.70f)},
        {MODEL_PATH("armadillo.obj"), solid(0.55f, 0.75f, 0.45f)},
    };
    const int count = (int)(sizeof(items) / sizeof(items[0]));
    const float spacing = 2.8f;
    float x0 = -spacing * (float)(count - 1) * 0.5f;

    for (int i = 0; i < count; i++) {
        float x = x0 + spacing * (float)i;
        if (items[i].path) {
            load_model(app, items[i].path, items[i].mat, x);
        } else {
            cpu_mesh_t cube;
            if (cpu_mesh_cube(&cube)) {
                place_cpu_mesh(app, &cube, items[i].mat, x);
                cpu_mesh_free(&cube);
            }
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
    signature_set(&require, app->renderable_id);
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
        renderable_t *r = query_get(&it, app->renderable_id);
        render_mesh(r->mesh, r->material,
                    mat4_from_trs(t->position, t->rotation, t->scale));
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
