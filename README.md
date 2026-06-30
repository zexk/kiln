# Kiln

C99 game engine. Vulkan renderer, archetype ECS, asset pipeline, scene editor. Linux/Win32.

Kyub (the voxel game built on it) lives at [zexk/kyub](https://github.com/zexk/kyub).

## Modules

**render**: Vulkan, dynamic rendering, no render pass objects. HDR offscreen target, bloom (threshold + gaussian + Reinhard), cascaded shadow maps (3 cascades, PCF 3x3), point lights (up to 8, Blinn-Phong), normal mapping, instanced draws, frustum culling, skybox, CPU particles, screenshot readback.

**ecs**: archetype-based world. `query_iter` / `query_next` / `query_get`.

**voxel**: chunked voxel world — greedy meshing, per-LOD on-demand remesh, distance-sorted streaming, O(1) slot lookup.

**settings**: engine + key-binding settings with INI persistence (vsync, fps limit, fov, mouse sensitivity, bloom, rebindable actions).

**texture**: deduplicating array/layered texture builder over `kiln_assets` image decode.

**platform**: window, event loop. X11 and Win32 backends.

**core**: linalg, AABB, frustum, noise (Perlin + fBm), arena allocator, timer, logging, file I/O.

**camera**: orbit and FPS.

**ui**: immediate-mode panels, sliders, buttons, text, graph strip.

**gizmo**: screen-space translate/rotate/scale gizmo.

**assets**: OBJ loader, kmesh binary format, STB image, scene serializer (.kscn).

**app**: scene editor. Orbit/FPS camera, entity spawn from templates, ray-cast pick (AABB + Moller-Trumbore), transform gizmo, scene save/load, light controls, debug panel.

**physics**: AABB voxel collision.

## Build

```sh
nix develop
ninja -C build
```

Without Nix (Linux, X11, Vulkan SDK required):

```sh
cmake -B build -G Ninja
ninja -C build
```

Cross-compile for Windows from Linux:

```sh
nix build .#win32
```

## Run

```sh
./build/src/app/kiln        # scene editor
```

Test models are not in git. Fetch them:

```sh
bash assets/models/fetch.sh
```

Downloads CC0 OBJ meshes from [alecjacobson/common-3d-test-models](https://github.com/alecjacobson/common-3d-test-models) and bakes to .kmesh.

## Demos

**Buffon's Needle**: 3D physics simulation estimating pi by dropping needles onto a ruled plane. WASD/mouse to fly, `+`/`-` to scale speed, `Space` to pause, `R` to reset.

```sh
./build/demos/buffon/buffon        # Linux
nix build .#buffon                 # Linux (Nix)
nix build .#buffon-win32           # Windows cross-compile
```

## As a library

Games can pull kiln in as a Nix flake input and use `lib.mkKilnGame`:

```nix
inputs.kiln.url = "github:zexk/kiln";

packages.default = kiln.lib.mkKilnGame {
  inherit pkgs;
  pname = "mygame";
  src = ./.;
};
```

The game's `CMakeLists.txt` receives `-DKILN_DIR` pointing at kiln's store path and links against `kiln_renderer`, `kiln_ecs`, `kiln_core`, etc. For local builds without Nix, set `KILN_DIR` manually or use kiln as a git submodule at `extern/kiln`.

## Structure

```
src/
  core/       math, noise, AABB, frustum, arena, timer, log, file I/O
  platform/   X11 / Win32 window and events
  ecs/        archetype ECS
  camera/     orbit + FPS camera
  render/     Vulkan renderer + shaders
  voxel/      chunked voxel world (meshing, LOD, streaming)
  ui/         immediate-mode UI
  gizmo/      transform gizmo
  assets/     OBJ, kmesh, STB image, scene format
  texture/    array/layered texture builder
  settings/   engine + key-binding settings (INI)
  physics/    AABB voxel collision
  app/        scene editor (entry point)
  tools/      kiln-bake (OBJ -> kmesh)
demos/        standalone demos (Buffon's needle, ...)
tests/        unit tests (arena, ECS, math)
assets/
  models/     .kmesh files (gitignored; fetch.sh downloads OBJ sources)
```

## Environment variables

| Variable | Purpose |
|---|---|
| `KILN_SHADER_DIR` | override compiled .spv search path |
| `KILN_UI_SHADER_DIR` | override compiled HUD .spv search path |
| `KILN_ASSET_DIR` | override asset root |

## License

MIT. Test meshes from alecjacobson/common-3d-test-models (CC0).
