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
    vec4 light_dir;
    vec4 light_color;
    vec4 ambient_color;
    vec4 view_pos;
    mat4 light_vp;
} scene;
layout(set = 1, binding = 1) uniform sampler2DShadow shadow_map;

float pcf_shadow(vec3 proj) {
    vec2 texel = 1.0 / vec2(textureSize(shadow_map, 0));
    float s = 0.0;
    for (int x = -1; x <= 1; x++)
        for (int y = -1; y <= 1; y++)
            s += texture(shadow_map, vec3(proj.xy + vec2(x, y) * texel, proj.z - 0.002));
    return s / 9.0;
}

void main() {
    vec3 n = normalize(frag_normal);
    vec3 l = normalize(scene.light_dir.xyz);
    vec3 v = normalize(scene.view_pos.xyz - frag_world_pos);
    vec3 h = normalize(l + v);

    float ndotl   = dot(n, l);
    float diffuse = abs(ndotl);
    float spec    = max(ndotl, 0.0) * pow(max(dot(n, h), 0.0), 32.0);

    /* Shadow: project frag_world_pos into the light's clip space. */
    vec4 lc   = scene.light_vp * vec4(frag_world_pos, 1.0);
    vec3 proj = lc.xyz / lc.w;
    proj.xy   = proj.xy * 0.5 + 0.5;
    float shadow = (proj.z > 1.0) ? 1.0 : pcf_shadow(proj);

    vec3 base  = mat.base_color.rgb * texture(albedo, frag_uv).rgb;
    vec3 color = base  * (shadow * diffuse * scene.light_color.rgb + scene.ambient_color.rgb)
               + shadow * spec * 0.35 * scene.light_color.rgb;
    out_color = vec4(color, 1.0);
}
