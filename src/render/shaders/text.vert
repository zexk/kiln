#version 450

layout(location = 0) in vec2 in_position; /* screen pixels, top-left origin */
layout(location = 1) in vec3 in_color;

layout(push_constant) uniform PushConstants {
    vec2 screen;
} pc;

layout(location = 0) out vec3 frag_color;

void main() {
    /* pixel -> NDC; drawn under a positive-height viewport so +y is down. */
    vec2 ndc = in_position / pc.screen * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    frag_color = in_color;
}
