#version 450 core

layout(location = 0) in vec2 FragUV;
layout(location = 1) in vec4 FragColor;
layout(location = 0) out vec4 OutColor;

layout(set = 0, binding = 0) uniform sampler2D font_tex;

void main() {
    vec4 texColor = texture(font_tex, FragUV);
    OutColor = FragColor * texColor;
}
