#pragma once

#include <stdbool.h>

#include "camera.h"
#include "fps_camera.h"
#include "gizmo.h"
#include "linalg.h"
#include "platform.h"
#include "render.h"
#include "settings.h"
#include "ui.h"
#include "ecs.h"

/* Snapshot of an entity's transform — used by the undo/redo history. */
typedef struct {
    vec3_t position;
    quat_t rotation;
    vec3_t scale;
} saved_xform_t;

typedef enum { CMD_SPAWN, CMD_DELETE, CMD_TRANSFORM } cmd_type_t;

typedef struct {
    cmd_type_t    type;
    entity_t      entity;   /* live handle; updated when entity is re-created */
    int           proto;    /* prototype index (SPAWN / DELETE) */
    saved_xform_t xform;    /* SPAWN/DELETE: the saved transform; TRANSFORM: before */
    saved_xform_t xform2;   /* TRANSFORM: after */
} cmd_t;

#define HISTORY_CAP 64

typedef struct {
    cmd_t entries[HISTORY_CAP];
    int   top;   /* entries[0..top-1] are valid */
    int   pos;   /* current undo position; pos==top means nothing to redo */
} history_t;

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
    settings_t settings;

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

    /* fly camera: TAB toggles; WASD+QE move, mouse look rotates */
    fps_camera_t fly_cam;
    vec3_t       fly_pos;
    bool         fly_mode;
    bool         fly_keys[KEY_COUNT]; /* held-key state tracked while in fly mode */

    /* diagnostics + things the UI tampers with */
    float fps;
    uint32_t draw_count;
    bool auto_rotate;
    float spin_speed;
    float bg_color[3];
    bool  vsync;      /* mirrors the renderer present mode */
    float fps_limit;  /* Hz; 0 = unlimited */
    bool  wireframe;

    /* directional light, set each frame from these spherical coordinates */
    float light_yaw;         /* degrees: horizontal rotation of the sun */
    float light_pitch;       /* degrees: elevation above the horizon (0–90) */
    float light_intensity;   /* key-light brightness multiplier */
    float ambient_intensity; /* ambient fill brightness multiplier */

    /* scene persistence: brief status line shown in the panel */
    char scene_status[64];

    /* rolling frame-time history for the graph widget (milliseconds) */
#define APP_FRAME_SAMPLES 128
    float frame_ms[APP_FRAME_SAMPLES];
    int   frame_ms_head;

    gpu_emitter_handle_t particle_emitter;

    /* Instanced ground-plane cubes — exercises the GPU cull + indirect path. */
    mesh_handle_t     inst_mesh;
    material_handle_t inst_mat;

    /* Undo/redo history for scene mutations (spawn, delete, transform). */
    history_t     history;
    saved_xform_t gizmo_drag_start; /* transform snapshot at the start of a gizmo drag */

    bool show_grid;

    /* Outliner scroll state (right-side panel). */
    int outliner_scroll;         /* first visible row index */
    int outliner_pending_scroll; /* wheel delta accumulated in event loop */

    /* Properties inspector drag gesture tracking. */
    bool          prop_editing;     /* true while a drag-float field is held */
    saved_xform_t prop_edit_start;  /* transform snapshot at drag-start */
} app_t;

bool app_init(app_t *app);
void app_run(app_t *app);
void app_shutdown(app_t *app);
