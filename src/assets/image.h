#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Set the global vertical-flip flag so uv (0,0) maps to the bottom-left.
   Must be called once before any image_load. */
void image_init(void);

/* Decode an image file into tightly-packed RGBA8 pixels (4 bytes/texel).
   Returns false on failure. Free the result with image_free. */
bool image_load(const char *path, uint8_t **out_pixels, int *out_w, int *out_h);
void image_free(uint8_t *pixels);
