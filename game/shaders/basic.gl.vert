#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(location = 2) in vec3 aNormal;
layout(location = 3) in float aAO;
layout(location = 4) in vec2 aTexCoord;
layout(location = 5) in float aTexLayer;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

layout(location = 0) out vec3 ourColor;
layout(location = 1) out vec3 Normal;
layout(location = 2) out float AO;
layout(location = 3) out vec2 TexCoord;
layout(location = 4) out vec3 view_pos;
layout(location = 5) out float TexLayer;

void main() {
    vec4 world_pos = model * vec4(aPos, 1.0);
    gl_Position = projection * view * world_pos;
    ourColor = aColor;
    Normal = aNormal;
    AO = aAO;
    TexCoord = aTexCoord;
    TexLayer = aTexLayer;
    view_pos = (view * world_pos).xyz;
}
