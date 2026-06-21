#version 450

layout(location = 0) in  vec3 frag_dir;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform samplerCube skybox;

void main() {
    out_color = texture(skybox, frag_dir);
}
