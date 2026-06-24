#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 ourColor;
layout(location = 1) in vec3 Normal;
layout(location = 2) in float AO;
layout(location = 3) in vec3 TexCoord;
layout(location = 4) in vec3 view_pos;

layout(set = 0, binding = 0) uniform sampler2DArray uTexture;

layout(push_constant) uniform PushConstants {
    layout(offset = 192) vec3  uFogColor;
    layout(offset = 204) float uFogDensity;
} pc;

void main() {
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float diff    = max(dot(Normal, lightDir), 0.0);
    float light   = min(0.3 + diff, 1.0);

    vec4 texColor = texture(uTexture, TexCoord);

    vec3 lit_color = texColor.rgb * ourColor * light * AO;

    float dist = length(view_pos);
    float fog  = clamp(exp(-dist * pc.uFogDensity), 0.0, 1.0);
    vec3 result = mix(pc.uFogColor, lit_color, fog);

    FragColor = vec4(result, 1.0);
}
