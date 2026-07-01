#include "aabb.h"
#include "frustum.h"
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

/* --- aabb: construction --- */

Test(aabb, from_minmax_stores_bounds_verbatim) {
    aabb_t box = aabb_from_minmax((vec3_t){-1, -2, -3}, (vec3_t){1, 2, 3});
    cr_assert(vec3_approx(box.min, (vec3_t){-1, -2, -3}));
    cr_assert(vec3_approx(box.max, (vec3_t){1, 2, 3}));
}

Test(aabb, from_center_uses_half_extents) {
    aabb_t box = aabb_from_center((vec3_t){5, 5, 5}, (vec3_t){1, 2, 3});
    cr_assert(vec3_approx(box.min, (vec3_t){4, 3, 2}));
    cr_assert(vec3_approx(box.max, (vec3_t){6, 7, 8}));
}

/* --- aabb: containment / intersection --- */

Test(aabb, contains_point_inside_and_on_boundary) {
    aabb_t box = aabb_from_minmax((vec3_t){0, 0, 0}, (vec3_t){1, 1, 1});
    cr_assert(aabb_contains_point(&box, (vec3_t){0.5f, 0.5f, 0.5f}));
    cr_assert(aabb_contains_point(&box, (vec3_t){0, 0, 0})); /* min corner */
    cr_assert(aabb_contains_point(&box, (vec3_t){1, 1, 1})); /* max corner */
}

Test(aabb, does_not_contain_point_outside) {
    aabb_t box = aabb_from_minmax((vec3_t){0, 0, 0}, (vec3_t){1, 1, 1});
    cr_assert(not(aabb_contains_point(&box, (vec3_t){1.1f, 0.5f, 0.5f})));
    cr_assert(not(aabb_contains_point(&box, (vec3_t){0.5f, -0.1f, 0.5f})));
}

Test(aabb, intersects_overlapping_boxes) {
    aabb_t a = aabb_from_minmax((vec3_t){0, 0, 0}, (vec3_t){2, 2, 2});
    aabb_t b = aabb_from_minmax((vec3_t){1, 1, 1}, (vec3_t){3, 3, 3});
    cr_assert(aabb_intersects(&a, &b));
}

Test(aabb, intersects_touching_faces_counts_as_intersecting) {
    aabb_t a = aabb_from_minmax((vec3_t){0, 0, 0}, (vec3_t){1, 1, 1});
    aabb_t b = aabb_from_minmax((vec3_t){1, 0, 0}, (vec3_t){2, 1, 1});
    cr_assert(aabb_intersects(&a, &b));
}

Test(aabb, does_not_intersect_separated_boxes) {
    aabb_t a = aabb_from_minmax((vec3_t){0, 0, 0}, (vec3_t){1, 1, 1});
    aabb_t b = aabb_from_minmax((vec3_t){2, 0, 0}, (vec3_t){3, 1, 1});
    cr_assert(not(aabb_intersects(&a, &b)));
}

/* --- aabb: ray intersection --- */

Test(aabb, ray_intersect_hits_box_from_outside) {
    aabb_t box = aabb_from_minmax((vec3_t){-1, -1, -1}, (vec3_t){1, 1, 1});
    float t;
    bool hit = aabb_ray_intersect(&box, (vec3_t){-5, 0, 0}, (vec3_t){1, 0, 0}, &t);
    cr_assert(hit);
    cr_assert(approx(t, 4.0f)); /* travels from x=-5 to the near face at x=-1 */
}

Test(aabb, ray_intersect_misses_box_entirely) {
    aabb_t box = aabb_from_minmax((vec3_t){-1, -1, -1}, (vec3_t){1, 1, 1});
    float t;
    bool hit = aabb_ray_intersect(&box, (vec3_t){-5, 5, 0}, (vec3_t){1, 0, 0}, &t);
    cr_assert(not(hit));
}

Test(aabb, ray_intersect_pointing_away_from_box_misses) {
    aabb_t box = aabb_from_minmax((vec3_t){-1, -1, -1}, (vec3_t){1, 1, 1});
    float t;
    bool hit = aabb_ray_intersect(&box, (vec3_t){-5, 0, 0}, (vec3_t){-1, 0, 0}, &t);
    cr_assert(not(hit));
}

Test(aabb, ray_intersect_origin_inside_box_returns_exit_distance) {
    aabb_t box = aabb_from_minmax((vec3_t){-1, -1, -1}, (vec3_t){1, 1, 1});
    float t;
    bool hit = aabb_ray_intersect(&box, (vec3_t){0, 0, 0}, (vec3_t){1, 0, 0}, &t);
    cr_assert(hit);
    cr_assert(approx(t, 1.0f)); /* exits the +X face */
}

/* --- aabb: expand / merge --- */

Test(aabb, expand_grows_to_include_point) {
    aabb_t box = aabb_from_minmax((vec3_t){0, 0, 0}, (vec3_t){1, 1, 1});
    aabb_t grown = aabb_expand(&box, (vec3_t){-2, 5, 0.5f});
    cr_assert(vec3_approx(grown.min, (vec3_t){-2, 0, 0}));
    cr_assert(vec3_approx(grown.max, (vec3_t){1, 5, 1}));
}

Test(aabb, merge_produces_union_bounds) {
    aabb_t a = aabb_from_minmax((vec3_t){0, 0, 0}, (vec3_t){1, 1, 1});
    aabb_t b = aabb_from_minmax((vec3_t){-1, 2, -3}, (vec3_t){0.5f, 3, 4});
    aabb_t m = aabb_merge(&a, &b);
    cr_assert(vec3_approx(m.min, (vec3_t){-1, 0, -3}));
    cr_assert(vec3_approx(m.max, (vec3_t){1, 3, 4}));
}

/* --- frustum --- */

/* Camera at (0,0,5) looking down -Z at the origin, matching the convention
   pinned down in test_math.c's look_at test. fov 90deg, aspect 1, near 0.1,
   far 100 so boxes at moderate distances are comfortably inside. */
static frustum_t make_test_frustum(void) {
    mat4_t view = mat4_look_at((vec3_t){0, 0, 5}, (vec3_t){0, 0, 0}, (vec3_t){0, 1, 0});
    mat4_t proj = mat4_perspective(kln_radians(90.0f), 1.0f, 0.1f, 100.0f);
    mat4_t vp = mat4_mul(proj, view);
    frustum_t f;
    frustum_extract(&f, vp);
    return f;
}

Test(frustum, box_at_origin_is_inside) {
    frustum_t f = make_test_frustum();
    cr_assert(frustum_intersects_aabb(&f, (vec3_t){-0.5f, -0.5f, -0.5f},
                                      (vec3_t){0.5f, 0.5f, 0.5f}));
}

Test(frustum, box_behind_camera_is_outside) {
    frustum_t f = make_test_frustum();
    /* z = 10..11 is behind the camera at z=5 looking toward -Z */
    cr_assert(not(frustum_intersects_aabb(&f, (vec3_t){-0.5f, -0.5f, 10.0f},
                                          (vec3_t){0.5f, 0.5f, 11.0f})));
}

Test(frustum, box_far_beyond_far_plane_is_outside) {
    frustum_t f = make_test_frustum();
    cr_assert(not(frustum_intersects_aabb(&f, (vec3_t){-0.5f, -0.5f, -200.0f},
                                          (vec3_t){0.5f, 0.5f, -199.0f})));
}

Test(frustum, box_far_to_the_side_is_outside) {
    frustum_t f = make_test_frustum();
    cr_assert(not(frustum_intersects_aabb(&f, (vec3_t){500.0f, -0.5f, -1.0f},
                                          (vec3_t){501.0f, 0.5f, -1.0f})));
}

Test(frustum, box_straddling_near_plane_boundary_is_inside) {
    frustum_t f = make_test_frustum();
    /* Camera is at z=5 looking toward -Z; near plane sits at z = 5 - 0.1 = 4.9.
       A box straddling that boundary should still test as intersecting. */
    cr_assert(frustum_intersects_aabb(&f, (vec3_t){-0.1f, -0.1f, 4.5f},
                                      (vec3_t){0.1f, 0.1f, 5.3f}));
}
