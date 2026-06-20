#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 pos;

void main() {
    vec3 horizon = vec3(0.53, 0.81, 0.92);
    vec3 zenith = vec3(0.2, 0.5, 0.86);
    
    float y = normalize(pos).y;
    y = clamp(y, -1.0, 1.0);
    y = y * 0.5 + 0.5;
    
    vec3 color = mix(horizon, zenith, y);
    FragColor = vec4(color, 1.0);
}
