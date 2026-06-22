#include "render.h"
#include "platform.h"
#include "linalg.h"
#include "fps_camera.h"
#include "physics.h"
#include "timer.h"
#include "mesh.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Buffon's Needle in 3D: needles fall from above and land on a ruled plane.
   Parallel lines run along X at integer Z values.  Needle length == line
   spacing, so P(crossing) = 2/pi and pi ~ 2*landed/crossings.

   Timescale is implemented by sub-stepping: each visible frame runs N physics
   sub-steps (N = round(timescale)), each with a fixed dt = 1/60 s.  This
   avoids the 1/60 clamp inside phys_step while scaling both fall speed and
   spawn rate uniformly. */

#define WIN_W          1280
#define WIN_H          720

#define MAX_NEEDLES    3000
#define BASE_SPAWN      3       /* needles per sub-step */
#define FIELD_HALF     40.0f   /* lines from -FIELD_HALF to +FIELD_HALF in Z */
#define LINE_SPACING    1.0f
#define NEEDLE_LEN      1.0f   /* == LINE_SPACING, so P = 2/pi */
#define NEEDLE_GRAVITY 12.0f
#define SPAWN_Y        10.0f

#define SUBSTEP_DT     (1.0f / 60.0f)

static float randf(float lo, float hi) {
    return lo + (float)rand() / ((float)RAND_MAX + 1.0f) * (hi - lo);
}

typedef struct {
    phys_body_t body;
    float       angle;     /* rotation around Y, fixed at spawn */
    bool        landed;
    bool        crossing;
} needle_t;

static needle_t g_needles[MAX_NEEDLES];
static int      g_count    = 0;   /* ring fill level */
static long     g_total    = 0;   /* all-time landings */
static long     g_cross    = 0;   /* all-time crossings */

/* Solid callback: the ground plane is at y = 0; anything below is solid. */
static bool ground_solid(void *ctx, int x, int y, int z) {
    (void)ctx; (void)x; (void)z;
    return y < 0;
}

static void spawn_needle(void) {
    if (g_count >= MAX_NEEDLES) g_count = 0;
    needle_t *n = &g_needles[g_count++];
    n->body = (phys_body_t){
        .position = { randf(-FIELD_HALF, FIELD_HALF), SPAWN_Y,
                      randf(-FIELD_HALF, FIELD_HALF) },
        .velocity = {0},
        .half_w   = 0.02f,
        .foot_off = 0.01f,
        .head_off = 0.01f,
        .gravity  = NEEDLE_GRAVITY,
        .grounded = false,
    };
    n->angle   = randf(0.0f, KLN_PI);
    n->landed  = false;
    n->crossing = false;
}

static void land_needle(needle_t *n) {
    n->landed = true;
    float z   = n->body.position.z;
    float hpz = (NEEDLE_LEN * 0.5f) * fabsf(sinf(n->angle));
    n->crossing = (int)floorf((z - hpz) / LINE_SPACING) !=
                  (int)floorf((z + hpz) / LINE_SPACING);
    g_total++;
    if (n->crossing) g_cross++;
}

int main(void) {
    srand((unsigned)time(NULL));

    window_t *win = window_create("Buffon's Needle", WIN_W, WIN_H);
    if (!win) return 1;
    if (!render_init(win)) { window_destroy(win); return 1; }

    render_set_clear_color(0.08f, 0.08f, 0.10f);
    render_set_light(
        vec3_normalize((vec3_t){-0.4f, -1.0f, -0.3f}),
        (vec3_t){1.0f, 0.95f, 0.85f},
        (vec3_t){0.35f, 0.35f, 0.40f});

    /* ---- meshes ---- */
    cpu_mesh_t cm = {0};
    if (!cpu_mesh_cube(&cm)) return 1;
    mesh_handle_t needle_mesh = render_upload_mesh(&cm);
    mesh_handle_t line_mesh   = render_upload_mesh(&cm);
    mesh_handle_t ground_mesh = render_upload_mesh(&cm);
    cpu_mesh_free(&cm);

    /* ---- materials ---- */
    material_handle_t mat_hit    = render_create_material(
        (vec4_t){0.25f, 0.85f, 0.35f, 1.0f}, RENDER_TEXTURE_INVALID);
    material_handle_t mat_miss   = render_create_material(
        (vec4_t){0.85f, 0.25f, 0.25f, 1.0f}, RENDER_TEXTURE_INVALID);
    material_handle_t mat_fly    = render_create_material(
        (vec4_t){0.90f, 0.80f, 0.20f, 1.0f}, RENDER_TEXTURE_INVALID);
    material_handle_t mat_ground = render_create_material(
        (vec4_t){0.18f, 0.18f, 0.20f, 1.0f}, RENDER_TEXTURE_INVALID);
    material_handle_t mat_line   = render_create_material(
        (vec4_t){0.45f, 0.65f, 0.85f, 1.0f}, RENDER_TEXTURE_INVALID);

    /* ---- precompute line model matrices ---- */
    int n_lines = (int)(2.0f * FIELD_HALF / LINE_SPACING) + 1;
    mat4_t *line_models = malloc(sizeof(mat4_t) * (uint32_t)n_lines);
    for (int i = 0; i < n_lines; i++) {
        float z = -FIELD_HALF + i * LINE_SPACING;
        line_models[i] = mat4_from_trs(
            (vec3_t){0.0f, 0.003f, z},
            quat_identity(),
            (vec3_t){FIELD_HALF * 2.0f, 0.006f, 0.04f});
    }

    mat4_t ground_model = mat4_from_trs(
        (vec3_t){0.0f, -0.005f, 0.0f},
        quat_identity(),
        (vec3_t){FIELD_HALF * 2.5f, 0.008f, FIELD_HALF * 2.5f});

    /* ---- camera ---- */
    fps_camera_t cam;
    fps_camera_init(&cam);
    cam.yaw   =  KLN_PI * 0.5f;  /* look along +Z */
    cam.pitch = -0.45f;           /* look down ~26° */
    vec3_t cam_pos = {0.0f, 14.0f, -18.0f};
    /* recompute front after setting yaw/pitch manually */
    fps_camera_rotate(&cam, 0.0f, 0.0f);

    window_set_cursor_mode(win, CURSOR_DISABLED);

    /* ---- instance buffers (rebuilt each frame) ---- */
    mat4_t *inst_hit  = malloc(sizeof(mat4_t) * MAX_NEEDLES);
    mat4_t *inst_miss = malloc(sizeof(mat4_t) * MAX_NEEDLES);
    mat4_t *inst_fly  = malloc(sizeof(mat4_t) * MAX_NEEDLES);

    bool   running   = true;
    bool   paused    = false;
    float  timescale = 1.0f;
    double last_time = kln_timer_now();

    float  mouse_dx = 0.0f, mouse_dy = 0.0f;
    bool   keys[KEY_COUNT];
    memset(keys, 0, sizeof(keys));

    while (running) {
        double now = kln_timer_now();
        float  dt  = (float)(now - last_time);
        last_time  = now;
        if (dt > 0.1f) dt = 0.1f;

        mouse_dx = mouse_dy = 0.0f;

        uint32_t w = WIN_W, h = WIN_H;
        window_size(win, &w, &h);

        event_t ev;
        while (window_poll_event(win, &ev)) {
            switch (ev.type) {
            case EVENT_QUIT: running = false; break;
            case EVENT_KEY_DOWN:
                if (ev.key.code < KEY_COUNT) keys[ev.key.code] = true;
                if (ev.key.code == KEY_ESCAPE) running = false;
                if (ev.key.code == KEY_SPACE)  paused = !paused;
                if (ev.key.keysym == 'r' || ev.key.keysym == 'R')
                    { g_count = 0; g_total = 0; g_cross = 0; }
                if (ev.key.keysym == '=' || ev.key.keysym == '+')
                    { timescale *= 2.0f; if (timescale > 64.0f) timescale = 64.0f; }
                if (ev.key.keysym == '-')
                    { timescale *= 0.5f; if (timescale < 0.25f) timescale = 0.25f; }
                break;
            case EVENT_KEY_UP:
                if (ev.key.code < KEY_COUNT) keys[ev.key.code] = false;
                break;
            case EVENT_MOUSE_MOTION:
                mouse_dx += ev.motion.dx;
                mouse_dy += ev.motion.dy;
                break;
            default: break;
            }
        }

        /* camera rotation */
        if (!paused) fps_camera_rotate(&cam, mouse_dx, mouse_dy);

        /* camera translation */
        {
            float speed = 8.0f * dt;
            vec3_t right, cam_up;
            fps_camera_basis(&cam, &right, &cam_up);
            if (keys[KEY_W]) cam_pos = vec3_add(cam_pos, vec3_scale(cam.front, speed));
            if (keys[KEY_S]) cam_pos = vec3_sub(cam_pos, vec3_scale(cam.front, speed));
            if (keys[KEY_A]) cam_pos = vec3_sub(cam_pos, vec3_scale(right, speed));
            if (keys[KEY_D]) cam_pos = vec3_add(cam_pos, vec3_scale(right, speed));
            if (keys[KEY_E]) cam_pos = vec3_add(cam_pos, vec3_scale(cam_up, speed));
            if (keys[KEY_Q]) cam_pos = vec3_sub(cam_pos, vec3_scale(cam_up, speed));
        }

        /* physics sub-steps */
        if (!paused) {
            int steps = (int)roundf(timescale);
            if (steps < 1) steps = 1;
            for (int s = 0; s < steps; s++) {
                for (int i = 0; i < BASE_SPAWN; i++) spawn_needle();
                for (int i = 0; i < g_count; i++) {
                    needle_t *n = &g_needles[i];
                    if (n->landed) continue;
                    bool was_grounded = n->body.grounded;
                    phys_step(&n->body, SUBSTEP_DT, ground_solid, NULL);
                    if (!was_grounded && n->body.grounded) land_needle(n);
                }
            }
        }

        /* ---- render ---- */
        float aspect = h > 0 ? (float)w / (float)h : 1.0f;
        mat4_t view  = fps_camera_view(&cam, cam_pos);
        mat4_t proj  = mat4_perspective(kln_radians(60.0f), aspect, 0.1f, 300.0f);
        render_set_camera(view, proj);

        render_mesh(ground_mesh, mat_ground, ground_model);
        render_mesh_instanced(line_mesh, mat_line,
                              line_models, (uint32_t)n_lines);

        int n_hit = 0, n_miss = 0, n_fly = 0;
        for (int i = 0; i < g_count; i++) {
            needle_t *n = &g_needles[i];
            float len = NEEDLE_LEN;
            float h2  = 0.025f;
            mat4_t m  = mat4_from_trs(
                n->body.position,
                quat_from_axis_angle((vec3_t){0,1,0}, n->angle),
                (vec3_t){len, h2, h2 * 0.6f});
            if (!n->landed) {
                if (n_fly < MAX_NEEDLES) inst_fly[n_fly++] = m;
            } else if (n->crossing) {
                if (n_hit < MAX_NEEDLES) inst_hit[n_hit++] = m;
            } else {
                if (n_miss < MAX_NEEDLES) inst_miss[n_miss++] = m;
            }
        }
        if (n_fly  > 0) render_mesh_instanced(needle_mesh, mat_fly,  inst_fly,  (uint32_t)n_fly);
        if (n_miss > 0) render_mesh_instanced(needle_mesh, mat_miss, inst_miss, (uint32_t)n_miss);
        if (n_hit  > 0) render_mesh_instanced(needle_mesh, mat_hit,  inst_hit,  (uint32_t)n_hit);

        /* ---- overlay ---- */
        double pi_est = g_cross > 0 ? 2.0 * g_total / (double)g_cross : 0.0;
        double err    = pi_est > 0.0
            ? 100.0 * fabs(pi_est - 3.14159265358979) / 3.14159265358979 : 0.0;
        char buf[160];
        render_rect(0.0f, 0.0f, (float)w, 56.0f, 0.05f, 0.05f, 0.07f);
        snprintf(buf, sizeof(buf),
                 "Buffon's Needle   pi ~ %.8f   error %.5f%%   landed %ld",
                 pi_est, err, g_total);
        render_text(12.0f, 10.0f, 2.0f, 1.0f, 1.0f, 1.0f, buf);
        snprintf(buf, sizeof(buf),
                 "timescale x%.0f   %s   +/- scale   space pause   r reset   esc quit",
                 timescale, paused ? "PAUSED" : "wasd/mouse fly");
        render_text(12.0f, 36.0f, 1.5f, 0.40f, 0.72f, 0.52f, buf);

        render_draw();
    }

    free(inst_hit); free(inst_miss); free(inst_fly); free(line_models);
    render_shutdown();
    window_destroy(win);
    return 0;
}
