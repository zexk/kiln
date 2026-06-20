#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 ourColor;
layout(location = 1) in vec3 Normal;
layout(location = 2) in float AO;
layout(location = 3) in vec2 TexCoord;
layout(location = 4) in vec3 view_pos;
layout(location = 5) in float TexLayer;

layout(binding = 0) uniform sampler2DArray uTexture;

uniform vec3 uFogColor;
uniform float uFogDensity;

void main() {
    // Simple directional lighting
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float diff = max(dot(Normal, lightDir), 0.0);
    
    float ambient = 0.3;
    float light = min(ambient + diff, 1.0);
    
    // Sample texture array and combine with lighting and AO
    vec4 texColor = texture(uTexture, vec3(TexCoord, TexLayer));
    
    // Apply AO and lighting to the texture
    vec3 lit_color = texColor.rgb * light * AO;
    
    // Distance fog
    float dist = length(view_pos);
    float fog = exp(-dist * uFogDensity);
    fog = clamp(fog, 0.0, 1.0);
    vec3 result = mix(uFogColor, lit_color, fog);
    
    FragColor = vec4(result, 1.0);
}
