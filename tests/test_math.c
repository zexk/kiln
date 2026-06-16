#include "linalg.h"

#include <criterion/criterion.h>
#include <criterion/new/assert.h>

#include <stdbool.h>

#define EPS 1e-4f

static bool approx(float a, float b) {
    float d = a - b;
    return (d < 0.0f ? -d : d) < EPS;
}

static bool vec3_approx(vec3_t a, vec3_t b) {
    return approx(a.x, b.x) && approx(a.y, b.y) && approx(a.z, b.z);
}

/* --- scalars & vectors --- */

Test(math, radians_degrees_roundtrip) {
    cr_assert(approx(kln_radians(180.0f), KLN_PI));
    cr_assert(approx(kln_degrees(KLN_PI), 180.0f));
}

Test(math, vec3_cross_is_right_handed) {
    vec3_t x = {1, 0, 0};
    vec3_t y = {0, 1, 0};
    /* x cross y = +z in a right-handed basis */
    cr_assert(vec3_approx(vec3_cross(x, y), (vec3_t){0, 0, 1}));
}

Test(math, vec3_normalize_unit_length) {
    vec3_t n = vec3_normalize((vec3_t){3, 0, 4});
    cr_assert(approx(vec3_length(n), 1.0f));
    cr_assert(vec3_approx(n, (vec3_t){0.6f, 0.0f, 0.8f}));
}

/* --- matrix composition / storage --- */

Test(math, identity_leaves_point_unchanged) {
    vec3_t p = {1, 2, 3};
    cr_assert(vec3_approx(mat4_transform_point(mat4_identity(), p), p));
}

Test(math, translation_moves_point) {
    /* Catches transposed indexing in column-major storage: translation must
       live in the m[12..14] column, not the bottom row. */
    mat4_t t = mat4_translation((vec3_t){10, 20, 30});
    cr_assert(vec3_approx(mat4_transform_point(t, (vec3_t){1, 2, 3}),
                          (vec3_t){11, 22, 33}));
}

Test(math, mul_applies_right_operand_first) {
    mat4_t t = mat4_translation((vec3_t){1, 0, 0});
    mat4_t s = mat4_scaling((vec3_t){2, 2, 2});
    /* mul(t, s) = translate after scale: scale (1,1,1)->(2,2,2) then +x */
    mat4_t ts = mat4_mul(t, s);
    cr_assert(vec3_approx(mat4_transform_point(ts, (vec3_t){1, 1, 1}),
                          (vec3_t){3, 2, 2}));
}

Test(math, inverse_round_trips_to_identity) {
    mat4_t a = mat4_mul(mat4_translation((vec3_t){5, -3, 2}),
                        mat4_scaling((vec3_t){2, 4, 0.5f}));
    mat4_t i = mat4_mul(a, mat4_inverse(a));
    mat4_t id = mat4_identity();
    for (int k = 0; k < 16; k++) {
        cr_assert(approx(i.m[k], id.m[k]));
    }
}

/* --- projection: pins down the Vulkan [0,1] depth convention --- */

Test(math, perspective_maps_near_to_zero_far_to_one) {
    float near = 1.0f, far = 100.0f;
    mat4_t p = mat4_perspective(kln_radians(60.0f), 16.0f / 9.0f, near, far);

    vec4_t cn = mat4_mul_vec4(p, (vec4_t){0, 0, -near, 1});
    cr_assert(approx(cn.z / cn.w, 0.0f)); /* OpenGL [-1,1] would give -1 */

    vec4_t cf = mat4_mul_vec4(p, (vec4_t){0, 0, -far, 1});
    cr_assert(approx(cf.z / cf.w, 1.0f));
}

Test(math, perspective_keeps_axis_centered) {
    mat4_t p = mat4_perspective(kln_radians(90.0f), 1.0f, 0.1f, 50.0f);
    vec4_t c = mat4_mul_vec4(p, (vec4_t){0, 0, -5, 1});
    cr_assert(approx(c.x / c.w, 0.0f));
    cr_assert(approx(c.y / c.w, 0.0f));
}

/* --- view: pins down handedness --- */

Test(math, look_at_places_target_down_negative_z) {
    mat4_t v = mat4_look_at((vec3_t){0, 0, 5}, (vec3_t){0, 0, 0},
                            (vec3_t){0, 1, 0});
    /* The world origin sits 5 units in front of the camera, i.e. -Z. */
    cr_assert(vec3_approx(mat4_transform_point(v, (vec3_t){0, 0, 0}),
                          (vec3_t){0, 0, -5}));
}

/* --- quaternions --- */

Test(math, quat_rotates_x_to_y_about_z) {
    quat_t q = quat_from_axis_angle((vec3_t){0, 0, 1}, kln_radians(90.0f));
    cr_assert(vec3_approx(quat_rotate_vec3(q, (vec3_t){1, 0, 0}),
                          (vec3_t){0, 1, 0}));
}

Test(math, quat_matrix_agrees_with_direct_rotation) {
    quat_t q = quat_from_axis_angle((vec3_t){0.3f, 1.0f, -0.5f},
                                    kln_radians(57.0f));
    mat4_t m = mat4_from_quat(q);
    vec3_t v = {1, 2, 3};
    cr_assert(vec3_approx(quat_rotate_vec3(q, v), mat4_transform_dir(m, v)));
}

Test(math, quat_identity_is_no_op) {
    cr_assert(vec3_approx(quat_rotate_vec3(quat_identity(), (vec3_t){4, 5, 6}),
                          (vec3_t){4, 5, 6}));
}

Test(math, slerp_hits_endpoints) {
    quat_t a = quat_identity();
    quat_t b = quat_from_axis_angle((vec3_t){0, 1, 0}, kln_radians(90.0f));
    vec3_t v = {1, 0, 0};
    cr_assert(vec3_approx(quat_rotate_vec3(quat_slerp(a, b, 0.0f), v),
                          quat_rotate_vec3(a, v)));
    cr_assert(vec3_approx(quat_rotate_vec3(quat_slerp(a, b, 1.0f), v),
                          quat_rotate_vec3(b, v)));
}
