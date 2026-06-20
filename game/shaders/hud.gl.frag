#version 450 core

layout(location = 0) out vec4 FragColor;

uniform vec3 uColor;
uniform float uAlpha;

void main() {
    FragColor = vec4(uColor, uAlpha);
}
