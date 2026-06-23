#include "texture.h"
#include "image.h"
#include "log.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>

int texture_array_add(texture_array_builder_t *b, const char *path) {
    if (!path) return -1;
    for (int i = 0; i < b->count; i++)
        if (strcmp(b->paths[i], path) == 0) return i;
    if (b->count >= TEXTURE_ARRAY_MAX) return -1;
    b->paths[b->count] = path;
    return b->count++;
}

static R_Texture texture_array_create(int width, int height, int count) {
    R_Texture tex = renderer_create_texture_array(width, height, count);
    if (tex == R_INVALID_HANDLE) {
        LOG_ERROR(LOG_CAT_RENDERER, "Failed to create texture array");
        return R_INVALID_HANDLE;
    }
    renderer_bind_texture(R_TEX_2D, tex);
    renderer_tex_param(R_TEX_2D, R_TEX_WRAP_S, R_TEX_REPEAT);
    renderer_tex_param(R_TEX_2D, R_TEX_WRAP_T, R_TEX_REPEAT);
    renderer_tex_param(R_TEX_2D, R_TEX_MIN_FILTER, R_TEX_NEAREST);
    renderer_tex_param(R_TEX_2D, R_TEX_MAG_FILTER, R_TEX_NEAREST);
    return tex;
}

static void texture_array_upload(int layer, const char *path,
                                 int width, int height) {
    char *resolved = platform_resolve_path(path);
    if (!resolved) {
        LOG_WARN(LOG_CAT_RENDERER, "Path resolve failed: %s", path);
        return;
    }
    uint8_t *data = NULL;
    int w = 0, h = 0;
    bool ok = image_load(resolved, &data, &w, &h);
    free(resolved);
    if (!ok) {
        LOG_WARN(LOG_CAT_RENDERER, "Failed to load texture layer %d: %s", layer, path);
        return;
    }
    if (w != width || h != height) {
        LOG_WARN(LOG_CAT_RENDERER, "Texture size mismatch: %s (%dx%d vs %dx%d)",
                 path, w, h, width, height);
        image_free(data);
        return;
    }
    renderer_tex_sub_image_array(layer, width, height, data);
    image_free(data);
}

R_Texture texture_array_load(const texture_array_builder_t *b,
                             int width, int height) {
    if (!b || b->count <= 0) return R_INVALID_HANDLE;
    R_Texture tex = texture_array_create(width, height, b->count);
    if (tex == R_INVALID_HANDLE) return R_INVALID_HANDLE;
    for (int i = 0; i < b->count; i++)
        texture_array_upload(i, b->paths[i], width, height);
    return tex;
}

R_Texture texture_array_load_paths(const char **paths, int count,
                                   int width, int height) {
    if (!paths || count <= 0) return R_INVALID_HANDLE;
    R_Texture tex = texture_array_create(width, height, count);
    if (tex == R_INVALID_HANDLE) return R_INVALID_HANDLE;
    for (int i = 0; i < count; i++)
        texture_array_upload(i, paths[i], width, height);
    return tex;
}
