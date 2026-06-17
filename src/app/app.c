#include "app.h"

#include "camera.h"
#include "core.h"
#include "image.h"
#include "linalg.h"
#include "mesh.h"
#include "obj.h"
#include "render.h"
#include "ui.h"
#include "assets.h"

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define MODEL_PATH(name) KILN_ASSET_DIR "/models/" name

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

/* Register a spawnable prototype from a CPU mesh: upload it, record the
   normalize-to-~2-units scale and local centre. Returns its index or -1. */
static int add_prototype(app_t *app, const char *name, cpu_mesh_t *m,
                         material_handle_t mat) {
    vec3_t bmin, bmax;
    mesh_handle_t mesh = render_upload_mesh(m);
    if (app->prototype_count >= APP_MAX_PROTOTYPES ||
        mesh == RENDER_MESH_INVALID || mat == RENDER_MATERIAL_INVALID ||
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

    prototype_t *p = &app->prototypes[app->prototype_count];
    p->name = name;
    p->mesh = mesh;
    p->material = mat;
    p->scale = (max_dim > 1e-6f) ? 2.0f / max_dim : 1.0f;
    p->center = vec3_scale(vec3_add(bmin, bmax), 0.5f);
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
    /* position = at - scale*center, so the model's centre lands on `at`. */
    t->position = vec3_sub(at, vec3_scale(p->center, p->scale));

    renderable_t *r = entity_add_component(app->world, e, app->renderable_id);
    r->mesh = p->mesh;
    r->material = p->material;
    return e;
}

/* Register the prototypes and lay one of each out in a starting row. */
static void build_scene(app_t *app) {
    material_handle_t spot_mat = textured(MODEL_PATH("spot.png"));
    if (spot_mat == RENDER_MATERIAL_INVALID) {
        spot_mat = solid(0.90f, 0.85f, 0.80f);
    }
    app->highlight_material = solid(1.0f, 0.80f, 0.15f);

    cpu_mesh_t cube;
    if (cpu_mesh_cube(&cube)) {
        add_prototype(app, "CUBE", &cube, solid(0.85f, 0.55f, 0.25f));
        cpu_mesh_free(&cube);
    }
    load_prototype(app, "COW", MODEL_PATH("cow.obj"), solid(0.80f, 0.30f, 0.30f));
    load_prototype(app, "SPOT", MODEL_PATH("spot.obj"), spot_mat);
    load_prototype(app, "FANDISK", MODEL_PATH("fandisk.obj"),
                   solid(0.45f, 0.65f, 0.85f));
    load_prototype(app, "CHEBURASHKA", MODEL_PATH("cheburashka.obj"),
                   solid(0.55f, 0.45f, 0.70f));
    load_prototype(app, "ARMADILLO", MODEL_PATH("armadillo.obj"),
                   solid(0.55f, 0.75f, 0.45f));

    const float spacing = 2.8f;
    float x0 = -spacing * (float)(app->prototype_count - 1) * 0.5f;
    for (int i = 0; i < app->prototype_count; i++) {
        spawn(app, i, (vec3_t){x0 + spacing * (float)i, 0.0f, 0.0f});
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

    ui_init(&app->ui);
    app->selected = ECS_ENTITY_NULL;
    app->sel_prototype = 0;
    app->spin_speed = 0.6f;
    app->bg_color[0] = 0.02f;
    app->bg_color[1] = 0.02f;
    app->bg_color[2] = 0.05f;

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

static const char *selected_name(app_t *app) {
    if (app->selected == ECS_ENTITY_NULL ||
        !entity_is_alive(app->world, app->selected)) {
        return "NONE";
    }
    renderable_t *r =
        entity_get_component(app->world, app->selected, app->renderable_id);
    if (r) {
        for (int i = 0; i < app->prototype_count; i++) {
            if (app->prototypes[i].mesh == r->mesh) {
                return app->prototypes[i].name;
            }
        }
    }
    return "?";
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
                break;
            case EVENT_BUTTON_DOWN:
                if (event.button.button == MOUSE_BUTTON_LEFT) {
                    app->mouse_left = true;
                }
                break;
            case EVENT_BUTTON_UP:
                if (event.button.button == MOUSE_BUTTON_LEFT) {
                    app->mouse_left = false;
                }
                break;
            default:
                break;
            }

            /* The camera gets mouse events only when the UI didn't own the
               mouse last frame; non-mouse events always pass through. */
            bool mouse_ev = event.type == EVENT_MOUSE_MOVE ||
                            event.type == EVENT_BUTTON_DOWN ||
                            event.type == EVENT_BUTTON_UP ||
                            event.type == EVENT_SCROLL;
            if (!(mouse_ev && app->ui_capture)) {
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

        render_scene(app);
        build_ui(app);
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
