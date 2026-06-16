#include "app.h"

#include "core.h"
#include "render.h"
#include "assets.h"

#include <stdint.h>
#include <stdlib.h>

bool app_init(app_t *app) {
    core_init();

    app->world = world_create();

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
                break;
            }
        }

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
