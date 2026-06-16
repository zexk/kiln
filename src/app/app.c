#include "app.h"

#include "core.h"
#include "render.h"
#include "assets.h"

bool app_init(app_t *app) {
    core_init();

    app->world = world_create();

    app->window = window_create("Kiln", 1280, 720);
    if (!app->window) {
        return false;
    }

    render_init();
    assets_init();

    return true;
}

void app_run(app_t *app) {
    bool running = true;
    while (running) {
        /* Idle without spinning until the OS has something for us. */
        window_wait_events(app->window);

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
    }
}

void app_shutdown(app_t *app) {
    window_destroy(app->window);
    world_destroy(app->world);
}
