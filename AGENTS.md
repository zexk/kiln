# AGENTS.md

Agent context for working with this repository.

## Build

```sh
nix develop          # enter dev shell (sets VK_LAYER_PATH, regenerates .clangd)
ninja -C build       # build everything (cmake configure is pre-run in the dev shell)
```

First-time or after CMakeLists changes:

```sh
cmake -B build -G Ninja   # configure
ninja -C build            # build
```

Run the scene editor:

```sh
./build/src/app/kiln
```

Cross-compile for Windows:

```sh
nix build .#win32
```

## Tests

Tests use the [Criterion](https://criterion.readthedocs.io) framework and require `pkgs.criterion` (included in the dev shell).

```sh
ctest --test-dir build          # run all tests
ctest --test-dir build -R ecs   # run a single test suite (ecs | arena | math | physics | core_geom | assets | voxel)
```

Test sources live in `tests/`, one file per suite: `test_ecs.c`, `test_arena.c`, `test_math.c` (`kiln_ecs`/`kiln_core`); `test_physics.c` (`kiln_physics` — AABB collision + voxel DDA raycast); `test_core_geom.c` (`kiln_core` — `aabb`/`frustum`); `test_assets.c` (`kiln_assets` — OBJ/kmesh/scene parsers, round-trips and malformed-input handling); `test_voxel.c` (`kiln_voxel` — greedy meshing face-culling and LOD, exercised without a live Vulkan device since `kv_mesh_upload` is never called).

Build with `-DKILN_SANITIZE=ON` to compile/link with AddressSanitizer + UndefinedBehaviorSanitizer (CI runs this in the `sanitize` job).

## Shaders

GLSL shaders in `src/render/shaders/` are compiled to `.spv` via `glslc` as part of the CMake build. The built `.spv` files land in `build/src/render/shaders/`. The path is baked into `kiln_render` at compile time via `-DKILN_SHADER_DIR`; override with `$KILN_SHADER_DIR` at runtime.

Adding a new shader: add the filename to the `foreach` list in `src/render/CMakeLists.txt`.

## Architecture

Kiln is a C99 game engine targeting Linux/X11 with a Vulkan renderer. Libraries are always built; executables and tests only build when kiln is the top-level CMake project (guarded by `CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME`), so downstream games that include kiln as a subdirectory only pay for the libs.

**Two renderer APIs over one shared Vulkan context (`src/render/`):**
- `render.h` / `render_vk.c` — high-level scene renderer: uploads meshes/textures/materials as opaque handles, queues draw calls per frame, drives the full pipeline (HDR offscreen, cascaded shadow maps, bloom, skybox, GPU particles). This is what `app/` and demos use.
- `renderer.h` / `r_*.c` — low-level abstract renderer: thin Vulkan wrappers around programs, buffers, VAOs, and textures (ported from Kyub). Both layers share a single `g_vk` context (`render_internal.h`); the thin renderer records into the rich renderer's frame via an overlay hook (`renderer_set_overlay_fn`). Used by `voxel/`, `ui/` (hud), `texture/`, and the `app/` overlay.

**UI** (`src/ui/`): `kiln_ui` (`ui.h`) is a backend-agnostic immediate-mode widget set — the caller supplies a `ui_draw_t` rect/text vtable (the editor routes it through the hud backend). `hud.h` is the ready-made backend on the low-level renderer: batched screen-space rects, bitmap text (via `kiln_font8x8`, one quad per lit glyph pixel — no atlas) and buttons, plus `hud_draw()` which hands `kiln_ui` a `ui_draw_t` vtable. It ships and compiles its own HUD shaders (located via `$KILN_UI_SHADER_DIR`).

**Texture** (`src/texture/`): `kiln_texture` (`texture.h`) builds layered/array textures from image files — a deduplicating builder (`texture_array_add`/`texture_array_load`) plus a one-shot `texture_array_load_paths`. Decoding goes through `kiln_assets`' `image_load`.

**ECS** (`src/ecs/`): archetype-based. Components are registered at runtime with `component_register`. Entities move between archetypes on add/remove. Query via `query_iter` / `query_next` / `query_get`. The world owns a bump-arena (`src/core/arena.h`) for structural allocations; per-entity data is heap-allocated separately since it must grow.

**Platform** (`src/platform/`): `window_t` is opaque. X11 and Win32 backends share the same `platform.h` API. Native handles (Display/XID or HINSTANCE/HWND) are exposed through `window_get_native_handles` without requiring platform headers.

**Assets** (`src/assets/`):
- `.kmesh` — binary mesh format (positions, uvs, uint16/uint32 indices; normals recomputed at load).
- `.kscn` — text scene format: one entity per line (`name x y z qx qy qz qw sx sy sz`).
- `kiln-bake` (`src/tools/`) converts OBJ → kmesh. Test models aren't in git; `bash assets/models/fetch.sh` downloads them.

**Physics** (`src/physics/`): `kiln_physics` (`physics.h`) operates against a caller-supplied `phys_solid_fn` voxel query. `phys_step` integrates an AABB body with axis-separated collision; `phys_raycast_voxel` is an Amanatides–Woo DDA returning the first solid voxel hit plus its face normal (for block placement).

**App** (`src/app/`): the scene editor entry point. Links all libraries.

**Demos** (`demos/`): standalone executables; currently only `buffon` (Buffon's needle simulation).

## Environment variables

| Variable | Purpose |
|---|---|
| `KILN_SHADER_DIR` | Override compiled `.spv` search path |
| `KILN_UI_SHADER_DIR` | Override compiled HUD `.spv` search path |
| `KILN_ASSET_DIR` | Override asset root |

## Using kiln as a library

Downstream games use `lib.mkKilnGame` from the flake and set `-DKILN_DIR` pointing at kiln's store path. Without Nix, set `KILN_DIR` manually or add kiln as a git submodule at `extern/kiln`. Library targets (`kiln_core`, `kiln_render`, `kiln_ecs`, etc.) are always available; the app/tools/tests are excluded from subproject builds.

## Tooling notes

- `.clangd` is generated by `scripts/gen_clangd.sh` (auto-run on `nix develop`). Do not edit it manually.
- `compile_commands.json` is symlinked from `build/` to the repo root for editors.
- `nix fmt <file>` or `./format.sh` for Nix file formatting (nixpkgs-fmt).
