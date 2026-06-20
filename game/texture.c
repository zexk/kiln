#define STB_IMAGE_IMPLEMENTATION
#include "texture.h"
#include "logger.h"
#include "platform.h"
#include <stb/stb_image.h>
#include <stdio.h>
#include <stdlib.h>

R_Texture texture_load(const char *path) {
    char *resolved = platform_resolve_path(path);
    if (!resolved) return R_INVALID_HANDLE;

    int w, h, ch;
    unsigned char *data = stbi_load(resolved, &w, &h, &ch, 0);
    free(resolved);
    if (!data) { LOG_ERROR(CAT_RENDERER, "Failed to load texture: %s", path); return R_INVALID_HANDLE; }

    R_Texture tex = renderer_create_texture();
    renderer_bind_texture(R_TEX_2D, tex);
    renderer_tex_param(R_TEX_2D, R_TEX_WRAP_S, R_TEX_REPEAT);
    renderer_tex_param(R_TEX_2D, R_TEX_WRAP_T, R_TEX_REPEAT);
    renderer_tex_param(R_TEX_2D, R_TEX_MIN_FILTER, R_TEX_NEAREST);
    renderer_tex_param(R_TEX_2D, R_TEX_MAG_FILTER, R_TEX_NEAREST);
    renderer_tex_image_2d(w, h, data);
    renderer_generate_mipmap();
    stbi_image_free(data);
    LOG_INFO(CAT_RENDERER, "Loaded texture: %s (%dx%d)", path, w, h);
    return tex;
}

R_Texture texture_load_array(const char **paths, int count, int tex_width, int tex_height) {
    if (count <= 0) return R_INVALID_HANDLE;
    R_Texture tex = renderer_create_texture_array(tex_width, tex_height, count);
    if (tex == R_INVALID_HANDLE) { LOG_ERROR(CAT_RENDERER, "Failed to create texture array"); return R_INVALID_HANDLE; }
    renderer_bind_texture(R_TEX_2D, tex);
    renderer_tex_param(R_TEX_2D, R_TEX_WRAP_S, R_TEX_REPEAT);
    renderer_tex_param(R_TEX_2D, R_TEX_WRAP_T, R_TEX_REPEAT);
    renderer_tex_param(R_TEX_2D, R_TEX_MIN_FILTER, R_TEX_NEAREST);
    renderer_tex_param(R_TEX_2D, R_TEX_MAG_FILTER, R_TEX_NEAREST);

    for (int i = 0; i < count; i++) {
        char *resolved = platform_resolve_path(paths[i]);
        if (!resolved) { LOG_WARN(CAT_RENDERER, "Path resolve failed: %s", paths[i]); continue; }
        int w, h, ch;
        unsigned char *data = stbi_load(resolved, &w, &h, &ch, 4);
        free(resolved);
        if (!data) { LOG_WARN(CAT_RENDERER, "Failed to load texture layer %d: %s", i, paths[i]); continue; }
        if (w != tex_width || h != tex_height) {
            LOG_WARN(CAT_RENDERER, "Texture size mismatch: %s (%dx%d vs %dx%d)", paths[i], w, h, tex_width, tex_height);
            stbi_image_free(data); continue;
        }
        renderer_tex_sub_image_array(i, tex_width, tex_height, data);
        stbi_image_free(data);
        LOG_INFO(CAT_RENDERER, "Loaded texture array layer %d: %s", i, paths[i]);
    }
    return tex;
}
