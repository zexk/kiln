#version 450

layout(location = 0) in vec3 frag_normal;

layout(location = 0) out vec4 out_color;

/* Simple two-sided diffuse against a fixed key light plus a cool ambient fill,
   so the test meshes read as solid 3D shapes without any material data. */
void main() {
    vec3 n = normalize(frag_normal);
    vec3 light_dir = normalize(vec3(0.4, 0.85, 0.35));
    float diffuse = abs(dot(n, light_dir)); /* two-sided: show open meshes too */

    vec3 base = vec3(0.82, 0.80, 0.74);
    vec3 ambient = vec3(0.16, 0.18, 0.24);
    vec3 color = base * diffuse + base * ambient;
    out_color = vec4(color, 1.0);
}
