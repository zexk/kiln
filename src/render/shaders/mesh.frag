#version 450

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_uv;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform Material {
    vec4 base_color;
} mat;
layout(set = 0, binding = 1) uniform sampler2D albedo;

/* Albedo = base colour * texture (texture is a 1x1 white pixel for untextured
   materials, so base_color passes through). Two-sided diffuse against a fixed
   key light plus a cool ambient fill. */
void main() {
    vec3 n = normalize(frag_normal);
    vec3 light_dir = normalize(vec3(0.4, 0.85, 0.35));
    float diffuse = abs(dot(n, light_dir)); /* two-sided: show open meshes too */

    vec3 base = mat.base_color.rgb * texture(albedo, frag_uv).rgb;
    vec3 ambient = vec3(0.16, 0.18, 0.24);
    vec3 color = base * diffuse + base * ambient;
    out_color = vec4(color, 1.0);
}
