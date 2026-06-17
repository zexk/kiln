#include "image.h"

/* stb_image is vendored via the flake (buildInputs). This TU instantiates its
   implementation; it's compiled with warnings off (see CMakeLists) since the
   third-party source doesn't pass -Wall -Wextra. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb/stb_image.h>

#include <stdio.h>

bool image_load(const char *path, uint8_t **out_pixels, int *out_w,
                int *out_h) {
    stbi_set_flip_vertically_on_load(1);
    int channels;
    stbi_uc *pixels = stbi_load(path, out_w, out_h, &channels, 4 /* RGBA */);
    if (!pixels) {
        fprintf(stderr, "[image] failed to load '%s': %s\n", path,
                stbi_failure_reason());
        return false;
    }
    *out_pixels = pixels;
    return true;
}

void image_free(uint8_t *pixels) {
    stbi_image_free(pixels);
}
