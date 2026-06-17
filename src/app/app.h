#pragma once

#include <stdbool.h>

#include "camera.h"
#include "gizmo.h"
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

/* A spawnable template: a loaded mesh + material plus the uniform scale and
   local centre that normalize it to a viewable size. The editor instantiates
   entities from these. */
typedef struct {
    const char *name;
    mesh_handle_t mesh;
    material_handle_t material;
    float scale; /* uniform normalize-to-~2-units scale; mesh is pre-centred */

    /* CPU geometry kept for ray picking — the recentred, unscaled local-space
       triangles (the GPU copy is freed after upload). pick_min/max is the local
       AABB used as a broadphase before the per-triangle test. */
    vec3_t *pick_pos;
    uint32_t pick_vcount;
    uint32_t *pick_idx;
    uint32_t pick_icount;
    vec3_t pick_min, pick_max;
} prototype_t;

#define APP_MAX_PROTOTYPES 16

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

    /* Left-click selection. A press that neither the UI nor the gizmo claims
       arms a pick; we accumulate pointer motion while held (reliable even when
       the cursor is captured for orbit) and, if the release barely moved, cast a
       ray through the press pixel. */
    bool pick_armed;
    float pick_down_x, pick_down_y; /* press pixel (cursor still un-warped) */
    float pick_drag;                /* summed |motion| since the press */
    bool pick_request;              /* a click is awaiting its ray cast */

    /* crude editor: spawnable templates + the currently selected instance */
    prototype_t prototypes[APP_MAX_PROTOTYPES];
    int prototype_count;
    int sel_prototype;             /* which template ADD spawns */
    entity_t selected;             /* ECS_ENTITY_NULL if nothing selected */
    material_handle_t highlight_material;
    gizmo_t gizmo;
    bool gizmo_capture; /* gizmo owned the mouse last frame; gates camera */

    /* diagnostics + things the UI tampers with */
    float fps;
    uint32_t draw_count;
    bool auto_rotate;
    float spin_speed;
    float bg_color[3];

    /* directional light, set each frame from these spherical coordinates */
    float light_yaw;         /* degrees: horizontal rotation of the sun */
    float light_pitch;       /* degrees: elevation above the horizon (0–90) */
    float light_intensity;   /* key-light brightness multiplier */
    float ambient_intensity; /* ambient fill brightness multiplier */

    /* scene persistence: brief status line shown in the panel */
    char scene_status[64];
} app_t;

bool app_init(app_t *app);
void app_run(app_t *app);
void app_shutdown(app_t *app);
