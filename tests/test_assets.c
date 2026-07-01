#include "obj.h"
#include "kmesh.h"
#include "scene.h"
#include "mesh.h"

#include <criterion/criterion.h>
#include <criterion/new/assert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EPS 1e-4f

static bool approx(float a, float b) {
    float d = a - b;
    return (d < 0.0f ? -d : d) < EPS;
}

static bool vec3_approx(vec3_t a, vec3_t b) {
    return approx(a.x, b.x) && approx(a.y, b.y) && approx(a.z, b.z);
}

/* --- scratch dir shared by this test binary's run --- */

static char g_tmpdir[256];

static void make_tmpdir(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/kiln_test_assets_XXXXXX");
    cr_assert(mkdtemp(g_tmpdir) != NULL, "failed to create scratch dir");
}

static const char *tmp_path(const char *name) {
    static char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_tmpdir, name);
    return path;
}

static void write_text_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    cr_assert(f != NULL, "failed to open '%s' for writing", path);
    fputs(content, f);
    fclose(f);
}

/* ============================== obj_load ============================== */

Test(obj, triangulates_a_quad_into_two_triangles, .init = make_tmpdir) {
    const char *path = tmp_path("quad.obj");
    write_text_file(path,
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 1 1 0\n"
        "v 0 1 0\n"
        "f 1 2 3 4\n");

    cpu_mesh_t mesh;
    bool ok = obj_load(path, &mesh);
    cr_assert(ok);
    cr_assert(eq(u32, mesh.vertex_count, 4));
    cr_assert(eq(u32, mesh.index_count, 6));

    cr_assert(vec3_approx(mesh.vertices[0].position, (vec3_t){0, 0, 0}));
    cr_assert(vec3_approx(mesh.vertices[1].position, (vec3_t){1, 0, 0}));
    cr_assert(vec3_approx(mesh.vertices[2].position, (vec3_t){1, 1, 0}));
    cr_assert(vec3_approx(mesh.vertices[3].position, (vec3_t){0, 1, 0}));

    uint32_t expected_idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
        cr_assert(eq(u32, mesh.indices[i], expected_idx[i]), "index %d", i);
    }

    cpu_mesh_free(&mesh);
}

Test(obj, resolves_negative_relative_indices, .init = make_tmpdir) {
    const char *path = tmp_path("neg.obj");
    write_text_file(path,
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f -3 -2 -1\n");

    cpu_mesh_t mesh;
    bool ok = obj_load(path, &mesh);
    cr_assert(ok);
    cr_assert(eq(u32, mesh.vertex_count, 3));
    cr_assert(eq(u32, mesh.index_count, 3));
    cr_assert(vec3_approx(mesh.vertices[0].position, (vec3_t){0, 0, 0}));
    cr_assert(vec3_approx(mesh.vertices[2].position, (vec3_t){0, 1, 0}));

    cpu_mesh_free(&mesh);
}

Test(obj, computes_smooth_normals) {
    /* A flat quad in the XY plane (normal recomputation is smooth, but a
       single flat face degenerates to the same normal on every vertex). */
    const char *path = "/tmp/kiln_test_assets_flat.obj";
    write_text_file(path,
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 1 1 0\n"
        "v 0 1 0\n"
        "f 1 2 3 4\n");

    cpu_mesh_t mesh;
    bool ok = obj_load(path, &mesh);
    cr_assert(ok);
    for (uint32_t i = 0; i < mesh.vertex_count; i++) {
        cr_assert(vec3_approx(mesh.vertices[i].normal, (vec3_t){0, 0, 1}),
                  "vertex %u normal", i);
    }
    cpu_mesh_free(&mesh);
    remove(path);
}

Test(obj, missing_file_fails_and_zeroes_output) {
    cpu_mesh_t mesh;
    mesh.vertices = (mesh_vertex_t *)0x1; /* sentinel to prove memset(0) ran */
    bool ok = obj_load("/nonexistent/path/does/not/exist.obj", &mesh);
    cr_assert(not(ok));
    cr_assert(eq(ptr, (void *)mesh.vertices, NULL));
    cr_assert(eq(u32, mesh.vertex_count, 0));
}

Test(obj, file_with_no_geometry_fails, .init = make_tmpdir) {
    const char *path = tmp_path("empty.obj");
    write_text_file(path, "# just a comment, no v or f lines\n");

    cpu_mesh_t mesh;
    bool ok = obj_load(path, &mesh);
    cr_assert(not(ok));
    cr_assert(eq(u32, mesh.vertex_count, 0));
}

Test(obj, vertices_only_no_faces_fails, .init = make_tmpdir) {
    const char *path = tmp_path("noface.obj");
    write_text_file(path, "v 0 0 0\nv 1 0 0\nv 0 1 0\n");

    cpu_mesh_t mesh;
    bool ok = obj_load(path, &mesh);
    cr_assert(not(ok));
}

/* ============================ kmesh round-trip ========================= */

Test(kmesh, save_then_load_roundtrips_positions_and_indices, .init = make_tmpdir) {
    cpu_mesh_t original;
    cr_assert(cpu_mesh_cube(&original));

    const char *path = tmp_path("cube.kmesh");
    cr_assert(kmesh_save(path, &original));

    cpu_mesh_t loaded;
    cr_assert(kmesh_load(path, &loaded));

    cr_assert(eq(u32, loaded.vertex_count, original.vertex_count));
    cr_assert(eq(u32, loaded.index_count, original.index_count));

    for (uint32_t i = 0; i < original.vertex_count; i++) {
        cr_assert(vec3_approx(loaded.vertices[i].position,
                              original.vertices[i].position),
                  "vertex %u position", i);
    }
    for (uint32_t i = 0; i < original.index_count; i++) {
        cr_assert(eq(u32, loaded.indices[i], original.indices[i]), "index %u", i);
    }

    cpu_mesh_free(&original);
    cpu_mesh_free(&loaded);
}

Test(kmesh, load_rejects_bad_magic, .init = make_tmpdir) {
    const char *path = tmp_path("badmagic.kmesh");
    /* 16 header-sized bytes, all zero — magic won't match KMESH_MAGIC. */
    unsigned char junk[16] = {0};
    FILE *f = fopen(path, "wb");
    cr_assert(f != NULL);
    fwrite(junk, 1, sizeof(junk), f);
    fclose(f);

    cpu_mesh_t mesh;
    bool ok = kmesh_load(path, &mesh);
    cr_assert(not(ok));
}

Test(kmesh, load_rejects_truncated_file, .init = make_tmpdir) {
    cpu_mesh_t original;
    cr_assert(cpu_mesh_cube(&original));
    const char *path = tmp_path("truncated.kmesh");
    cr_assert(kmesh_save(path, &original));
    cpu_mesh_free(&original);

    /* Truncate to just the header. */
    FILE *f = fopen(path, "r+b");
    cr_assert(f != NULL);
    cr_assert(eq(int, ftruncate(fileno(f), 16), 0));
    fclose(f);

    cpu_mesh_t mesh;
    bool ok = kmesh_load(path, &mesh);
    cr_assert(not(ok));
}

Test(kmesh, save_rejects_empty_mesh, .init = make_tmpdir) {
    cpu_mesh_t empty = {0};
    const char *path = tmp_path("empty.kmesh");
    bool ok = kmesh_save(path, &empty);
    cr_assert(not(ok));
}

Test(kmesh, load_missing_file_fails) {
    cpu_mesh_t mesh;
    bool ok = kmesh_load("/nonexistent/path/does/not/exist.kmesh", &mesh);
    cr_assert(not(ok));
}

/* ============================ scene round-trip ========================= */

Test(scene, save_then_load_roundtrips_entities, .init = make_tmpdir) {
    scene_entity_t entities[2];
    memset(entities, 0, sizeof(entities));
    strcpy(entities[0].name, "cube");
    entities[0].position = (vec3_t){1, 2, 3};
    entities[0].rotation = (quat_t){0, 0, 0, 1};
    entities[0].scale    = (vec3_t){1, 1, 1};

    strcpy(entities[1].name, "sphere");
    entities[1].position = (vec3_t){-4, 5, -6};
    entities[1].rotation = (quat_t){0.5f, 0.5f, 0.5f, 0.5f};
    entities[1].scale    = (vec3_t){2, 2, 2};

    const char *path = tmp_path("scene.kscn");
    cr_assert(scene_save(path, entities, 2));

    scene_entity_t loaded[4];
    int n = scene_load(path, loaded, 4);
    cr_assert(eq(int, n, 2));

    cr_assert(eq(str, loaded[0].name, "cube"));
    cr_assert(vec3_approx(loaded[0].position, entities[0].position));
    cr_assert(vec3_approx(loaded[1].position, entities[1].position));
    cr_assert(eq(str, loaded[1].name, "sphere"));
}

Test(scene, load_truncates_to_max_count, .init = make_tmpdir) {
    scene_entity_t entities[3];
    memset(entities, 0, sizeof(entities));
    strcpy(entities[0].name, "a");
    strcpy(entities[1].name, "b");
    strcpy(entities[2].name, "c");

    const char *path = tmp_path("scene3.kscn");
    cr_assert(scene_save(path, entities, 3));

    scene_entity_t loaded[2];
    int n = scene_load(path, loaded, 2);
    cr_assert(eq(int, n, 2));
}

Test(scene, load_missing_file_returns_negative_one) {
    scene_entity_t loaded[4];
    int n = scene_load("/nonexistent/path/does/not/exist.kscn", loaded, 4);
    cr_assert(eq(int, n, -1));
}

Test(scene, load_rejects_bad_magic, .init = make_tmpdir) {
    const char *path = tmp_path("badmagic.kscn");
    unsigned char junk[16] = {0};
    FILE *f = fopen(path, "wb");
    cr_assert(f != NULL);
    fwrite(junk, 1, sizeof(junk), f);
    fclose(f);

    scene_entity_t loaded[4];
    int n = scene_load(path, loaded, 4);
    cr_assert(eq(int, n, -1));
}
