#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
} pc;

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec2 frag_uv;
layout(location = 2) out vec3 frag_world_pos;

void main() {
    vec4 world     = pc.model * vec4(in_position, 1.0);
    gl_Position    = pc.mvp   * vec4(in_position, 1.0);
    frag_normal    = transpose(inverse(mat3(pc.model))) * in_normal;
    frag_uv        = in_uv;
    frag_world_pos = world.xyz;
}
