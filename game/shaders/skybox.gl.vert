#version 450 core

layout(location = 0) in vec2 aPos;

uniform mat4 inv_projection;
uniform mat4 inv_view_rotation;

layout(location = 0) out vec3 ray_dir;

void main() {
    gl_Position = vec4(aPos, 1.0, 1.0);
    vec4 viewRay = inv_projection * vec4(aPos, 1.0, 1.0);
    ray_dir = (inv_view_rotation * viewRay).xyz;
}
