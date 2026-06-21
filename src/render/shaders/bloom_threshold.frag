#version 450

layout(location = 0) in  vec2 frag_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D dummy; /* unused */

layout(push_constant) uniform PC {
    float threshold;
    float pad0, pad1, pad2;
} pc;

void main() {
    vec3 c   = texture(src, frag_uv).rgb;
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float k   = max(lum - pc.threshold, 0.0) / max(lum, 0.001);
    out_color = vec4(c * k, 1.0);
}
