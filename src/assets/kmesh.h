#pragma once

#include <stdbool.h>

#include "mesh.h"

/* Binary mesh format (.kmesh) — compact, load-time ready.
   On-disk layout (little-endian, no padding):
     kmesh_header_t        (16 bytes)
     float[3] × vcount     positions (always)
     float[2] × vcount     uvs       (only if KMESH_FLAG_HAS_UVS)
     uint16_t × icount     indices   (if KMESH_FLAG_IDX16)
     uint32_t × icount               (otherwise)
   Normals are never stored; recomputed from positions+indices at load. */

#define KMESH_MAGIC   0x484D4C4BU /* 'K','L','M','H' as little-endian u32 */
#define KMESH_VERSION 1

#define KMESH_FLAG_IDX16   (1u << 0) /* indices stored as uint16_t */
#define KMESH_FLAG_HAS_UVS (1u << 1) /* uv block present */

bool kmesh_save(const char *path, const cpu_mesh_t *mesh);
bool kmesh_load(const char *path, cpu_mesh_t *out);
