#version 450

/* No vertex buffer. Generates a fullscreen triangle; depth forced to 1.0. */
layout(location = 0) out vec3 frag_dir;

layout(push_constant) uniform PC {
    mat4 inv_proj;
    mat4 inv_view_rot; /* view matrix with translation zeroed */
} pc;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    /* NDC position, depth = 1 (far plane) */
    vec4 clip = vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    /* Unproject to view space, then rotate to world space */
    vec4 view = pc.inv_proj * clip;
    view.w = 0.0;
    frag_dir = (pc.inv_view_rot * view).xyz;
    /* Write at maximum depth so skybox is behind everything */
    gl_Position = vec4(uv * 2.0 - 1.0, 0.9999, 1.0);
}
