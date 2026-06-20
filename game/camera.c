#include "camera.h"
#include "world.h"
#include "logger.h"
#include "components.h"
#include <math.h>

bool position_is_safe(const World *world, vec3 pos) {
    float hw   = PLAYER_HALF_WIDTH;
    float min_x = pos.x - hw,  max_x = pos.x + hw;
    float min_z = pos.z - hw,  max_z = pos.z + hw;
    float min_y = pos.y - PLAYER_EYES_HEIGHT + 0.01f;
    float max_y = pos.y + (PLAYER_HEIGHT - PLAYER_EYES_HEIGHT) - 0.01f;
    for (int y = (int)floorf(min_y); y <= (int)floorf(max_y); y++) {
        if (world_is_solid(world, (int)floorf(min_x), y, (int)floorf(min_z))) return false;
        if (world_is_solid(world, (int)floorf(min_x), y, (int)floorf(max_z))) return false;
        if (world_is_solid(world, (int)floorf(max_x), y, (int)floorf(min_z))) return false;
        if (world_is_solid(world, (int)floorf(max_x), y, (int)floorf(max_z))) return false;
    }
    return true;
}

bool player_collides_with_block(const World *world, vec3 player_pos, BlockPos block) {
    (void)world;
    float hw = PLAYER_HALF_WIDTH;
    float p_min_x = player_pos.x - hw,  p_max_x = player_pos.x + hw;
    float p_min_z = player_pos.z - hw,  p_max_z = player_pos.z + hw;
    float p_min_y = player_pos.y - PLAYER_EYES_HEIGHT;
    float p_max_y = player_pos.y + (PLAYER_HEIGHT - PLAYER_EYES_HEIGHT);
    float b_min_x = (float)block.x,      b_max_x = (float)block.x + 1.0f;
    float b_min_y = (float)block.y,      b_max_y = (float)block.y + 1.0f;
    float b_min_z = (float)block.z,      b_max_z = (float)block.z + 1.0f;
    return (p_min_x < b_max_x && p_max_x > b_min_x) &&
           (p_min_y < b_max_y && p_max_y > b_min_y) &&
           (p_min_z < b_max_z && p_max_z > b_min_z);
}

void camera_init(Camera *cam, ECS *ecs) {
    cam->front       = (vec3){0.0f, -1.0f, 0.0f};
    cam->up          = (vec3){0.0f,  1.0f, 0.0f};
    cam->yaw         = -90.0f;
    cam->pitch       = -45.0f;
    cam->speed       = 5.0f;
    cam->sensitivity = 0.1f;
    cam->player      = ecs_spawn(ecs);

    C_Transform *t = ecs_add(ecs, cam->player, COMP_TRANSFORM);
    t->position = (vec3){8.0f, 20.0f, 8.0f};
    t->yaw   = cam->yaw;
    t->pitch = cam->pitch;

    C_Movement *m = ecs_add(ecs, cam->player, COMP_MOVEMENT);
    m->velocity  = (vec3){0.0f, 0.0f, 0.0f};
    m->speed     = cam->speed;
    m->grounded  = false;

    C_Health *h = ecs_add(ecs, cam->player, COMP_HEALTH);
    h->current = 20.0f;
    h->max     = 20.0f;
}

static void update_vectors(Camera *cam) {
    vec3 front = {
        cosf(cam->yaw * PI / 180.0f) * cosf(cam->pitch * PI / 180.0f),
        sinf(cam->pitch * PI / 180.0f),
        sinf(cam->yaw * PI / 180.0f) * cosf(cam->pitch * PI / 180.0f),
    };
    cam->front = vec3_normalize(front);
}

void camera_update(Camera *cam, float dt, World *world, GameInput *gi, ECS *ecs) {
    static bool prev_w, prev_a, prev_s, prev_d;

    C_Transform *transform = ecs_get(ecs, cam->player, COMP_TRANSFORM);
    C_Movement  *movement  = ecs_get(ecs, cam->player, COMP_MOVEMENT);
    if (!transform || !movement) return;

    bool keys_w     = gi->keys['w'];
    bool keys_a     = gi->keys['a'];
    bool keys_s     = gi->keys['s'];
    bool keys_d     = gi->keys['d'];
    bool key_space  = gi->keys[' '];
    bool key_shift  = gi->keys[0xe1]; /* XK_Shift_L & 0xFF */
    float mouse_dx  = gi->mouse_dx;
    float mouse_dy  = gi->mouse_dy;

    if (keys_w != prev_w || keys_a != prev_a || keys_s != prev_s || keys_d != prev_d) {
        LOG_DEBUG(CAT_INPUT, "camera key change: w=%d a=%d s=%d d=%d", keys_w, keys_a, keys_s, keys_d);
        prev_w = keys_w; prev_a = keys_a; prev_s = keys_s; prev_d = keys_d;
    }

    cam->yaw   += mouse_dx * cam->sensitivity;
    cam->pitch -= mouse_dy * cam->sensitivity;
    if (cam->pitch >  89.0f) cam->pitch =  89.0f;
    if (cam->pitch < -89.0f) cam->pitch = -89.0f;
    transform->yaw   = cam->yaw;
    transform->pitch = cam->pitch;
    update_vectors(cam);

    float max_step = 0.3f;
    float velocity = cam->speed * dt;
    if (key_shift) velocity *= 6.0f;
    if (velocity > max_step) velocity = max_step;

    vec3 move_dir = {0};
    if (keys_w) move_dir = vec3_add(move_dir, cam->front);
    if (keys_s) move_dir = vec3_sub(move_dir, cam->front);
    vec3 right = vec3_normalize(vec3_cross(cam->front, cam->up));
    if (keys_a) move_dir = vec3_sub(move_dir, right);
    if (keys_d) move_dir = vec3_add(move_dir, right);

    if (move_dir.x != 0 || move_dir.z != 0) {
        move_dir = vec3_normalize(move_dir);
        float new_x = transform->position.x + move_dir.x * velocity;
        float new_z = transform->position.z + move_dir.z * velocity;
        float orig_x = transform->position.x;
        float orig_z = transform->position.z;
        vec3 test = {new_x, transform->position.y, orig_z};
        if (position_is_safe(world, test)) transform->position.x = new_x;
        test.x = orig_x; test.z = new_z;
        if (position_is_safe(world, test)) transform->position.z = new_z;
    }

    if (key_space && movement->grounded) {
        movement->velocity.y = JUMP_VELOCITY;
        movement->grounded   = false;
    }

    gi->mouse_dx = 0;
    gi->mouse_dy = 0;
}

mat4 camera_get_view_matrix(const Camera *cam, ECS *ecs) {
    C_Transform *t = ecs_get(ecs, cam->player, COMP_TRANSFORM);
    if (!t) {
        LOG_WARN(CAT_WORLD, "camera_get_view_matrix: player transform missing!");
        return mat4_identity();
    }
    return mat4_look_at(t->position, vec3_add(t->position, cam->front), cam->up);
}
