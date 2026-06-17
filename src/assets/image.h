#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Decode an image file into tightly-packed RGBA8 pixels (4 bytes/texel). Rows
   are flipped so uv (0,0) maps to the bottom-left (OBJ/OpenGL convention),
   matching what the renderer's sampler expects. Returns false on failure.
   Free the result with image_free. */
bool image_load(const char *path, uint8_t **out_pixels, int *out_w, int *out_h);
void image_free(uint8_t *pixels);
