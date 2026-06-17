#pragma once

#include <stdbool.h>

#include "camera.h"
#include "linalg.h"
#include "platform.h"
#include "ecs.h"

/* World-space placement of an entity. The only component for now; lives here
 * (rather than its own module) until a second component justifies one. */
typedef struct {
    vec3_t position;
    quat_t rotation;
    vec3_t scale;
} transform_t;

/* Owns the engine subsystems for one running instance. Fields accrete
 * here as subsystems come online, keeping main.c a thin entry point. */
typedef struct {
    window_t *window;
    world_t *world;
    component_id_t transform_id;
    camera_t camera;
} app_t;

bool app_init(app_t *app);
void app_run(app_t *app);
void app_shutdown(app_t *app);
