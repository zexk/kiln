#version 450

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) in vec3 frag_world_pos;
layout(location = 3) in vec3 frag_tangent;
layout(location = 4) in vec3 frag_bitangent;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform Material {
    vec4 base_color;
} mat;
layout(set = 0, binding = 1) uniform sampler2D albedo;
layout(set = 0, binding = 2) uniform sampler2D normal_map;

struct PointLight {
    vec4 pos;    /* xyz: world position */
    vec4 color;  /* xyz: RGB intensity */
    vec4 params; /* x: radius */
};

layout(set = 1, binding = 0) uniform SceneUBO {
    vec4       light_dir;    /* xyz: dir, w: point_light_count */
    vec4       light_color;
    vec4       ambient_color;
    vec4       view_pos;
    mat4       light_vp;
    PointLight point_lights[8];
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
    /* Build TBN and sample normal map. */
    mat3 tbn = mat3(normalize(frag_tangent),
                    normalize(frag_bitangent),
                    normalize(frag_normal));
    vec3 tn  = texture(normal_map, frag_uv).rgb * 2.0 - 1.0;
    vec3 n   = normalize(tbn * tn);
    vec3 v   = normalize(scene.view_pos.xyz - frag_world_pos);

    /* Directional light + PCF shadow. */
    vec3  l    = normalize(scene.light_dir.xyz);
    vec3  h    = normalize(l + v);
    float ndl  = dot(n, l);
    float diff = abs(ndl);
    float spec = max(ndl, 0.0) * pow(max(dot(n, h), 0.0), 32.0);

    vec4  lc     = scene.light_vp * vec4(frag_world_pos, 1.0);
    vec3  proj   = lc.xyz / lc.w;
    proj.xy      = proj.xy * 0.5 + 0.5;
    float shadow = (proj.z > 1.0) ? 1.0 : pcf_shadow(proj);

    vec3 base  = mat.base_color.rgb * texture(albedo, frag_uv).rgb;
    vec3 color = base  * (shadow * diff * scene.light_color.rgb + scene.ambient_color.rgb)
               + shadow * spec * 0.35 * scene.light_color.rgb;

    /* Point lights. */
    int n_pl = int(scene.light_dir.w);
    for (int i = 0; i < n_pl; i++) {
        vec3  lp  = scene.point_lights[i].pos.xyz - frag_world_pos;
        float d   = length(lp);
        float r   = scene.point_lights[i].params.x;
        if (d >= r) continue;
        float att = 1.0 - (d / r);
        att      *= att;
        vec3  ln  = lp / d;
        vec3  ph  = normalize(ln + v);
        float pd  = max(dot(n, ln), 0.0);
        float ps  = pd * pow(max(dot(n, ph), 0.0), 32.0);
        vec3  plc = scene.point_lights[i].color.rgb;
        color += att * (base * pd * plc + ps * 0.35 * plc);
    }

    out_color = vec4(color, 1.0);
}
