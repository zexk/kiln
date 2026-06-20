#include "renderer.h"
#include "platform.h"
#include "core.h"
#include "timer.h"
#include "noise.h"
#include "math3d.h"
#include "logger.h"
#include "game_input.h"
#include "ui.h"
#include "voxel.h"
#include "mesh.h"
#include "camera.h"
#include "world.h"
#include "texture.h"
#include "ecs.h"
#include "components.h"
#include "systems.h"
#include "gui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_WIDTH        800
#define WINDOW_HEIGHT       600
#define RAYCAST_MAX_DISTANCE 8.0f
#define RAYCAST_STEP        0.05f
#define COOLDOWN_TIME       0.25f
#define FOV_DEGREES         45.0f
#define NEAR_PLANE          0.1f
#define FAR_PLANE           100.0f

/* ── Game-side ui_draw_t: routes to gui.c's renderer.h primitives ─────────── */

static Gui *s_ui_gui; /* set once after gui_init, before first ui_begin */

static void game_ui_rect(void *ud, float x, float y, float w, float h,
                          float r, float g, float b) {
    gui_rect((Gui *)ud, x, y, w, h, r, g, b);
}
static void game_ui_text(void *ud, float x, float y, float scale,
                          float r, float g, float b, const char *s) {
    gui_write_text((Gui *)ud, x, y, s, scale, r, g, b);
}

static void quit_button_callback(void *userdata) {
    bool *running = userdata;
    if (running) *running = false;
}

static bool raycast_find_solid(World *world, vec3 pos, vec3 dir, float max_dist, float step, BlockPos *out) {
    for (float t = 0; t < max_dist; t += step) {
        vec3 p = vec3_add(pos, vec3_scale(dir, t));
        int bx = (int)floorf(p.x), by = (int)floorf(p.y), bz = (int)floorf(p.z);
        if (world_get_block(world, bx, by, bz) != BLOCK_AIR) {
            out->x = bx; out->y = by; out->z = bz;
            return true;
        }
    }
    return false;
}

static bool raycast_find_solid_with_prev(World *world, vec3 pos, vec3 dir, float max_dist, float step,
                                         BlockPos *out, BlockPos *prev_out) {
    BlockPos prev = {0};
    bool has_prev = false;
    for (float t = 0; t < max_dist; t += step) {
        vec3 p = vec3_add(pos, vec3_scale(dir, t));
        int bx = (int)floorf(p.x), by = (int)floorf(p.y), bz = (int)floorf(p.z);
        if (world_get_block(world, bx, by, bz) != BLOCK_AIR) {
            if (out)      { out->x = bx; out->y = by; out->z = bz; }
            if (prev_out) { *prev_out = prev; }
            return has_prev;
        }
        prev.x = bx; prev.y = by; prev.z = bz;
        has_prev = true;
    }
    return false;
}

int main(void) {
    core_install_crash_handler();
    noise_init(42);

#ifdef ENABLE_LOGGER
    log_init("kyub.log");
#endif

    window_t *win = window_create("Kyub", WINDOW_WIDTH, WINDOW_HEIGHT);
    if (!win) { fprintf(stderr, "Failed to create window\n"); return 1; }

    platform_native_handles_t nh = window_get_native_handles(win);
    if (!renderer_init(WINDOW_WIDTH, WINDOW_HEIGHT, &nh)) {
        fprintf(stderr, "Failed to initialize renderer\n");
        window_destroy(win);
        return 1;
    }

    renderer_viewport(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);
    renderer_swap_interval(0);
    renderer_enable(R_CAP_DEPTH_TEST);
    renderer_enable(R_CAP_CULL_FACE);
    renderer_enable(R_CAP_MULTISAMPLE);

    ecs_init(&g_ecs, 4096);
    components_init(&g_ecs);
    memset(g_block_entities, 0, sizeof(g_block_entities));

    register_block_type(&g_ecs, BLOCK_AIR,    "kyub:air",    "Air",    false, false, 0.0f, NULL, NULL, NULL, NULL);
    register_block_type(&g_ecs, BLOCK_DIRT,   "kyub:dirt",   "Dirt",   true,  true,  1.0f, "assets/textures/dirt.png",   NULL, NULL, NULL);
    register_block_type(&g_ecs, BLOCK_GRASS,  "kyub:grass",  "Grass",  true,  true,  1.0f, "assets/textures/dirt.png",   "assets/textures/grass_top.png", "assets/textures/dirt.png", "assets/textures/grass_side.png");
    register_block_type(&g_ecs, BLOCK_STONE,  "kyub:stone",  "Stone",  true,  true,  2.0f, "assets/textures/stone.png",  NULL, NULL, NULL);
    register_block_type(&g_ecs, BLOCK_SAND,   "kyub:sand",   "Sand",   true,  true,  1.0f, "assets/textures/sand.png",   NULL, NULL, NULL);
    register_block_type(&g_ecs, BLOCK_GRAVEL, "kyub:gravel", "Gravel", true,  true,  1.0f, "assets/textures/gravel.png", NULL, NULL, NULL);
    register_block_type(&g_ecs, BLOCK_WOOD,   "kyub:wood",   "Wood",   true,  true,  2.0f, "assets/textures/wood.png",   NULL, NULL, NULL);
    register_block_type(&g_ecs, BLOCK_LEAVES, "kyub:leaves", "Leaves", true,  false, 0.5f, "assets/textures/leaves.png", NULL, NULL, NULL);

    /* Collect unique texture paths */
    const char *tex_paths[32]; int tex_path_count = 0;
    for (int t = 0; t < 256; t++) {
        Entity e = g_block_entities[t]; if (!e) continue;
        C_BlockDef *def = ecs_get(&g_ecs, e, COMP_BLOCK_DEF); if (!def) continue;
        const char *paths[4] = {def->tex_path, def->tex_top, def->tex_bottom, def->tex_side};
        for (int p = 0; p < 4; p++) {
            if (!paths[p]) continue;
            bool found = false;
            for (int i = 0; i < tex_path_count; i++) if (strcmp(tex_paths[i], paths[p]) == 0) { found = true; break; }
            if (!found && tex_path_count < 32) tex_paths[tex_path_count++] = paths[p];
        }
    }

    R_Texture tex_array = texture_load_array(tex_paths, tex_path_count, 16, 16);
    if (tex_array == R_INVALID_HANDLE) { fprintf(stderr, "Failed to load block textures\n"); return 1; }

    /* Resolve layer indices */
    for (int t = 0; t < 256; t++) {
        Entity e = g_block_entities[t]; if (!e) continue;
        C_BlockDef *def = ecs_get(&g_ecs, e, COMP_BLOCK_DEF); if (!def) continue;
        const char *paths[4] = {def->tex_path, def->tex_top, def->tex_bottom, def->tex_side};
        int *layers[4] = {&def->layer_default, &def->layer_top, &def->layer_bottom, &def->layer_side};
        for (int p = 0; p < 4; p++) {
            if (!paths[p]) continue;
            for (int i = 0; i < tex_path_count; i++)
                if (strcmp(tex_paths[i], paths[p]) == 0) { *layers[p] = i; break; }
        }
    }

    R_Program shader_program  = renderer_create_program("shaders/basic.vert",   "shaders/basic.frag");
    R_Program hud_program     = renderer_create_program("shaders/hud.vert",     "shaders/hud.frag");
    R_Program skybox_program  = renderer_create_program("shaders/skybox.vert",  "shaders/skybox.frag");
    R_Program outline_program = renderer_create_program("shaders/outline.vert", "shaders/outline.frag");
    if (shader_program == R_INVALID_HANDLE || hud_program == R_INVALID_HANDLE ||
        skybox_program == R_INVALID_HANDLE || outline_program == R_INVALID_HANDLE) {
        fprintf(stderr, "Failed to load shaders\n"); return 1;
    }

    /* Skybox VAO */
    R_VAO    skybox_vao = renderer_create_vao();
    R_Buffer skybox_vbo = renderer_create_buffer();
    float skybox_tri[] = { -1.0f,-1.0f,  3.0f,-1.0f,  -1.0f,3.0f };
    renderer_bind_vao(skybox_vao);
    renderer_bind_buffer(R_BUF_ARRAY, skybox_vbo);
    renderer_buffer_data(R_BUF_ARRAY, sizeof(skybox_tri), skybox_tri, R_USAGE_STATIC);
    renderer_attrib_pointer(0, 2, R_TYPE_FLOAT, false, 8, 0);
    renderer_enable_attrib(0);
    renderer_bind_vao(R_INVALID_HANDLE);

    /* Outline VAO */
    R_VAO    outline_vao = renderer_create_vao();
    R_Buffer outline_vbo = renderer_create_buffer();
    float outline_cube[] = {
        0,0,0, 1,0,0,   1,0,0, 1,0,1,   1,0,1, 0,0,1,   0,0,1, 0,0,0,
        0,1,0, 1,1,0,   1,1,0, 1,1,1,   1,1,1, 0,1,1,   0,1,1, 0,1,0,
        0,0,0, 0,1,0,   1,0,0, 1,1,0,   1,0,1, 1,1,1,   0,0,1, 0,1,1,
    };
    renderer_bind_vao(outline_vao);
    renderer_bind_buffer(R_BUF_ARRAY, outline_vbo);
    renderer_buffer_data(R_BUF_ARRAY, sizeof(outline_cube), outline_cube, R_USAGE_STATIC);
    renderer_attrib_pointer(0, 3, R_TYPE_FLOAT, false, 12, 0);
    renderer_enable_attrib(0);
    renderer_bind_vao(R_INVALID_HANDLE);

    /* Overlay (pause dim) VAO */
    R_VAO    overlay_vao = renderer_create_vao();
    R_Buffer overlay_vbo = renderer_create_buffer();
    float overlay_tri[] = { -1.0f,-1.0f,  3.0f,-1.0f,  -1.0f,3.0f };
    renderer_bind_vao(overlay_vao);
    renderer_bind_buffer(R_BUF_ARRAY, overlay_vbo);
    renderer_buffer_data(R_BUF_ARRAY, sizeof(overlay_tri), overlay_tri, R_USAGE_STATIC);
    renderer_attrib_pointer(0, 2, R_TYPE_FLOAT, false, 8, 0);
    renderer_enable_attrib(0);
    renderer_bind_vao(R_INVALID_HANDLE);

    /* HUD (crosshair) VAO */
    R_VAO    hud_vao = renderer_create_vao();
    R_Buffer hud_vbo = renderer_create_buffer();
    float crosshair[] = { -0.015f,0.0f, 0.015f,0.0f,  0.0f,-0.02f, 0.0f,0.02f };
    renderer_bind_vao(hud_vao);
    renderer_bind_buffer(R_BUF_ARRAY, hud_vbo);
    renderer_buffer_data(R_BUF_ARRAY, sizeof(crosshair), crosshair, R_USAGE_STATIC);
    renderer_attrib_pointer(0, 2, R_TYPE_FLOAT, false, 2 * sizeof(float), 0);
    renderer_enable_attrib(0);
    renderer_bind_vao(R_INVALID_HANDLE);

    /* Hotbar VAO */
#define HOTBAR_SLOTS       7
#define HOTBAR_SQUARE_SIZE 0.07f
#define HOTBAR_GAP         0.015f
    R_VAO    hotbar_vao = renderer_create_vao();
    R_Buffer hotbar_vbo = renderer_create_buffer();
    float hotbar_total_w = HOTBAR_SLOTS * HOTBAR_SQUARE_SIZE + (HOTBAR_SLOTS - 1) * HOTBAR_GAP;
    float hotbar_start_x = -hotbar_total_w / 2.0f;
    float hotbar_y0 = -0.93f, hotbar_y1 = hotbar_y0 + HOTBAR_SQUARE_SIZE;
    float hotbar_verts[HOTBAR_SLOTS * 6 * 2];
    for (int i = 0; i < HOTBAR_SLOTS; i++) {
        float x0 = hotbar_start_x + i * (HOTBAR_SQUARE_SIZE + HOTBAR_GAP);
        float x1 = x0 + HOTBAR_SQUARE_SIZE;
        int vi = i * 12;
        hotbar_verts[vi+0]=x0; hotbar_verts[vi+1]=hotbar_y0; hotbar_verts[vi+2]=x1; hotbar_verts[vi+3]=hotbar_y0;
        hotbar_verts[vi+4]=x1; hotbar_verts[vi+5]=hotbar_y1; hotbar_verts[vi+6]=x0; hotbar_verts[vi+7]=hotbar_y0;
        hotbar_verts[vi+8]=x1; hotbar_verts[vi+9]=hotbar_y1; hotbar_verts[vi+10]=x0; hotbar_verts[vi+11]=hotbar_y1;
    }
    renderer_bind_vao(hotbar_vao);
    renderer_bind_buffer(R_BUF_ARRAY, hotbar_vbo);
    renderer_buffer_data(R_BUF_ARRAY, sizeof(hotbar_verts), hotbar_verts, R_USAGE_STATIC);
    renderer_attrib_pointer(0, 2, R_TYPE_FLOAT, false, 2 * sizeof(float), 0);
    renderer_enable_attrib(0);
    renderer_bind_vao(R_INVALID_HANDLE);

    Gui gui;
    gui_init(&gui, hud_program);
    s_ui_gui = &gui;

    ui_t debug_ui;
    ui_init(&debug_ui);
    const ui_draw_t game_draw = {game_ui_rect, game_ui_text, &gui};

    World world;
    world_init(&world, 2);

    Camera camera;
    camera_init(&camera, &g_ecs);

    C_Transform *player_transform = ecs_get(&g_ecs, camera.player, COMP_TRANSFORM);
    vec3 player_pos = player_transform ? player_transform->position : (vec3){8.0f, 20.0f, 8.0f};

    world_update(&world, player_pos);
    for (int i = 0; i < 10; i++) world_update(&world, player_pos);

    int spawn_x = (int)floorf(player_pos.x);
    int spawn_z = (int)floorf(player_pos.z);
    int ground_y = 0;
    for (int y = CHUNK_SIZE - 1; y >= 0; y--) {
        if (world_is_solid(&world, spawn_x, y, spawn_z)) { ground_y = y; break; }
    }
    if (player_transform)
        player_transform->position.y = (float)(ground_y + 2) + PLAYER_EYES_HEIGHT;
    LOG_INFO(CAT_PLATFORM, "Spawn at y=%.2f (ground at y=%d)",
             player_transform ? player_transform->position.y : 20.0f, ground_y);

    BlockType selected_block = BLOCK_STONE;
    int render_distance = world.render_distance;

    window_set_cursor_mode(win, CURSOR_DISABLED);

    double last_time       = kln_timer_now();
    double last_fps_update = 0.0;
    double last_save_flush = 0.0;
    bool   running         = true;
    bool   paused          = false;
    bool   show_debug      = false;
    float  fps             = 0.0f;
    double fps_acc         = 0.0;
    int    fps_frames      = 0;
    uint32_t win_width  = WINDOW_WIDTH;
    uint32_t win_height = WINDOW_HEIGHT;
    window_size(win, &win_width, &win_height);

    GameInput game_input; game_input_init(&game_input);

    while (running) {
        window_size(win, &win_width, &win_height);
        double now = kln_timer_now();
        double dt  = now - last_time;
        last_time  = now;

        fps_acc += dt; fps_frames++;
        if (fps_acc >= 0.5) { fps = (float)(fps_frames / fps_acc); fps_acc = 0; fps_frames = 0; }

        event_t event;
        while (window_poll_event(win, &event)) {
            game_input_handle_event(&game_input, &event);

            switch (event.type) {
            case EVENT_KEY_DOWN:
                if (event.key.keysym == 't' || event.key.code == KEY_ESCAPE) {
                    paused = !paused;
                    window_set_cursor_mode(win, paused ? CURSOR_NORMAL : CURSOR_DISABLED);
                }
                if (event.key.keysym == 0xFFC0) /* F3 */ show_debug = !show_debug;
                break;
            case EVENT_SCROLL:
                if (!paused && event.scroll.delta != 0.0f) {
                    int next = (int)selected_block + (event.scroll.delta > 0.0f ? 1 : -1);
                    if (next > BLOCK_LEAVES) next = BLOCK_DIRT;
                    if (next < BLOCK_DIRT)   next = BLOCK_LEAVES;
                    selected_block = (BlockType)next;
                }
                break;
            case EVENT_RESIZE:
                win_width  = event.resize.width;
                win_height = event.resize.height;
                renderer_viewport(0, 0, (int)win_width, (int)win_height);
                break;
            case EVENT_QUIT:
                running = false;
                break;
            default:
                break;
            }
        }

        gui_begin_frame(&gui, (int)win_width, (int)win_height,
                        (int)game_input.mouse_x, (int)game_input.mouse_y,
                        game_input.mouse_left);

        if (paused) {
            GameInput zero = {0};
            camera_update(&camera, (float)dt, &world, &zero, &g_ecs);
            sys_movement(&g_ecs, &world, 0.0f);
        } else {
            camera_update(&camera, (float)dt, &world, &game_input, &g_ecs);
            sys_movement(&g_ecs, &world, (float)dt);
        }

        static float break_cooldown = 0.0f;
        static float place_cooldown = 0.0f;
        break_cooldown -= (float)dt;
        place_cooldown -= (float)dt;

        player_transform = ecs_get(&g_ecs, camera.player, COMP_TRANSFORM);
        vec3 cam_pos = player_transform ? player_transform->position : (vec3){0,0,0};

        if (!paused) {
            if (game_input.mouse_left && break_cooldown <= 0.0f) {
                BlockPos hit;
                if (raycast_find_solid(&world, cam_pos, camera.front, RAYCAST_MAX_DISTANCE, RAYCAST_STEP, &hit))
                    world_set_block(&world, hit.x, hit.y, hit.z, BLOCK_AIR);
                break_cooldown = COOLDOWN_TIME;
            }
            if (game_input.mouse_right && place_cooldown <= 0.0f) {
                BlockPos hit, prev;
                if (raycast_find_solid_with_prev(&world, cam_pos, camera.front, RAYCAST_MAX_DISTANCE, RAYCAST_STEP, &hit, &prev)) {
                    if (world_get_block(&world, prev.x, prev.y, prev.z) == BLOCK_AIR)
                        if (!player_collides_with_block(&world, cam_pos, prev))
                            world_set_block(&world, prev.x, prev.y, prev.z, selected_block);
                }
                place_cooldown = COOLDOWN_TIME;
            }
        }

        BlockPos hl; bool hl_found = raycast_find_solid(&world, cam_pos, camera.front, RAYCAST_MAX_DISTANCE, RAYCAST_STEP, &hl);

        renderer_enable(R_CAP_DEPTH_TEST);
        renderer_enable(R_CAP_CULL_FACE);
        renderer_disable(R_CAP_BLEND);
        renderer_clear(0.1f, 0.1f, 0.12f, 1.0f);

        world_update(&world, cam_pos);

        renderer_use_program(shader_program);
        renderer_active_texture(0);
        renderer_bind_texture(R_TEX_2D, tex_array);
        renderer_uniform_int(renderer_uniform_location(shader_program, "uTexture"), 0);
        renderer_uniform_vec3(renderer_uniform_location(shader_program, "uFogColor"), 0.53f, 0.81f, 0.92f);
        renderer_uniform_float(renderer_uniform_location(shader_program, "uFogDensity"), 0.015f);

        float aspect = (win_height > 0) ? (float)win_width / (float)win_height : 1.0f;
        mat4 projection = mat4_perspective(FOV_DEGREES * PI / 180.0f, aspect, NEAR_PLANE, FAR_PLANE);
        mat4 view       = camera_get_view_matrix(&camera, &g_ecs);

        Frustum frustum; frustum_extract(&frustum, mat4_mul(projection, view));

        int model_loc = renderer_uniform_location(shader_program, "model");
        int view_loc  = renderer_uniform_location(shader_program, "view");
        int proj_loc  = renderer_uniform_location(shader_program, "projection");
        renderer_uniform_mat4(view_loc, view.m);
        renderer_uniform_mat4(proj_loc, projection.m);

        for (int i = 0; i < world.capacity; i++) {
            if (world.chunks[i].active &&
                frustum_intersects_aabb(&frustum, world.chunks[i].chunk->min, world.chunks[i].chunk->max)) {
                mat4 model = mat4_identity();
                renderer_uniform_mat4(model_loc, model.m);
                renderer_bind_vao(world.chunks[i].mesh->vao);
                renderer_draw_arrays(R_PRIM_TRIANGLES, 0, (int)world.chunks[i].mesh->vertex_count);
            }
        }

        /* Block highlight outline */
        if (hl_found) {
            renderer_use_program(outline_program);
            mat4 hl_model = mat4_translation((vec3){(float)hl.x, (float)hl.y, (float)hl.z});
            renderer_uniform_mat4(renderer_uniform_location(outline_program, "model"), hl_model.m);
            renderer_uniform_mat4(renderer_uniform_location(outline_program, "view"),  view.m);
            renderer_uniform_mat4(renderer_uniform_location(outline_program, "projection"), projection.m);
            renderer_uniform_vec3(renderer_uniform_location(outline_program, "uColor"), 0.6f, 0.6f, 0.6f);
            renderer_depth_mask(false);
            renderer_polygon_offset(-1.0f, -1.0f);
            renderer_enable(R_CAP_POLYGON_OFFSET_LINE);
            renderer_line_width(3.0f);
            renderer_bind_vao(outline_vao);
            renderer_draw_arrays(R_PRIM_LINES, 0, 24);
            renderer_bind_vao(R_INVALID_HANDLE);
            renderer_line_width(1.0f);
            renderer_disable(R_CAP_POLYGON_OFFSET_LINE);
            renderer_depth_mask(true);
        }

        /* Skybox */
        renderer_depth_mask(false);
        renderer_depth_func(R_FUNC_LEQUAL);
        renderer_disable(R_CAP_CULL_FACE);
        renderer_use_program(skybox_program);
        mat4 inv_proj         = mat4_inverse(projection);
        mat4 view_rot         = view; view_rot.m[12] = view_rot.m[13] = view_rot.m[14] = 0;
        mat4 inv_view_rot     = mat4_transpose(view_rot);
        renderer_uniform_mat4(renderer_uniform_location(skybox_program, "inv_projection"),    inv_proj.m);
        renderer_uniform_mat4(renderer_uniform_location(skybox_program, "inv_view_rotation"), inv_view_rot.m);
        renderer_bind_vao(skybox_vao);
        renderer_draw_arrays(R_PRIM_TRIANGLES, 0, 3);
        renderer_bind_vao(R_INVALID_HANDLE);
        renderer_enable(R_CAP_CULL_FACE);
        renderer_depth_func(R_FUNC_LESS);
        renderer_depth_mask(true);

        /* HUD */
        renderer_disable(R_CAP_DEPTH_TEST);
        renderer_enable(R_CAP_BLEND);
        renderer_blend_func(R_BLEND_SRC_ALPHA, R_BLEND_ONE_MINUS_SRC_ALPHA);
        renderer_use_program(hud_program);
        renderer_bind_vao(hud_vao);
        renderer_bind_buffer(R_BUF_ARRAY, hud_vbo);
        renderer_uniform_vec3(renderer_uniform_location(hud_program, "uColor"), 0.7f, 0.7f, 0.7f);
        renderer_uniform_float(renderer_uniform_location(hud_program, "uAlpha"), 1.0f);
        renderer_draw_arrays(R_PRIM_LINES, 0, 4);

        /* Hotbar */
        int hb_color_loc = renderer_uniform_location(hud_program, "uColor");
        int hb_alpha_loc = renderer_uniform_location(hud_program, "uAlpha");
        renderer_uniform_float(hb_alpha_loc, 1.0f);
        renderer_bind_vao(hotbar_vao);
        renderer_bind_buffer(R_BUF_ARRAY, hotbar_vbo);
        for (int i = 0; i < HOTBAR_SLOTS; i++) {
            BlockType slot = (BlockType)(BLOCK_DIRT + i);
            if (slot == selected_block) renderer_uniform_vec3(hb_color_loc, 1.0f, 1.0f, 1.0f);
            else                        renderer_uniform_vec3(hb_color_loc, 0.3f, 0.3f, 0.3f);
            renderer_draw_arrays(R_PRIM_TRIANGLES, i * 6, 6);
        }
        renderer_bind_vao(R_INVALID_HANDLE);

        renderer_use_program(R_INVALID_HANDLE);
        renderer_bind_buffer(R_BUF_ARRAY, R_INVALID_HANDLE);

        /* Pause overlay */
        if (paused) {
            renderer_enable(R_CAP_BLEND);
            renderer_blend_func(R_BLEND_SRC_ALPHA, R_BLEND_ONE_MINUS_SRC_ALPHA);
            renderer_disable(R_CAP_DEPTH_TEST);
            renderer_disable(R_CAP_CULL_FACE);
            renderer_use_program(hud_program);
            renderer_uniform_vec3(renderer_uniform_location(hud_program, "uColor"), 0.0f, 0.0f, 0.0f);
            renderer_uniform_float(renderer_uniform_location(hud_program, "uAlpha"), 0.3f);
            renderer_bind_vao(overlay_vao);
            renderer_draw_arrays(R_PRIM_TRIANGLES, 0, 3);
            renderer_bind_vao(R_INVALID_HANDLE);
            gui_write_text(&gui, (float)win_width * 0.5f - 54.0f, (float)win_height * 0.5f - 78.0f,
                           "Paused", 4.0f, 0.9f, 0.9f, 0.9f);
            gui_create_button(&gui, (float)win_width * 0.5f - 110.0f, (float)win_height * 0.5f - 20.0f,
                              220.0f, 44.0f, "quit game", quit_button_callback, &running);
            renderer_use_program(R_INVALID_HANDLE);
            renderer_enable(R_CAP_DEPTH_TEST);
            renderer_enable(R_CAP_CULL_FACE);
        }

        /* Debug panel (F3) */
        if (show_debug) {
            renderer_disable(R_CAP_DEPTH_TEST);
            renderer_enable(R_CAP_BLEND);
            renderer_blend_func(R_BLEND_SRC_ALPHA, R_BLEND_ONE_MINUS_SRC_ALPHA);
            renderer_use_program(hud_program);

            int active_chunks = 0;
            for (int i = 0; i < world.capacity; i++) if (world.chunks[i].active) active_chunks++;

            ui_input_t ui_in = {
                .mouse_x     = game_input.mouse_x,
                .mouse_y     = game_input.mouse_y,
                .mouse_down  = game_input.mouse_left,
                .pointer_valid = paused, /* only interactive when paused */
            };
            ui_begin(&debug_ui, &ui_in, (float)win_width, (float)win_height, &game_draw);
            ui_panel_begin(&debug_ui, 10.0f, 10.0f, 240.0f);
            ui_text(&debug_ui, "%.0f fps", (double)fps);
            ui_text(&debug_ui, "pos  %.1f %.1f %.1f",
                    (double)cam_pos.x, (double)cam_pos.y, (double)cam_pos.z);
            ui_progress(&debug_ui, "chunks", (float)active_chunks, (float)world.capacity);
            ui_separator(&debug_ui);
            ui_slider_int(&debug_ui, "render dist", &render_distance, 1, 8);
            ui_panel_end(&debug_ui);
            ui_end(&debug_ui);

            renderer_enable(R_CAP_DEPTH_TEST);
        }

        if (now - last_save_flush >= 5.0) { world_flush_saves(&world); last_save_flush = now; }
        (void)last_fps_update;

        world.render_distance = render_distance;
        renderer_enable(R_CAP_DEPTH_TEST);
        renderer_swap();

        game_input_end_frame(&game_input);
    }

    world_flush_saves(&world);
    world_free(&world);
    renderer_destroy_program(shader_program);
    renderer_destroy_program(hud_program);
    renderer_destroy_program(skybox_program);
    renderer_destroy_program(outline_program);
    renderer_destroy_vao(skybox_vao);   renderer_destroy_buffer(skybox_vbo);
    renderer_destroy_vao(outline_vao);  renderer_destroy_buffer(outline_vbo);
    renderer_destroy_vao(overlay_vao);  renderer_destroy_buffer(overlay_vbo);
    renderer_destroy_vao(hud_vao);      renderer_destroy_buffer(hud_vbo);
    renderer_destroy_vao(hotbar_vao);   renderer_destroy_buffer(hotbar_vbo);
    gui_shutdown(&gui);
    renderer_destroy_texture(tex_array);
    renderer_shutdown();
    ecs_shutdown(&g_ecs);

#ifdef ENABLE_LOGGER
    log_shutdown();
#endif

    window_destroy(win);
    return 0;
}
