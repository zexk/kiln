#include "physics.h"

#include <criterion/criterion.h>
#include <criterion/new/assert.h>

#include <math.h>
#include <stdbool.h>

#define EPS 1e-4f

static bool approx(float a, float b) {
    float d = a - b;
    return (d < 0.0f ? -d : d) < EPS;
}

/* --- solid-query fixtures --- */

/* Infinite flat floor: every voxel with y < 0 is solid. */
static bool floor_solid(void *ctx, int x, int y, int z) {
    (void)ctx; (void)x; (void)z;
    return y < 0;
}

/* Never solid: free fall / open space. */
static bool empty_solid(void *ctx, int x, int y, int z) {
    (void)ctx; (void)x; (void)y; (void)z;
    return false;
}

/* A single solid voxel at a fixed point, set via ctx. */
typedef struct { int x, y, z; } point_t;

static bool point_solid(void *ctx, int x, int y, int z) {
    const point_t *p = ctx;
    return x == p->x && y == p->y && z == p->z;
}

/* A vertical wall: every voxel with x == wall_x is solid (ctx holds wall_x). */
static bool wall_x_solid(void *ctx, int x, int y, int z) {
    (void)y; (void)z;
    return x == *(int *)ctx;
}

/* A ceiling: every voxel with y == ceil_y is solid. */
static bool ceiling_solid(void *ctx, int x, int y, int z) {
    (void)x; (void)z;
    return y == *(int *)ctx;
}

/* --- phys_step: gravity --- */

Test(physics, gravity_accelerates_downward_in_free_fall) {
    phys_body_t body = {0};
    body.gravity = 10.0f;
    body.half_w = 0.4f;
    body.foot_off = 0.9f;
    body.head_off = 0.9f;

    phys_step(&body, 1.0f / 60.0f, empty_solid, NULL);

    cr_assert(body.velocity.y < 0.0f, "gravity should pull velocity.y negative");
    cr_assert(not(body.grounded));
}

Test(physics, dt_is_clamped_to_sixtieth_second) {
    /* A huge dt should be clamped internally, so the velocity change from
       one call with dt=1.0 matches one call with dt=1/60. */
    phys_body_t a = {0};
    a.gravity = 10.0f;
    phys_body_t b = a;

    phys_step(&a, 1.0f, empty_solid, NULL);
    phys_step(&b, 1.0f / 60.0f, empty_solid, NULL);

    cr_assert(approx(a.velocity.y, b.velocity.y));
    cr_assert(approx(a.position.y, b.position.y));
}

/* --- phys_step: ground collision --- */

Test(physics, body_lands_and_stops_on_floor) {
    phys_body_t body = {0};
    body.position = (vec3_t){0.0f, 0.5f, 0.0f};
    body.velocity = (vec3_t){0.0f, -20.0f, 0.0f}; /* fast enough to test tunnelling */
    body.gravity  = 10.0f;
    body.half_w   = 0.4f;
    body.foot_off = 0.5f;
    body.head_off = 0.9f;

    /* Step repeatedly, as a game loop would, until it settles. */
    for (int i = 0; i < 120; i++) {
        phys_step(&body, 1.0f / 60.0f, floor_solid, NULL);
    }

    cr_assert(body.grounded);
    cr_assert(approx(body.velocity.y, 0.0f));
    cr_assert(approx(body.position.y, 0.5f), "foot should rest exactly at y=0 (position.y - foot_off == 0)");
}

Test(physics, resting_body_stays_grounded_with_zero_gravity_velocity) {
    phys_body_t body = {0};
    body.position = (vec3_t){0.0f, 0.5f, 0.0f};
    body.gravity  = 10.0f;
    body.half_w   = 0.4f;
    body.foot_off = 0.5f;
    body.head_off = 0.9f;

    for (int i = 0; i < 5; i++) {
        phys_step(&body, 1.0f / 60.0f, floor_solid, NULL);
    }

    cr_assert(body.grounded);
    cr_assert(approx(body.position.y, 0.5f));
}

/* --- phys_step: horizontal collision --- */

Test(physics, wall_blocks_x_movement_and_zeroes_velocity) {
    int wall_x = 2;
    phys_body_t body = {0};
    body.position = (vec3_t){0.0f, 10.0f, 0.0f}; /* high enough to avoid the floor check entirely */
    body.velocity = (vec3_t){5.0f, 0.0f, 0.0f};
    body.half_w   = 0.4f;
    body.foot_off = 0.9f;
    body.head_off = 0.9f;

    float last_x = body.position.x;
    for (int i = 0; i < 60; i++) {
        phys_step(&body, 1.0f / 60.0f, wall_x_solid, &wall_x);
        if (body.velocity.x == 0.0f) break;
        last_x = body.position.x;
    }

    cr_assert(approx(body.velocity.x, 0.0f), "velocity.x should be zeroed on wall contact");
    cr_assert(body.position.x < (float)wall_x - body.half_w + EPS,
              "body should not have penetrated the wall's near face");
    (void)last_x;
}

Test(physics, open_space_does_not_block_x_movement) {
    phys_body_t body = {0};
    body.position = (vec3_t){0.0f, 10.0f, 0.0f};
    body.velocity = (vec3_t){5.0f, 0.0f, 0.0f};
    body.half_w   = 0.4f;
    body.foot_off = 0.9f;
    body.head_off = 0.9f;

    phys_step(&body, 1.0f / 60.0f, empty_solid, NULL);

    cr_assert(approx(body.velocity.x, 5.0f));
    cr_assert(body.position.x > 0.0f);
}

/* --- phys_step: ceiling collision --- */

Test(physics, ceiling_stops_upward_velocity) {
    int ceil_y = 5;
    phys_body_t body = {0};
    body.position = (vec3_t){0.0f, 0.0f, 0.0f};
    body.velocity = (vec3_t){0.0f, 20.0f, 0.0f};
    body.half_w   = 0.4f;
    body.foot_off = 0.9f;
    body.head_off = 0.9f;

    for (int i = 0; i < 60; i++) {
        phys_step(&body, 1.0f / 60.0f, ceiling_solid, &ceil_y);
        if (body.velocity.y == 0.0f) break;
    }

    cr_assert(approx(body.velocity.y, 0.0f));
    cr_assert(not(body.grounded), "hitting a ceiling should not set grounded");
    cr_assert(body.position.y + body.head_off <= (float)ceil_y + EPS,
              "head should not have penetrated the ceiling");
}

/* --- phys_raycast_voxel --- */

Test(physics, raycast_misses_when_nothing_solid_in_range) {
    phys_raycast_hit_t hit = phys_raycast_voxel(
        (vec3_t){0.5f, 0.5f, 0.5f}, (vec3_t){1.0f, 0.0f, 0.0f}, 10.0f,
        empty_solid, NULL);

    cr_assert(not(hit.hit));
}

Test(physics, raycast_hits_solid_voxel_along_positive_x) {
    point_t target = {3, 0, 0};
    phys_raycast_hit_t hit = phys_raycast_voxel(
        (vec3_t){0.5f, 0.5f, 0.5f}, (vec3_t){1.0f, 0.0f, 0.0f}, 10.0f,
        point_solid, &target);

    cr_assert(hit.hit);
    cr_assert(eq(int, hit.x, 3));
    cr_assert(eq(int, hit.y, 0));
    cr_assert(eq(int, hit.z, 0));
    /* face normal should point back toward the ray, i.e. -X */
    cr_assert(eq(int, hit.nx, -1));
    cr_assert(eq(int, hit.ny, 0));
    cr_assert(eq(int, hit.nz, 0));
}

Test(physics, raycast_returns_zero_normal_when_origin_starts_inside_solid) {
    point_t target = {0, 0, 0};
    phys_raycast_hit_t hit = phys_raycast_voxel(
        (vec3_t){0.5f, 0.5f, 0.5f}, (vec3_t){1.0f, 0.0f, 0.0f}, 10.0f,
        point_solid, &target);

    cr_assert(hit.hit);
    cr_assert(eq(int, hit.x, 0));
    cr_assert(eq(int, hit.nx, 0));
    cr_assert(eq(int, hit.ny, 0));
    cr_assert(eq(int, hit.nz, 0));
}

Test(physics, raycast_respects_max_distance) {
    point_t target = {100, 0, 0}; /* far outside max_dist */
    phys_raycast_hit_t hit = phys_raycast_voxel(
        (vec3_t){0.5f, 0.5f, 0.5f}, (vec3_t){1.0f, 0.0f, 0.0f}, 10.0f,
        point_solid, &target);

    cr_assert(not(hit.hit));
}

Test(physics, raycast_hits_along_negative_y) {
    point_t target = {0, -3, 0};
    phys_raycast_hit_t hit = phys_raycast_voxel(
        (vec3_t){0.5f, 0.5f, 0.5f}, (vec3_t){0.0f, -1.0f, 0.0f}, 10.0f,
        point_solid, &target);

    cr_assert(hit.hit);
    cr_assert(eq(int, hit.y, -3));
    cr_assert(eq(int, hit.ny, 1), "normal should point back up (+Y) toward the ray origin");
}

Test(physics, raycast_zero_length_direction_misses) {
    phys_raycast_hit_t hit = phys_raycast_voxel(
        (vec3_t){0.5f, 0.5f, 0.5f}, (vec3_t){0.0f, 0.0f, 0.0f}, 10.0f,
        empty_solid, NULL);

    cr_assert(not(hit.hit));
}
