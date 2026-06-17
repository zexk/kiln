#pragma once

#include <stdbool.h>

#include "linalg.h"

/* One entity record as it appears in a scene file. The prototype_name buffer
   is owned by the caller; scene_load writes into it using the provided stride. */
#define SCENE_NAME_MAX 64

typedef struct {
    char name[SCENE_NAME_MAX];
    vec3_t position;
    quat_t rotation;
    vec3_t scale;
} scene_entity_t;

/* Write `count` entity records to a text file at `path`. Returns false on I/O
   error; the file is either complete or not written. */
bool scene_save(const char *path, const scene_entity_t *entities, int count);

/* Parse a scene file into `out` (at most `max_count` records). Returns the
   number of entities loaded, or -1 on I/O or parse error. Unknown prototype
   names are preserved verbatim — the caller decides whether to skip them. */
int scene_load(const char *path, scene_entity_t *out, int max_count);
