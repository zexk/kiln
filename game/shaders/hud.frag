#version 450 core

layout(location = 0) out vec4 FragColor;

layout(push_constant) uniform PushConstants {
    layout(offset = 192) vec3 uColor;
    layout(offset = 208) float uAlpha;
} pc;

void main() {
    FragColor = vec4(pc.uColor, pc.uAlpha);
}
