#pragma once

#include <stdbool.h>

#include "mesh.h"

/* Load a Wavefront .obj into a CPU mesh. Parses vertex positions and triangle
   faces (fan-triangulating any polygons); ignores texcoords, normals (always
   recomputed smooth), materials and groups. Returns false and leaves `out`
   zeroed on any error. Caller owns the result — release with cpu_mesh_free. */
bool obj_load(const char *path, cpu_mesh_t *out);
