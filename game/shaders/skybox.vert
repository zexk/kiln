#version 450 core

layout(location = 0) in vec3 aPos;

layout(push_constant) uniform PushConstants {
    mat4 model;      /* padding - not used, offset 0 */
    mat4 view;       /* offset 64 */
    mat4 projection; /* offset 128 */
} pc;

layout(location = 0) out vec3 pos;

void main() {
    pos = aPos;
    mat4 view_rotation = mat4(mat3(pc.view));
    vec4 clip_pos = pc.projection * view_rotation * vec4(aPos, 1.0);
    gl_Position = clip_pos.xyww;
}
