#pragma once

#include <stdbool.h>

#include "platform.h"
#include "ecs.h"

/* Owns the engine subsystems for one running instance. Fields accrete
 * here as subsystems come online, keeping main.c a thin entry point. */
typedef struct {
    window_t *window;
    world_t *world;
} app_t;

bool app_init(app_t *app);
void app_run(app_t *app);
void app_shutdown(app_t *app);
