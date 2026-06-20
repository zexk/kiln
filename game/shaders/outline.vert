#version 450 core

layout(location = 0) in vec3 aPos;

layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec3 uColor;
} pc;

void main() {
    gl_Position = pc.projection * pc.view * pc.model * vec4(aPos, 1.0);
}
