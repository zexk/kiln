#version 450

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) in vec3 frag_world_pos;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform Material {
    vec4 base_color;
} mat;
layout(set = 0, binding = 1) uniform sampler2D albedo;

layout(set = 1, binding = 0) uniform SceneUBO {
    vec4 light_dir;     /* xyz: unit direction toward the key light */
    vec4 light_color;   /* xyz: RGB intensity of the key light */
    vec4 ambient_color; /* xyz: ambient fill RGB */
    vec4 view_pos;      /* xyz: camera world-space position */
} scene;

void main() {
    vec3 n = normalize(frag_normal);
    vec3 l = normalize(scene.light_dir.xyz);
    vec3 v = normalize(scene.view_pos.xyz - frag_world_pos);
    vec3 h = normalize(l + v);

    float ndotl   = dot(n, l);
    float diffuse = abs(ndotl);                               /* two-sided */
    float spec    = max(ndotl, 0.0) * pow(max(dot(n, h), 0.0), 32.0);

    vec3 base  = mat.base_color.rgb * texture(albedo, frag_uv).rgb;
    vec3 color = base  * (diffuse * scene.light_color.rgb + scene.ambient_color.rgb)
               + spec  * 0.35 * scene.light_color.rgb;
    out_color = vec4(color, 1.0);
}
