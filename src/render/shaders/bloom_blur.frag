#version 450

layout(location = 0) in  vec2 frag_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D dummy;

/* direction = (texel_size_x, 0) for horizontal, (0, texel_size_y) for vertical */
layout(push_constant) uniform PC { vec2 direction; } pc;

const float W[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main() {
    vec3 r = texture(src, frag_uv).rgb * W[0];
    for (int i = 1; i < 5; i++) {
        r += texture(src, frag_uv + pc.direction * float(i)).rgb * W[i];
        r += texture(src, frag_uv - pc.direction * float(i)).rgb * W[i];
    }
    out_color = vec4(r, 1.0);
}
