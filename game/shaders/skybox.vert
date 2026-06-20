#version 450 core

/* Full-screen triangle in NDC.  We reconstruct world-space ray directions
   for the fragment shader using the inverse matrices that main.c already
   computes and uploads under the "inv_view_rotation" / "inv_projection"
   uniform names, which the push-constant mapping puts at offsets 64 / 128. */

layout(location = 0) in vec2 aPos;   /* NDC xy of the full-screen triangle */

layout(push_constant) uniform PushConstants {
    mat4 _pad;           /* offset   0 – unused */
    mat4 inv_view_rot;   /* offset  64 – transpose(view_rotation_only) */
    mat4 inv_proj;       /* offset 128 – inverse projection */
} pc;

layout(location = 0) out vec3 rayDir;

void main() {
    /* Render at max depth via the xyw trick */
    gl_Position = vec4(aPos, 0.9999, 1.0);

    /* Reconstruct view-space ray from clip-space position */
    vec4 clip    = vec4(aPos, -1.0, 1.0);
    vec4 view_ray = pc.inv_proj * clip;
    view_ray      = vec4(view_ray.xyz, 0.0);   /* direction, not position */

    /* Rotate into world space */
    rayDir = (pc.inv_view_rot * view_ray).xyz;
}
