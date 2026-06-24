#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(location = 2) in vec3 aNormal;
layout(location = 3) in float aAO;
layout(location = 4) in vec2 aTexCoord;
layout(location = 5) in float aTexLayer;

layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 view;
    mat4 projection;
} pc;

layout(location = 0) out vec3 ourColor;
layout(location = 1) out vec3 Normal;
layout(location = 2) out float AO;
layout(location = 3) out vec3 TexCoord;
layout(location = 4) out vec3 view_pos;

void main() {
    vec4 world_pos = pc.model * vec4(aPos, 1.0);
    gl_Position = pc.projection * pc.view * world_pos;
    ourColor = aColor;
    Normal   = aNormal;
    AO       = aAO;
    TexCoord = vec3(aTexCoord, aTexLayer);
    view_pos = (pc.view * world_pos).xyz;
}
