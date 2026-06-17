#pragma once

#include <stdbool.h>

#include "camera.h"
#include "linalg.h"
#include "platform.h"
#include "render.h"
#include "ui.h"
#include "ecs.h"

/* World-space placement of an entity. The only component for now; lives here
 * (rather than its own module) until a second component justifies one. */
typedef struct {
    vec3_t position;
    quat_t rotation;
    vec3_t scale;
} transform_t;

/* A drawable: which mesh, rendered with which material. */
typedef struct {
    mesh_handle_t mesh;
    material_handle_t material;
} renderable_t;

/* Owns the engine subsystems for one running instance. Fields accrete
 * here as subsystems come online, keeping main.c a thin entry point. */
typedef struct {
    window_t *window;
    world_t *world;
    component_id_t transform_id;
    component_id_t renderable_id; /* stores a renderable_t */
    camera_t camera;

    ui_t ui;
    bool ui_capture; /* UI owned the mouse last frame; gates camera input */
    float mouse_x, mouse_y;
    bool mouse_left;

    /* diagnostics + things the UI tampers with */
    float fps;
    uint32_t draw_count;
    bool auto_rotate;
    float spin_speed;
    float bg_color[3];
} app_t;

bool app_init(app_t *app);
void app_run(app_t *app);
void app_shutdown(app_t *app);
