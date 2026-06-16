#include "linalg.h"

/* --- scalar helpers --- */

float kln_radians(float degrees) {
    return degrees * (KLN_PI / 180.0f);
}

float kln_degrees(float radians) {
    return radians * (180.0f / KLN_PI);
}

float kln_lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

float kln_clampf(float x, float lo, float hi) {
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

/* --- vec2 --- */

vec2_t vec2_add(vec2_t a, vec2_t b) {
    return (vec2_t){a.x + b.x, a.y + b.y};
}

vec2_t vec2_sub(vec2_t a, vec2_t b) {
    return (vec2_t){a.x - b.x, a.y - b.y};
}

vec2_t vec2_scale(vec2_t a, float s) {
    return (vec2_t){a.x * s, a.y * s};
}

float vec2_dot(vec2_t a, vec2_t b) {
    return a.x * b.x + a.y * b.y;
}

float vec2_length(vec2_t a) {
    return sqrtf(vec2_dot(a, a));
}

vec2_t vec2_normalize(vec2_t a) {
    float len = vec2_length(a);
    if (len == 0.0f) {
        return a;
    }
    return vec2_scale(a, 1.0f / len);
}

/* --- vec3 --- */

vec3_t vec3_add(vec3_t a, vec3_t b) {
    return (vec3_t){a.x + b.x, a.y + b.y, a.z + b.z};
}

vec3_t vec3_sub(vec3_t a, vec3_t b) {
    return (vec3_t){a.x - b.x, a.y - b.y, a.z - b.z};
}

vec3_t vec3_scale(vec3_t a, float s) {
    return (vec3_t){a.x * s, a.y * s, a.z * s};
}

vec3_t vec3_neg(vec3_t a) {
    return (vec3_t){-a.x, -a.y, -a.z};
}

float vec3_dot(vec3_t a, vec3_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

vec3_t vec3_cross(vec3_t a, vec3_t b) {
    return (vec3_t){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

float vec3_length(vec3_t a) {
    return sqrtf(vec3_dot(a, a));
}

float vec3_length_sq(vec3_t a) {
    return vec3_dot(a, a);
}

vec3_t vec3_normalize(vec3_t a) {
    float len = vec3_length(a);
    if (len == 0.0f) {
        return a;
    }
    return vec3_scale(a, 1.0f / len);
}

vec3_t vec3_lerp(vec3_t a, vec3_t b, float t) {
    return (vec3_t){
        kln_lerpf(a.x, b.x, t),
        kln_lerpf(a.y, b.y, t),
        kln_lerpf(a.z, b.z, t),
    };
}

float vec3_distance(vec3_t a, vec3_t b) {
    return vec3_length(vec3_sub(a, b));
}

/* --- vec4 --- */

vec4_t vec4_add(vec4_t a, vec4_t b) {
    return (vec4_t){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

vec4_t vec4_sub(vec4_t a, vec4_t b) {
    return (vec4_t){a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

vec4_t vec4_scale(vec4_t a, float s) {
    return (vec4_t){a.x * s, a.y * s, a.z * s, a.w * s};
}

float vec4_dot(vec4_t a, vec4_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

float vec4_length(vec4_t a) {
    return sqrtf(vec4_dot(a, a));
}

vec4_t vec4_normalize(vec4_t a) {
    float len = vec4_length(a);
    if (len == 0.0f) {
        return a;
    }
    return vec4_scale(a, 1.0f / len);
}

/* --- mat4 ---
   Indexing reminder: element (row r, col c) is m[c * 4 + r]. */

mat4_t mat4_identity(void) {
    mat4_t r = {{0}};
    r.m[0] = 1.0f;
    r.m[5] = 1.0f;
    r.m[10] = 1.0f;
    r.m[15] = 1.0f;
    return r;
}

mat4_t mat4_mul(mat4_t a, mat4_t b) {
    mat4_t r;
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = sum;
        }
    }
    return r;
}

mat4_t mat4_transpose(mat4_t a) {
    mat4_t r;
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            r.m[c * 4 + row] = a.m[row * 4 + c];
        }
    }
    return r;
}

mat4_t mat4_inverse(mat4_t a) {
    const float *m = a.m;
    float inv[16];

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] -
             m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] +
             m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] -
             m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] +
              m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] +
             m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] -
             m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] +
             m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] -
              m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] -
             m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] +
             m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] -
              m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] +
              m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] +
             m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] -
             m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] +
              m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] -
              m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) {
        return mat4_identity();
    }
    float inv_det = 1.0f / det;

    mat4_t r;
    for (int i = 0; i < 16; i++) {
        r.m[i] = inv[i] * inv_det;
    }
    return r;
}

mat4_t mat4_translation(vec3_t t) {
    mat4_t r = mat4_identity();
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

mat4_t mat4_scaling(vec3_t s) {
    mat4_t r = {{0}};
    r.m[0] = s.x;
    r.m[5] = s.y;
    r.m[10] = s.z;
    r.m[15] = 1.0f;
    return r;
}

mat4_t mat4_from_quat(quat_t q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;

    mat4_t r = mat4_identity();
    r.m[0] = 1.0f - 2.0f * (yy + zz);
    r.m[1] = 2.0f * (xy + wz);
    r.m[2] = 2.0f * (xz - wy);

    r.m[4] = 2.0f * (xy - wz);
    r.m[5] = 1.0f - 2.0f * (xx + zz);
    r.m[6] = 2.0f * (yz + wx);

    r.m[8] = 2.0f * (xz + wy);
    r.m[9] = 2.0f * (yz - wx);
    r.m[10] = 1.0f - 2.0f * (xx + yy);
    return r;
}

mat4_t mat4_perspective(float fovy, float aspect, float near, float far) {
    float f = 1.0f / tanf(fovy * 0.5f);
    mat4_t r = {{0}};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = far / (near - far);
    r.m[11] = -1.0f;
    r.m[14] = -(far * near) / (far - near);
    return r;
}

mat4_t mat4_ortho(float left, float right, float bottom, float top,
                  float near, float far) {
    mat4_t r = mat4_identity();
    r.m[0] = 2.0f / (right - left);
    r.m[5] = 2.0f / (top - bottom);
    r.m[10] = -1.0f / (far - near);
    r.m[12] = -(right + left) / (right - left);
    r.m[13] = -(top + bottom) / (top - bottom);
    r.m[14] = -near / (far - near);
    return r;
}

mat4_t mat4_look_at(vec3_t eye, vec3_t center, vec3_t up) {
    vec3_t f = vec3_normalize(vec3_sub(center, eye));
    vec3_t s = vec3_normalize(vec3_cross(f, up));
    vec3_t u = vec3_cross(s, f);

    mat4_t r = mat4_identity();
    r.m[0] = s.x;
    r.m[4] = s.y;
    r.m[8] = s.z;
    r.m[1] = u.x;
    r.m[5] = u.y;
    r.m[9] = u.z;
    r.m[2] = -f.x;
    r.m[6] = -f.y;
    r.m[10] = -f.z;
    r.m[12] = -vec3_dot(s, eye);
    r.m[13] = -vec3_dot(u, eye);
    r.m[14] = vec3_dot(f, eye);
    return r;
}

vec4_t mat4_mul_vec4(mat4_t a, vec4_t v) {
    const float *m = a.m;
    return (vec4_t){
        m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w,
        m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w,
        m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
        m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w,
    };
}

vec3_t mat4_transform_point(mat4_t a, vec3_t p) {
    vec4_t r = mat4_mul_vec4(a, (vec4_t){p.x, p.y, p.z, 1.0f});
    return (vec3_t){r.x, r.y, r.z};
}

vec3_t mat4_transform_dir(mat4_t a, vec3_t d) {
    vec4_t r = mat4_mul_vec4(a, (vec4_t){d.x, d.y, d.z, 0.0f});
    return (vec3_t){r.x, r.y, r.z};
}

/* --- quat --- */

quat_t quat_identity(void) {
    return (quat_t){0.0f, 0.0f, 0.0f, 1.0f};
}

quat_t quat_from_axis_angle(vec3_t axis, float angle) {
    vec3_t a = vec3_normalize(axis);
    float half = angle * 0.5f;
    float s = sinf(half);
    return (quat_t){a.x * s, a.y * s, a.z * s, cosf(half)};
}

quat_t quat_mul(quat_t a, quat_t b) {
    return (quat_t){
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

quat_t quat_conjugate(quat_t q) {
    return (quat_t){-q.x, -q.y, -q.z, q.w};
}

float quat_dot(quat_t a, quat_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

quat_t quat_normalize(quat_t q) {
    float len = sqrtf(quat_dot(q, q));
    if (len == 0.0f) {
        return quat_identity();
    }
    float inv = 1.0f / len;
    return (quat_t){q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

vec3_t quat_rotate_vec3(quat_t q, vec3_t v) {
    /* v' = v + 2w(q.xyz x v) + 2(q.xyz x (q.xyz x v)) */
    vec3_t u = {q.x, q.y, q.z};
    vec3_t t = vec3_scale(vec3_cross(u, v), 2.0f);
    return vec3_add(vec3_add(v, vec3_scale(t, q.w)), vec3_cross(u, t));
}

quat_t quat_slerp(quat_t a, quat_t b, float t) {
    float cos_theta = quat_dot(a, b);

    /* Take the shorter arc. */
    if (cos_theta < 0.0f) {
        b = (quat_t){-b.x, -b.y, -b.z, -b.w};
        cos_theta = -cos_theta;
    }

    /* Nearly parallel: fall back to linear interpolation to dodge a
       divide-by-near-zero in the sin() form. */
    if (cos_theta > 0.9995f) {
        quat_t r = {
            kln_lerpf(a.x, b.x, t),
            kln_lerpf(a.y, b.y, t),
            kln_lerpf(a.z, b.z, t),
            kln_lerpf(a.w, b.w, t),
        };
        return quat_normalize(r);
    }

    float theta = acosf(cos_theta);
    float sin_theta = sinf(theta);
    float wa = sinf((1.0f - t) * theta) / sin_theta;
    float wb = sinf(t * theta) / sin_theta;

    return (quat_t){
        wa * a.x + wb * b.x,
        wa * a.y + wb * b.y,
        wa * a.z + wb * b.z,
        wa * a.w + wb * b.w,
    };
}
