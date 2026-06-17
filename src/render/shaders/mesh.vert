#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
} pc;

layout(location = 0) out vec3 frag_normal;

void main() {
    gl_Position = pc.mvp * vec4(in_position, 1.0);
    /* World-space normal. Assumes uniform scale (true for our normalized test
       models); a proper inverse-transpose comes with non-uniform scaling. */
    frag_normal = mat3(pc.model) * in_normal;
}
