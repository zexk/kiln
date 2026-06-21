#version 450

layout(location = 0) in  vec2 frag_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D bloom;

layout(push_constant) uniform PC {
    float exposure;
    float strength;
    float pad0, pad1;
} pc;

void main() {
    vec3 hdr = texture(scene, frag_uv).rgb;
    vec3 bl  = texture(bloom, frag_uv).rgb;
    vec3 col = hdr + bl * pc.strength;
    /* Reinhard tone-mapping + gamma */
    col = vec3(1.0) - exp(-col * pc.exposure);
    out_color = vec4(pow(col, vec3(1.0 / 2.2)), 1.0);
}
