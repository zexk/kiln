#pragma once

/* Bespoke linear algebra for a right-handed, Vulkan-targeted renderer.

   Conventions, fixed across the whole engine:
     - Right-handed coordinate space; the camera looks down -Z in view space.
     - Angles are radians. Use kln_radians() to convert from degrees.
     - Matrices are 4x4, stored COLUMN-MAJOR in `m[16]`, so element (row r,
       col c) lives at m[c*4 + r] and translation occupies m[12..14]. This is
       the layout Vulkan/GLSL expect, so a mat4_t uploads to a uniform buffer
       verbatim.
     - Composition reads right-to-left: mat4_mul(a, b) applies b then a, and
       mat4_mul_vec4(m, v) computes m*v.
     - Clip space is Vulkan's: depth maps to [0, 1] (not OpenGL's [-1, 1]).

   Y-FLIP CONTRACT: the projection matrices here do NOT negate Y. Vulkan's clip
   space has +Y pointing down, so the renderer is responsible for flipping Y by
   binding a negative-height viewport (VkViewport.height < 0, requires Vulkan
   1.1 / VK_KHR_maintenance1). Do not also flip Y in the matrix or the image
   renders upside down. */

#include <math.h>
#include <stdbool.h>

#define KLN_PI 3.14159265358979323846f

typedef struct {
    float x, y;
} vec2_t;

typedef struct {
    float x, y, z;
} vec3_t;

typedef struct {
    float x, y, z, w;
} vec4_t;

/* Unit quaternion {x, y, z, w} with w the scalar part. Represents an
   active, right-handed rotation. */
typedef struct {
    float x, y, z, w;
} quat_t;

typedef struct {
    float m[16]; /* column-major */
} mat4_t;

/* --- scalar helpers --- */

float kln_radians(float degrees);
float kln_degrees(float radians);
float kln_lerpf(float a, float b, float t);
float kln_clampf(float x, float lo, float hi);

/* --- vec2 --- */

vec2_t vec2_add(vec2_t a, vec2_t b);
vec2_t vec2_sub(vec2_t a, vec2_t b);
vec2_t vec2_scale(vec2_t a, float s);
float vec2_dot(vec2_t a, vec2_t b);
float vec2_length(vec2_t a);
vec2_t vec2_normalize(vec2_t a);

/* --- vec3 --- */

vec3_t vec3_add(vec3_t a, vec3_t b);
vec3_t vec3_sub(vec3_t a, vec3_t b);
vec3_t vec3_scale(vec3_t a, float s);
vec3_t vec3_neg(vec3_t a);
float vec3_dot(vec3_t a, vec3_t b);
vec3_t vec3_cross(vec3_t a, vec3_t b);
float vec3_length(vec3_t a);
float vec3_length_sq(vec3_t a);
vec3_t vec3_normalize(vec3_t a);
vec3_t vec3_lerp(vec3_t a, vec3_t b, float t);
float vec3_distance(vec3_t a, vec3_t b);

/* --- vec4 --- */

vec4_t vec4_add(vec4_t a, vec4_t b);
vec4_t vec4_sub(vec4_t a, vec4_t b);
vec4_t vec4_scale(vec4_t a, float s);
float vec4_dot(vec4_t a, vec4_t b);
float vec4_length(vec4_t a);
vec4_t vec4_normalize(vec4_t a);

/* --- mat4 --- */

mat4_t mat4_identity(void);
mat4_t mat4_mul(mat4_t a, mat4_t b);
mat4_t mat4_transpose(mat4_t a);
mat4_t mat4_inverse(mat4_t a);

mat4_t mat4_translation(vec3_t t);
mat4_t mat4_scaling(vec3_t s);
mat4_t mat4_from_quat(quat_t q);
/* Compose translation * rotation * scale into one model matrix. */
mat4_t mat4_from_trs(vec3_t translation, quat_t rotation, vec3_t scale);

/* Right-handed perspective, vertical FOV in radians, depth in [0, 1].
   See the Y-FLIP CONTRACT above. */
mat4_t mat4_perspective(float fovy, float aspect, float near, float far);
/* Right-handed orthographic, depth in [0, 1]. */
mat4_t mat4_ortho(float left, float right, float bottom, float top,
                  float near, float far);
/* Right-handed view matrix looking from `eye` toward `center`. */
mat4_t mat4_look_at(vec3_t eye, vec3_t center, vec3_t up);

vec4_t mat4_mul_vec4(mat4_t a, vec4_t v);
/* Transform a position (implicit w = 1); ignores any perspective w. */
vec3_t mat4_transform_point(mat4_t a, vec3_t p);
/* Transform a direction (implicit w = 0); unaffected by translation. */
vec3_t mat4_transform_dir(mat4_t a, vec3_t d);

/* --- quat --- */

quat_t quat_identity(void);
quat_t quat_from_axis_angle(vec3_t axis, float angle);
quat_t quat_mul(quat_t a, quat_t b);
quat_t quat_conjugate(quat_t q);
quat_t quat_normalize(quat_t q);
float quat_dot(quat_t a, quat_t b);
vec3_t quat_rotate_vec3(quat_t q, vec3_t v);
quat_t quat_slerp(quat_t a, quat_t b, float t);
