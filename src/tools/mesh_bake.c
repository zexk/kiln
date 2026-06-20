#include "kmesh.h"
#include "obj.h"

#include <stdio.h>
#include <string.h>

/* kiln-bake <file.obj> [...]
   Converts each OBJ to a .kmesh file in the same directory. */

static void bake_one(const char *path) {
    /* Build output path: swap .obj suffix for .kmesh. */
    char out[1024];
    size_t len = strlen(path);
    if (len > 4 && path[len - 4] == '.' &&
        path[len - 3] == 'o' && path[len - 2] == 'b' && path[len - 1] == 'j') {
        snprintf(out, sizeof(out), "%.*s.kmesh", (int)(len - 4), path);
    } else {
        snprintf(out, sizeof(out), "%s.kmesh", path);
    }

    cpu_mesh_t mesh;
    if (!obj_load(path, &mesh)) {
        return; /* obj_load already printed the error */
    }
    kmesh_save(out, &mesh);
    cpu_mesh_free(&mesh);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: kiln-bake <file.obj> [...]\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        bake_one(argv[i]);
    }
    return 0;
}
