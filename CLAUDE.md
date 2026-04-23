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

The makefiles treat the common/graphics headers as siblings of this repo: they add `-I../../../cpp-common-class/src -I../../../cpp-graphics-math/src`. Those sibling paths are submodules — initialize with `git submodule update --init` before building.

Static archives for the dependencies are pre-built under `lib/<platform>/` (`libucm.a`, `libugm.a`, plus `libjpeg`/`libpng`/`libz` on macOS). Linux uses system `-ljpeg -lpng -lz`.

The release build uses `-O3 -ffast-math -flto -mcpu=apple-m1` on Apple Silicon. After any stray/incremental build oddity (e.g., seeing stale crashes that disappear with source changes) prefer `make clean && make` — LTO caches sometimes bite.

## Run

```shell
./raygen render <scene.json> [-options]
# e.g. ./raygen render ../../resources/scenes/iblTest/iblTest.json -s 100 -enaa true -c 8
```

Only the `render` command is implemented (see `src/main.cpp:259`). Sample scenes live in `resources/scenes/`:

- `cubeRoom/`, `sphereArray/`, `suzanne/`, `test/` — classic Cornell-style and primitive test scenes.
- `bokehTest/` — circular/hex bokeh exercise (background emitter field).
- `anisoTest/` — three flat panels comparing isotropic vs. ±anisotropic GGX.
- `iblTest/` — glass/diffuse/glossy spheres on a checker floor lit by an equirect HDR sky.
- `tobaTest/` — loads `.toba` bundles (chair, showroom) as real assets.

Output defaults to `<scene-basename>.jpg` next to the scene file.

Key CLI flags (full list in README): `-r WxH`, `-s <samples>`, `-c <threads>`, `-d <0|1|2|3|5>` (shader system — 5 = BSDF, the default and what this guide assumes), `-enaa true|false` (really toggles AA jitter now), `-encs`, `-enpp`, `--focus-obj <name>`, `--dump`.

`--dump` prints each scene object's resolved material (color, glossy, roughness, metallic, anisotropy, refraction, transparency, emission, texture presence) — useful for verifying bundle overrides.

There are no automated tests — validation is visual, by rendering a sample scene and inspecting the output image.

## Viewer (raygen-viewer)

Interactive tuner built on Dear ImGui + GLFW + OpenGL 3.2 core. Loads a scene.json, runs raygen on a worker thread, uploads results into a GL texture, and exposes every commonly-tuned parameter as a slider with live re-render. Primary use case: dialing in materials / exposure / bloom without the edit-save-re-run loop of the CLI.

### Build

Windows: `projects/raygen-viewer-win32/raygen-viewer.sln` (VS 2026, v145 toolset). The solution pulls in `raygen-core` (a `StaticLibrary` split of the renderer sources — `raygen.vcxproj` is just `main.cpp` + `raygen-core.lib` now, mirroring the `libraygen.a` layout the makefiles already use on mac/linux). GLFW is staged at `D:\Libs\glfw\glfw-3.4.bin.WIN64\` by default — override the `IncludePath`/`LibraryPath` if your prebuilt lives elsewhere.

`lib/windows/` collects every linker input (libucm, libugm, jpeg, libpng, zlib, glfw3). `zlib.dll` must sit next to the exe — zlib's DLL reports its import name as `zlib.dll`, so copying `D:\Libs\zlib-1.2.11\build\Release\zlib64.dll` as `zlib.dll` is the canonical fix. The same applies to `raygen.exe`.

Dear ImGui is a submodule at `inc/imgui` (pinned to v1.91.5); `git submodule update --init --recursive` after cloning.

Mac/Linux viewer projects aren't wired up yet — when adding them, link against the existing `libraygen.a` target and `brew install glfw` / `pkg-config --cflags --libs glfw3`. The viewer sources (`src/viewer/`) are already platform-neutral; only the build script is missing.

### Run

```shell
raygen-viewer.exe [path/to/scene.json]
```

Default scene path is `F:\3D Models\F2\raygen_export\F2.json` (change the default in `src/viewer/main.cpp` if that directory isn't on your machine). HiDPI: respects the monitor content scale reported by GLFW (Windows 150% / 200% etc.); override manually with `RAYGEN_UI_SCALE=2.0`.

### Panels

- **raygen viewer** — status line (green "ready" / yellow "tracing N%" / blue "post-processing...") + progress bar. The "x" button to the right of `tracing N%` cancels the in-flight render (cooperative — returns within a few ms at row granularity). Below that the control sections live under three `CollapsingHeader`s:
  - **Quality**: `samples` (1..1000), `threads` (1..32), `denoise` toggle, `denoise intensity` (0..1).
  - **Scene**: `exposure` (0.1..3), `envmap intensity` (0..3), `envmap rotation` (0..360).
  - **Post-process (bloom)**: enable toggle, `bloom threshold`, `bloom strength`, `bloom curve`.
- **Output** — width/height (step 10 / fast-step 100, clamped 16..8192), `Apply`, and auto-apply presets 480p/720p/1080p/2K/4K. `Apply` is disabled while the worker is running because `setRenderSize` reallocates `renderingImage`.
- **File** — `Reload scene` re-parses the JSON into a fresh `Scene` (useful when editing scene.json in your IDE). `output path` defaults to `<scene-basename>-out.jpg`; `Save render` writes the current `RayRenderer` result via `saveImage` (format from extension).
- **render** — displays the GL texture holding the latest rendered frame. Mouse-wheel over the window zooms in 1.1x steps (0.05..16x); `1:1` and `fit` buttons for quick reset.

### Architecture

A single worker thread owns `RayRenderer::render()`. The main thread holds the `Scene` (via `std::unique_ptr<Scene>` so reload can swap it), the `RayRenderer`, the GL context, and the ImGui state. Coordination is one mutex + condvar around a `RenderJob` struct.

**Job kinds:**

- `JobKind::Full` — trace + denoise + bloom. This is what runs for samples / exposure / envmap changes, and on a first render. The worker installs a throttled (`200 ms`) `progressCallback` that packs the in-flight `renderingImage` into RGBA and flags `ready = true` mid-render — that's the progressive preview you see converge.
- `JobKind::PostOnly` — just re-runs the bloom pass. Uses `RayRenderer::reapplyPostProcess()`, which restores `renderingImage` from the cached `preBloomImage` (snapshotted at the end of the previous full render, right before bloom) and re-applies bloom with current settings. Typically **a few ms**. No progress callback — the bar is pinned at full during post-only.

**Dispatch:** `onlyPostProcessChanged(lastKickedParams, uiParams)` decides Full vs PostOnly. Any non-bloom field delta forces Full. The `(next: post-only|full)` label on the control panel previews the dispatch decision before it fires.

**Dirty handling** is one kick site per frame. `pendingDirty` is set whenever a slider moves and cleared when we actually enqueue. An earlier two-site split (immediate + pending) raced on the same frame when a PostOnly finished mid-frame: it kicked a correct PostOnly, then immediately kicked a spurious Full whose `kickKind` comparison saw no delta and fell through to the default. Don't re-split it.

**Settings are written into `renderer.settings` directly**, not into an outside `RendererSettings`. The renderer copies its settings by value at construction (`this->settings = *settings;`), so mutating the original struct post-construction is a no-op. `applyParamsToScene` touches `renderer.settings.*`, `scene->envmapIntensity/Rotation`, and `scene->mainCamera->exposure`.

**Cancellation** is cooperative: `RayRenderer::cancelRequested` (atomic<bool>) is cleared at the start of every `render()`, each render thread checks it at row boundaries, and `render()` also short-circuits before denoise/bloom if cancellation fired. Cancelled renders leave `hasPreBloomImage` untouched so subsequent PostOnly operations reuse the last complete frame. PostOnly itself has no cancel path (too fast to matter).

**Scene reload** swaps the `std::unique_ptr<Scene>`. Safe only while the worker is idle (button disabled otherwise); the worker dereferences `*scene` at job start, so a pointer-swap from the main thread is picked up on the next job.

### Viewer-driven core changes to remember

- `RayRenderer::reapplyPostProcess()` / `applyPostProcess()` (private) / `preBloomImage` / `hasPreBloomImage` in `rayrenderer.{h,cpp}`. `setRenderSize` clears `hasPreBloomImage` so the cache doesn't outlive a buffer realloc.
- `RayRenderer::cancelRequested` (atomic<bool>), cleared at the top of `render()`.
- `ObjFileReader::readSurfaceLine` now fan-triangulates arbitrary n-gons (Blender's default OBJ export is quads). `LINE_BUFFER_LENGTH` bumped from 300 → 4096 so long n-gon face lines don't truncate mid-token.
- OBJ texture V is flipped on read (`uv.v = 1 - uv.v`) to match the sampler's top-origin convention.
- `usemtl` always records `selectedMatName` (previously only when the .mtl lookup failed), so scene.json `_materials` overrides apply whether or not the .mtl carried the material.
- `SceneJsonLoader::readObjAsSceneObjects` wires .obj meshes into the scene graph. Previously the `.obj` branch in `loadMeshFromFile` read the file but discarded the result.

## Architecture

CPU path tracer with a thin CLI → `libraygen.a` → shared C++ utility libs split.

### Core flow

1. **`RendererSceneLoader`** (`sceneloader.{h,cpp}`) reads JSON (JSONC: `//` comments allowed) into a `Scene`. Meshes come from primitives (`{ "type": "plane" | "cube" | "sphere" }`), `.obj` files, `.mesh` (RayGen binary), or `.toba` archives via the `_bundle` key.
2. **`Scene`** (`scene.{h,cpp}`) is a tree of `SceneObject`s with transforms, meshes, materials. `Camera` is a subclass; `scene.mainCamera` is the render viewpoint. Scene also owns the optional envmap (equirect or cubemap) and its luminance CDF for importance sampling.
3. **`RayRenderer`** (`rayrenderer.{h,cpp}`) is the engine. `init()` flattens the scene into world-space triangles and builds a **flat binned-SAH BVH** (`bvh.{h,cpp}`; replaces the old KDTree). `render()` spawns `settings.threads` workers that each call `renderPixel → traceEyeRay → tracePath`.
4. **Shader provider** (pluggable via `-d`): `RaySimpleShaderProvider` (0), `RayAmbientOcclusionShaderProvider` (1), `LambertShaderProvider` (2, `lambert.cpp`), `LambertWithAOShaderProvider` (3), **`RayBSDFShaderProvider` (5, default — `bsdf.cpp` + `rayrenderer.cpp`)**. The BSDF path combines `DiffuseShader`, `GlossyShader`, `RefractionShader`, `EmissionShader`, `TransparencyShader` via `MixShader`.
5. `renderPixel` applies **Reinhard tone map per channel** + **≈sRGB (1/2.2) gamma encode** before writing to `Image4f`. `saveImage` (ugm) writes the output.

### BSDF pipeline (shader = 5)

Material fields drive the lobe mix in `MixShader`: `diffuse = 1 - glossy - refraction`. Each non-trivial weight runs its lobe, multiplies by the weight, and accumulates.

- `DiffuseShader` — cos-weighted hemisphere BSDF sample (Halton dims 2–3), plus **direct-light NEE** via `traceLight` (area + point lights) and `traceEnvmapLight` (envmap importance sample + shadow ray). Uses `BSDFParam::misDiffuse` / `misEnvBsdfPdf` so tracePath's emission / envmap hits can MIS-weight against the direct strategies (power heuristic β=2). Texture sampled here and folded into albedo.
- `GlossyShader` — **GGX microfacet**. Heitz 2018 visible-normals sampling (VNDF) of the half vector, reflect the view across it. Schlick Fresnel with `F0 = lerp(0.04, albedo, metallic)`. Anisotropy split into `αx, αy = roughness² · (1 ∓ anisotropy)` so `> 0` elongates the highlight along the tangent; `anisoRotation` (degrees) spins the tangent frame around the shading normal. Roughness < 1e-3 falls back to a delta-mirror path. The texture sample is folded into albedo (and therefore F0 for metals).
- `RefractionShader` — stochastic Fresnel pick (Schlick) between reflect and refract; `refractionRatio` is the IOR. Uses `fabs(cos θ)` so internal hits use the correct incidence angle instead of collapsing to 1.0 (that was the glass-is-black bug). **Chromatic dispersion** (`chromaDispersion`) picks one of R/G/B at the first CA hit, commits the channel into `BSDFParam::chromaChannel` so all later refractive bounces on the same path reuse the same IOR, and masks the return to that channel × 3 for an unbiased MC estimate.
- `EmissionShader` — MIS-partner for `traceAreaLight`; applies the BSDF-strategy weight if the caller set `misDiffuse`.
- `TransparencyShader` — continues the ray straight through, attenuated by `m.transparency`.

Russian Roulette kicks in after `MIN_RR_DEPTH` (3) and uses max-channel throughput as the survival probability, clamped to `[RR_MIN_PROB=0.05, RR_MAX_PROB=0.95]`. `MAX_TRACE_DEPTH` is **32** (raised from 16 so concave glass like Suzanne doesn't clamp to black on internal bounce chains).

### Sampling

- **Halton LDS** (`raycommon.cpp`): per-pixel, per-sample walk of primes 2, 3, 5, …, 53. Cranley-Patterson rotation per (pixel, dim) decorrelates pixels while preserving within-pixel stratification. **Dimension budget** inside a pixel-sample: 0–1 sub-pixel AA jitter, 2–3 aperture, 4+ BSDF / light / env NEE. Past `LDS_MAX_DIM=16` it falls back to the PRNG — fine for deep path tails.
- **Anti-aliasing** is stochastic box-filter jitter within the pixel. Every primary ray adds `(jitter-0.5) · viewScaleX` (and the Y equivalent) to the pixel-centre direction. `enableAntialias=false` zeroes those offsets so edges alias but the LDS dims still advance consistently (so DOF / BSDF stratification is preserved).
- **Depth-of-field** (`Camera::depthOfField`, `Camera::aperture`) is a thin-lens model — jittered focal point × aperture-point on the lens disc → normalized ray. `Camera::apertureBlades` (0 = round, ≥3 = regular n-gon) shapes the bokeh; sampling is wedge-then-triangle fold (uniform in polygon). `Camera::apertureRotation` (degrees) rotates the polygon. `Camera::aperture` behaves like an f-stop: ctx.aperture = 1/aperture, so *smaller* JSON number = wider bokeh.
- **`Camera::fieldOfView`** is vertical FoV in degrees. `viewScaleX == viewScaleY` for square pixels — the horizontal FoV widens naturally because a row just has more pixels. **Do not multiply `viewScaleX` by the aspect ratio**; that was a bug that stretched every image vertically by W/H (fixed in `initRenderThreadContext`).

### Lighting

- **Area lights** are emissive triangle meshes (`mat.emission > 0`). `traceAreaLight` samples a triangle then a point inside it; the shadow ray predicate skips emissive surfaces so a multi-triangle area light doesn't self-shadow.
- **Point lights** are emissive meshes used as a location; see `tracePointLight`.
- **Image-Based Lighting** — the scene declares an envmap block:
  ```json
  "envmap": {
      "texture": "../../textures/sky_sun.hdr",   // equirect (HDR .hdr or LDR jpg/png)
      // OR
      "cubemap": "/path/to/dir",                 // 6 faces: px/nx/py/ny/pz/nz
      "ext": "jpg",                              // file extension for cubemap faces
      "intensity": 1.5,
      "rotation": 30                             // Y-axis spin, degrees
  }
  ```
  - `Texture::loadFromFile` routes to a minimal Radiance .hdr (RGBE) parser when the extension is `.hdr`. Non-HDR textures are sRGB (decoded to linear on sample); HDR stays linear. The `Texture::sRGB` flag drives this.
  - `sampleEnvironment(dir)` picks cubemap if all six faces are attached, else equirect. Cubemap uses the standard OpenGL convention (dominant axis picks face, other axes form UV).
  - **NEE + MIS** against DiffuseShader's BSDF sample: `Scene::buildEnvmapCDF` builds a luminance CDF (`sin θ` weight for equirect, per-texel `1/(s²+t²+1)^{3/2}` solid-angle weight for cubemap). `sampleEnvmapDirection` inverts it; `envmapDirectionPdf` is the query side.

### Envmap rotation convention

All paths apply the same `Y`-axis rotation: the *sample direction* is rotated by `+yaw` before texture lookup. When inverting (NEE sampling, going back from a texel to a world direction), rotate by `-yaw`. Keep this consistent when touching envmap code.

### BVH

`bvh.{h,cpp}`. Flat `std::vector<BVHNode>` (32 B/node: bmin, bmax, firstOrLeft, count, axis). Binned SAH build, iterative traversal. Two APIs:
- `intersectClosest(ray, info)` — closest hit with `info.t` pruning.
- `intersectAny<Pred>(ray, maxT, pred)` — templated any-hit for shadow rays; predicate decides what counts as an occluder. Shaders use `mat.emission > 0` to skip self-shadow from lights, `mat.transparency < 0.01 && mat.refraction > 0.1` style filters for glass.

`findNearestTriangle` wraps `intersectClosest`. Shadow rays call `bvh.intersectAny` directly inside RayRenderer (not through a shader helper).

### TOBA bundle flow

`.toba` is a chunk archive (ucm `Archive`) holding meshes, images, and a JSON manifest. Loading is triggered by a `"_bundle": "<path.toba>"` key inside a SceneObject:

1. `sceneloader.cpp:readSceneObject` opens the archive, reads chunk `0x1` (FORMAT_TAG_MIFT) as JSON.
2. The manifest's `_materials` block populates the current loading-stack scope.
3. Each non-`_materials`/`_models` property becomes a child SceneObject whose `mesh` / `mat` fields reference `sob://__this__/<uid>` or `tob://...`.
4. **After** the bundle's materials are processed, `readSceneObject` reads the scene's own `_materials` block — this intentional order lets a scene JSON override a bundle material by name (e.g. retune `metal` to `metallic: 1, anisotropy: 0.6`).

Editing a `.toba` from outside this repo uses the sibling tool:
```shell
# Extract to a folder:
/Users/.../tarumae-renderer/src/toba/_mac/toba <file.toba> -x <out_dir>
# Edit <out_dir>/00000001.mift (the manifest) to adjust materials
# Inject the updated manifest back in place:
/Users/.../tarumae-renderer/src/toba/_mac/toba <file.toba> -i 1 < <out_dir>/00000001.mift
```

### Post-processing

`renderPixel` returns tone-mapped, gamma-encoded color4f in [0,1]. Any extra post-process lives in `renderAsyncThread` / scene-save-time code paths; the default flag is off (`enableRenderingPostProcess`).

## Scene JSON reference (quick)

```jsonc
{
    "mainCamera": {
        "location": [0, 1.5, 5],
        "angle": [-8, 0, 0],
        "fieldOfView": 55,          // vertical FoV, degrees
        "depthOfField": 4.5,        // focal distance (camera units)
        "aperture": 2.0,            // f-stop-like; smaller = wider blur
        "apertureBlades": 0,        // 0 = round, ≥3 = polygon
        "apertureRotation": 0,      // degrees
        "focusOn": "someObject",    // overrides depthOfField via object centre
        "exposure": 1.0
    },

    "envmap": {
        // One of "texture" (equirect) OR "cubemap" (dir with px/nx/py/ny/pz/nz faces)
        "cubemap": "../../textures/cubemap/city-night",
        "ext": "jpg",
        "intensity": 2.5,
        "rotation": 40
    },

    "_materials": {
        "myMetal": {
            "color": [0.86, 0.86, 0.87],
            "glossy": 1.0,
            "metallic": 1.0,        // F0 = albedo when 1, dielectric 0.04 when 0
            "roughness": 0.25,
            "anisotropy": 0.6,      // -1..1, 0 = isotropic
            "anisoRotation": 0      // degrees around shading normal
        }
    },

    "world": {
        "chair": {
            "_bundle": "/path/to/chair_adv_01.toba"
            // bundle-defined materials can be overridden by a sibling
            // "_materials" block inside the same object.
        }
    }
}
```

Material fields — core: `color` ([r,g,b] or `"#rrggbb"`), `texture`/`tex` (path), `texTiling`. Lobes: `diffuse`, `glossy`, `refraction`, `transparency`. GGX extras: `metallic`, `roughness`, `anisotropy`, `anisoRotation`. Refraction extras: `refractionRatio`, `chromaDispersion`. Light: `emission`, `spotRange`.

## Dependency modules (external, under `inc/`)

- **ucm** (`cpp-common-class`) — `ucm::string`, `File`, `Stopwatch`, JSON parser/writer, `Archive`, ANSI terminal helpers. Used everywhere; `ucm::string` is the string type, not `std::string`.
- **ugm** (`cpp-graphics-math`) — vector/matrix math (`vec3`, `color3`, `color4`, `BoundingBox`), `Image`/`Image3f`/`Image4f`, image codecs, legacy `SpaceTree`/`KDTree`. The renderer's geometry and image primitives all come from here.

When adding functionality, prefer these libraries' types over STL/third-party equivalents — the codebase is consistent about it.

## Known issues / workarounds

- Stack-size / optimisation sensitivity: rendering paths are deep enough that stale incremental builds can trigger weird crashes at unpredictable depths. `make clean && make` if things go sideways after a header edit.

## Conventions

- C++14, clang++. Release: `-O3 -ffast-math -flto -mcpu=apple-m1`. Apple Silicon forces `-arch arm64`.
- All engine code is in `namespace raygen`; math/graphics types come from `namespace ugm`; utilities from `namespace ucm`.
- Header guards use `__snake_name_h__` style.
- Scene JSON: object-keyed maps (the key is the object name), `//` comments allowed, colors accept `[r,g,b]` floats or `"#rrggbb"` hex strings.
- User-visible *scene JSON overrides* (camera, material) should "just work" — scene-level values win over bundle defaults. Don't break that order without a reason.
- Tone-map is in the renderer (Reinhard × ≈1/2.2 gamma); textures are sRGB-decoded on sample (except `isHDR == true`). Don't double-encode gamma anywhere else.
