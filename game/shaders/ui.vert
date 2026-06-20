#version 450 core

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(push_constant) uniform PushConstants {
    layout(offset = 192) vec2 uScreenSize;
} pc;

layout(location = 0) out vec2 FragUV;
layout(location = 1) out vec4 FragColor;

void main() {
    vec2 ndc = (aPos / pc.uScreenSize) * 2.0 - 1.0;
    /* Flip Y to match OpenGL-compatible viewport (y=height, height=-height) */
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
    FragUV = aUV;
    FragColor = aColor;
}
