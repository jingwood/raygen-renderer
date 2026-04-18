# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Platform-specific build folders use plain GNU Make (no top-level build system):

```shell
cd build/mac-m      # or build/mac-intel, build/linux
make                # produces libraygen.a and the `raygen` executable
make clean
```

Xcode project lives at `projects/raygen.xcodeproj`; Windows at `projects/raygen-win32/raygen.sln`.

The makefiles treat the common/graphics headers as siblings of this repo: they add `-I../../../cpp-common-class/src -I../../../cpp-graphics-module/src`. The `inc/cpp-common-class` and `inc/cpp-graphics-module` directories are git submodules — make sure submodules are initialized (`git submodule update --init`) before building, and that the sibling-path includes resolve.

Static archives for the dependencies are pre-built under `lib/<platform>/` (`libucm.a`, `libugm.a`, plus `libjpeg`/`libpng`/`libz` on macOS). Linux uses system `-ljpeg -lpng -lz`.

## Run

```shell
./raygen render <scene.json> [-options]
# e.g. ./raygen render ../../resources/scenes/cubeRoom/cubeRoom.json -s 100 -enaa true
```

Only the `render` command is implemented (see `src/main.cpp:259`). Sample scenes live in `resources/scenes/{cubeRoom,sphereArray,suzanne,test}`. Output defaults to `<scene-basename>.jpg` next to the scene file.

Key CLI flags (full list in README): `-r WxH`, `-s <samples>`, `-c <threads>`, `-d <0|1|2|3|5>` (shader system — 5 = BSDF is default), `-enaa`, `-encs`, `-enpp`, `--focus-obj <name>`, `--dump`.

There are no automated tests — validation is visual, by rendering a sample scene and inspecting the output image.

## Architecture

This is a CPU path tracer. The split is **thin CLI wrapper → static library (`libraygen.a`) → shared C++ utility libs**.

- `src/main.cpp` — only the CLI. Parses args, builds a `RendererSettings`, calls `RendererSceneLoader::load`, then `RayRenderer::render`, then saves the result.
- `src/raygen/` — the library. Everything below lives here.

### Core flow

1. **`RendererSceneLoader`** (`sceneloader.{h,cpp}`) reads the JSON scene into a `Scene`. The loader supports JSONC (comments) and pulls meshes either from primitives (`type: "plane" | "cube" | ...`), `.obj` files, or RayGen's own binary `.mesh` format (see `meshloader.cpp`, `objreader.cpp`).
2. **`Scene`** (`scene.{h,cpp}`) is a tree of `SceneObject`s with transforms, meshes, and materials. `Camera` is a `SceneObject` subclass; `scene.mainCamera` is the render viewpoint.
3. **`RayRenderer`** (`rayrenderer.{h,cpp}`) is the engine. `init()` flattens the scene into world-space triangles and builds a spatial acceleration structure (`SpaceTree` by default; optional `KDTree` gated by `USE_SPACETREE`/`USE_KDTREE` macros — see `rayrenderer.h:23,64,170`). `render()` spawns `settings.threads` worker threads that each call `renderPixel` → `traceEyeRay` → `tracePath`.
4. **Shader provider** (pluggable via `-d`): `RaySimpleShaderProvider`, `RayAmbientOcclusionShaderProvider`, `RayLambertShaderProvider` (`lambert.cpp`), `RayBSDFShaderProvider` (`bsdf.cpp`). Selection happens by `settings.shaderProvider`. The BSDF path combines `DiffuseShader`, `GlossyShader`, `EmissionShader`, `RefractionShader`, `TransparencyShader`, `AnisotropicShader` via a `MixShader`.
5. Results land in `renderingImage` (`Image4f`), optionally post-processed, then written by `saveImage` (from `ugm`).

### Dependency modules (external, under `inc/`)

- **ucm** (`cpp-common-class`) — `ucm::string`, `File`, `Stopwatch`, JSON parser/writer (`jstypes.h`, `jsonwriter.h`), `Archive`, ANSI terminal helpers. Used everywhere; `ucm::string` is the string type, not `std::string`.
- **ugm** (`cpp-graphics-module`) — vector/matrix math (`vec3`, `color3`, `color4`, `BoundingBox`), `Image`/`Image3f`/`Image4f`, image codecs, `SpaceTree`/`KDTree`. The renderer's geometry and image primitives all come from here.

When adding functionality, prefer these libraries' types over STL/third-party equivalents — the codebase is consistent about it.

### Platform notes

- Debug vs release render defaults are chosen by `#ifdef DEBUG` in `rayrenderer.h:30-53` (sample counts, thread count, AA kernel size). Be aware that the same CLI invocation produces different quality on a debug build.
- The `fbxloader.{h,cpp}` + `inc/fbxsdk` is stubbed out — FBX is listed as "under development" in the README.

## Conventions

- C++14, clang++, `-O3 -pthread -Wall`. Apple Silicon build forces `-arch arm64`.
- All engine code is in `namespace raygen`; math/graphics types come from `namespace ugm`; utilities from `namespace ucm`.
- Header guards use `__snake_name_h__` style.
- Scene JSON uses object-keyed maps (the key is the object name), allows `//` comments, and accepts color as `[r,g,b]` floats or `"#rrggbb"` hex strings.
