#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 rayDir;

void main() {
    vec3 horizon = vec3(0.53, 0.81, 0.92);
    vec3 zenith  = vec3(0.20, 0.50, 0.86);

    float y = clamp(normalize(rayDir).y * 0.5 + 0.5, 0.0, 1.0);
    FragColor = vec4(mix(horizon, zenith, y), 1.0);
}
