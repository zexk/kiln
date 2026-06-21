# Kiln

A Vulkan 3D game engine and scene editor written in C99. Kiln is a learning/hobby engine with a clean layered architecture: a low-level render API, an archetype ECS, a small asset pipeline, and a scene editor built on top.

**Kyub** is the bundled voxel game demo that runs on the same engine.

---

## Features

### Renderer (Vulkan, dynamic rendering)
- HDR offscreen target with **bloom** post-processing (threshold → Gaussian blur → Reinhard composite)
- **Cascaded shadow maps** — 3 cascades, PCF 3×3, automatic frustum fitting per cascade
- **Point lights** — up to 8 per frame, quadratic attenuation, Blinn-Phong shading
- **Normal mapping** — tangents computed at upload time from UV deltas; flat default so untextured meshes are unaffected
- **Instanced rendering** — `render_mesh_instanced` draws N copies in one GPU call; shadow and colour passes both instanced
- **Frustum culling** — world-space AABB test per mesh before queueing
- **Skybox** — cubemap upload, fullscreen-triangle depth≤ pipeline, renders into HDR target
- **CPU particle system** — gravity + velocity integration, submits via instancing
- Mipmap generation, anisotropic filtering, wireframe toggle, vsync/FPS-limit control
- Screenshot readback: `render_save_screenshot("out.ppm")` dumps the HDR color image

### ECS
- Archetype-based world with component queries (`query_iter` / `query_next` / `query_get`)
- Used by both the editor (`src/ecs/`) and kyub game layer

### Editor (`src/app/`)
- Orbit camera + **FPS fly mode** (Tab to toggle, WASD/QE)
- Spawn entities from prototype templates; pick by ray-cast (AABB broadphase + per-triangle Möller-Trumbore)
- Transform gizmo: move / rotate / scale
- Scene save / load (`.kscn` format, palette-encoded block data)
- Directional light controls (yaw, pitch, intensity, ambient), background colour
- Debug panel: FPS graph, draw count, camera state, auto-rotate

### Asset pipeline (`src/assets/`)
- **OBJ loader** with smooth-normal computation
- **kmesh** — compact binary mesh format (header + packed positions + UVs + indices); `kiln-bake` converts OBJ → kmesh at build time
- **STB image** for texture loading

### Kyub (`game/`)
- Voxel world with greedy meshing, per-face texture arrays, ambient occlusion
- Chunk loading/unloading, chunk persistence (`.kch` binary format with block palette)
- Gravity, jumping, block break/place with ray-cast
- Uses kiln's ECS, FPS camera, UI, noise, and frustum modules

---

## Build

### Nix (recommended)

```sh
nix develop          # enters the dev shell with all dependencies
ninja -C build       # first run: cmake configures automatically via the shell hook
```

The dev shell provides: `cmake`, `ninja`, `glslc`, `nixd`, `statix`, `deadnix`, `nixfmt`.

### Manual (Linux, X11)

**Dependencies:** Vulkan SDK (headers + loader), X11, glslc (shaderc), stb (header-only)

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
```

**Windows (cross-compile from NixOS):**

```sh
nix build .#kiln-win32
```

---

## Running

```sh
# Editor
./build/src/app/kiln

# Voxel game
./build/game/kyub
```

### Test models

The editor loads meshes from `assets/models/`. They are excluded from git to avoid bloating the repo; fetch them with:

```sh
bash assets/models/fetch.sh
```

This downloads a small set of public-domain OBJ meshes from Alec Jacobson's [common-3d-test-models](https://github.com/alecjacobson/common-3d-test-models) repository and bakes them to `.kmesh`.

---

## Project structure

```
src/
  core/       linalg, ECS-independent math (aabb, frustum, noise, arena, timer)
  platform/   window creation, event loop (X11 / Win32)
  ecs/        archetype ECS (world, archetypes, queries)
  camera/     orbit camera + FPS camera
  render/     Vulkan renderer (render.h public API, render_vk.c implementation)
    shaders/  GLSL sources compiled to SPIR-V at build time
  ui/         immediate-mode UI (panels, sliders, buttons, graphs)
  gizmo/      screen-space transform gizmo
  assets/     OBJ loader, kmesh format, STB image wrapper, scene serialiser
  app/        scene editor (entry point, ECS setup, input, pick, gizmo wiring)
  tools/      kiln-bake (OBJ → kmesh batch converter)

game/         Kyub voxel game (uses kiln ECS, camera, UI, noise, frustum)
tests/        arena, ECS, math unit tests
assets/
  models/     .kmesh files (gitignored OBJ sources, fetch with fetch.sh)
```

---

## Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `KILN_SHADER_DIR` | `build/src/render/shaders` | Override compiled `.spv` search path |
| `KILN_ASSET_DIR` | `assets` | Override asset root |

---

## License

MIT — see [LICENSE](LICENSE).

Test meshes fetched by `fetch.sh` are from Alec Jacobson's common-3d-test-models collection (public domain / CC0 where applicable).
