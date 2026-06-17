#include "app.h"

#include "core.h"
#include "linalg.h"
#include "render.h"
#include "assets.h"

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* A small grid of cubes so the ECS-driven render path has something to chew
   on. Each becomes one entity with a transform component. */
static void spawn_scene(app_t *app) {
    const int n = 3; /* (2n+1)^2 cubes */
    for (int x = -n; x <= n; x++) {
        for (int z = -n; z <= n; z++) {
            entity_t e = entity_create(app->world);
            transform_t *t =
                entity_add_component(app->world, e, app->transform_id);
            t->position = (vec3_t){(float)x * 1.6f, 0.0f, (float)z * 1.6f};
            t->rotation = quat_identity();
            t->scale = (vec3_t){0.6f, 0.6f, 0.6f};
        }
    }
}

bool app_init(app_t *app) {
    core_init();

    app->world = world_create();
    app->transform_id = component_register(app->world, "transform",
                                           sizeof(transform_t),
                                           _Alignof(transform_t));
    spawn_scene(app);

    app->window = window_create("Kiln", 1280, 720);
    if (!app->window) {
        return false;
    }

    if (!render_init(app->window)) {
        return false;
    }
    assets_init();

    return true;
}

static query_iter_t transform_query(app_t *app) {
    signature_t require;
    signature_clear(&require);
    signature_set(&require, app->transform_id);
    return query_iter(app->world, (query_desc_t){.require = require});
}

/* Rotate every transform about the Y axis; the ECS query is the engine doing
   real per-entity work each frame. */
static void rotate_system(app_t *app, float dt) {
    query_iter_t it = transform_query(app);
    quat_t spin = quat_from_axis_angle((vec3_t){0, 1, 0}, dt * 0.8f);
    while (query_next(&it)) {
        transform_t *t = query_get(&it, app->transform_id);
        t->rotation = quat_normalize(quat_mul(spin, t->rotation));
    }
}

/* Set the camera, then queue one cube per transform entity. */
static void render_scene(app_t *app) {
    uint32_t w, h;
    window_size(app->window, &w, &h);
    float aspect = h ? (float)w / (float)h : 1.0f;

    mat4_t view = mat4_look_at((vec3_t){6.0f, 5.0f, 8.0f}, (vec3_t){0, 0, 0},
                               (vec3_t){0, 1, 0});
    mat4_t proj = mat4_perspective(kln_radians(60.0f), aspect, 0.1f, 100.0f);
    render_set_camera(view, proj);

    query_iter_t it = transform_query(app);
    while (query_next(&it)) {
        transform_t *t = query_get(&it, app->transform_id);
        render_cube(mat4_from_trs(t->position, t->rotation, t->scale));
    }
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
                break;
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        float dt = (float)(now.tv_sec - prev.tv_sec) +
                   (float)(now.tv_nsec - prev.tv_nsec) * 1e-9f;
        prev = now;

        rotate_system(app, dt);
        render_scene(app);

        render_text(16.0f, 16.0f, 3.0f, 0.9f, 0.9f, 0.2f, "KILN DEBUG");
        render_text(16.0f, 48.0f, 2.0f, 0.6f, 0.8f, 1.0f,
                    "the quick brown fox 0123456789");
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
