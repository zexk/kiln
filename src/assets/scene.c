#include "scene.h"

#include <stdio.h>
#include <string.h>

#define SCENE_VERSION 1

bool scene_save(const char *path, const scene_entity_t *entities, int count) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[scene] cannot write '%s'\n", path);
        return false;
    }

    fprintf(f, "# kiln scene v%d\n", SCENE_VERSION);
    for (int i = 0; i < count; i++) {
        const scene_entity_t *e = &entities[i];
        fprintf(f, "%s  %.6f %.6f %.6f  %.6f %.6f %.6f %.6f  %.6f %.6f %.6f\n",
                e->name,
                (double)e->position.x, (double)e->position.y, (double)e->position.z,
                (double)e->rotation.x, (double)e->rotation.y,
                (double)e->rotation.z, (double)e->rotation.w,
                (double)e->scale.x, (double)e->scale.y, (double)e->scale.z);
    }

    fclose(f);
    fprintf(stderr, "[scene] saved %d entities to '%s'\n", count, path);
    return true;
}

int scene_load(const char *path, scene_entity_t *out, int max_count) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[scene] cannot open '%s'\n", path);
        return -1;
    }

    int count = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        if (count >= max_count) {
            fprintf(stderr, "[scene] too many entities (max %d), truncating\n",
                    max_count);
            break;
        }

        scene_entity_t *e = &out[count];
        float px, py, pz, rx, ry, rz, rw, sx, sy, sz;
        /* sscanf width for the name field matches SCENE_NAME_MAX - 1. */
        if (sscanf(line, "%63s %f %f %f %f %f %f %f %f %f %f",
                   e->name, &px, &py, &pz, &rx, &ry, &rz, &rw, &sx, &sy, &sz) != 11) {
            fprintf(stderr, "[scene] skipping malformed line: %s", line);
            continue;
        }
        e->position = (vec3_t){px, py, pz};
        e->rotation = (quat_t){rx, ry, rz, rw};
        e->scale    = (vec3_t){sx, sy, sz};
        count++;
    }

    fclose(f);
    fprintf(stderr, "[scene] loaded %d entities from '%s'\n", count, path);
    return count;
}
