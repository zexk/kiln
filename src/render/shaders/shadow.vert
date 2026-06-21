#version 450

layout(location = 0) in vec3 in_position;

layout(push_constant) uniform PC {
    mat4 light_mvp;
} pc;

void main() {
    gl_Position = pc.light_mvp * vec4(in_position, 1.0);
}
