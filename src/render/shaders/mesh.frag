#version 450

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_uv;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform Material {
    vec4 base_color;
} mat;
layout(set = 0, binding = 1) uniform sampler2D albedo;

layout(set = 1, binding = 0) uniform SceneUBO {
    vec4 light_dir;     /* xyz: unit direction toward the key light */
    vec4 light_color;   /* xyz: RGB intensity of the key light */
    vec4 ambient_color; /* xyz: ambient fill RGB */
} scene;

void main() {
    vec3 n = normalize(frag_normal);
    float diffuse = abs(dot(n, scene.light_dir.xyz)); /* two-sided */

    vec3 base = mat.base_color.rgb * texture(albedo, frag_uv).rgb;
    vec3 color = base * diffuse * scene.light_color.rgb + base * scene.ambient_color.rgb;
    out_color = vec4(color, 1.0);
}
