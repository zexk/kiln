#pragma once

#include "renderer.h"

/* Helpers for building layered (array) textures from image files, the common
   case for voxel/tile games where many materials share a fixed-size atlas of
   per-layer textures.  Decoding goes through kiln_assets' image_load, so the
   global vertical-flip flag (image_init) applies here too; leave it unset for
   top-left-origin tile textures.

   Two entry points:
   - the deduplicating builder (texture_array_add / texture_array_load), which
     interns repeated paths onto shared layers and hands back each path's layer
     as you register it;
   - texture_array_load_paths, a one-shot loader for a ready-made path list. */

#define TEXTURE_ARRAY_MAX 256

typedef struct {
    const char *paths[TEXTURE_ARRAY_MAX]; /* borrowed; must outlive the load */
    int         count;
} texture_array_builder_t;

/* Return the array layer for `path`, registering it if unseen.  Identical
   strings collapse onto one layer.  Returns -1 if `path` is NULL or the
   builder is full.  The pointer is borrowed, not copied. */
int texture_array_add(texture_array_builder_t *b, const char *path);

/* Upload every registered path into a `width` x `height` x count RGBA8 array
   texture (nearest filtering, repeat wrap).  Layers that fail to load or whose
   dimensions don't match are left blank.  Returns R_INVALID_HANDLE on failure. */
R_Texture texture_array_load(const texture_array_builder_t *b,
                             int width, int height);

/* One-shot: build an array texture straight from a path list (no dedup). */
R_Texture texture_array_load_paths(const char **paths, int count,
                                   int width, int height);
