#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec3 in_tangent;

/* Per-instance model matrix, four columns at locations 4-7. */
layout(location = 4) in vec4 inst_c0;
layout(location = 5) in vec4 inst_c1;
layout(location = 6) in vec4 inst_c2;
layout(location = 7) in vec4 inst_c3;

layout(push_constant) uniform PC {
    mat4 proj_view;
} pc;

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec2 frag_uv;
layout(location = 2) out vec3 frag_world_pos;
layout(location = 3) out vec3 frag_tangent;
layout(location = 4) out vec3 frag_bitangent;

void main() {
    mat4 model     = mat4(inst_c0, inst_c1, inst_c2, inst_c3);
    vec4 world     = model * vec4(in_position, 1.0);
    gl_Position    = pc.proj_view * world;
    mat3 nm        = transpose(inverse(mat3(model)));
    frag_normal    = nm * in_normal;
    frag_tangent   = nm * in_tangent;
    frag_bitangent = cross(frag_normal, frag_tangent);
    frag_uv        = in_uv;
    frag_world_pos = world.xyz;
}
