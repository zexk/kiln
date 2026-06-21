#version 450

layout(location = 0) in vec3 in_position;

layout(location = 4) in vec4 inst_c0;
layout(location = 5) in vec4 inst_c1;
layout(location = 6) in vec4 inst_c2;
layout(location = 7) in vec4 inst_c3;

layout(push_constant) uniform PC {
    mat4 light_vp;
} pc;

void main() {
    mat4 model  = mat4(inst_c0, inst_c1, inst_c2, inst_c3);
    gl_Position = pc.light_vp * model * vec4(in_position, 1.0);
}
