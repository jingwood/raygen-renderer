# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Platform-specific folders under `build/` use plain GNU Make:

```shell
cd build/mac-m      # or build/mac-intel, build/linux
make                # produces libraygen.a and the `raygen` executable
make clean
```

Xcode: `projects/raygen.xcodeproj`. Windows: `projects/raygen-win32/raygen.sln`.

Makefiles include sibling repos `cpp-common-class` and `cpp-graphics-math` via `-I../../../`. They're submodules — run `git submodule update --init` first. Prebuilt static archives live under `lib/<platform>/`; Linux links system `-ljpeg -lpng -lz`.

Release flags: `-O3 -ffast-math -flto -mcpu=apple-m1` on Apple Silicon. If incremental builds misbehave, `make clean && make`.

## Run

```shell
./raygen render <scene.json> [-options]
```

Only the `render` command is implemented (`src/main.cpp`). Sample scenes in `resources/scenes/` (`cubeRoom`, `sphereArray`, `suzanne`, `bokehTest`, `anisoTest`, `iblTest`, `tobaTest`). Output defaults to `<scene-basename>.jpg`.

Key CLI flags: `-r WxH`, `-s <samples>`, `-c <threads>`, `-d <0|1|2|3|5>` (shader — 5 = BSDF, default), `-enaa`, `-encs`, `-enpp`, `--focus-obj <name>`, `--dump`.

No automated tests — validation is visual.

## Viewer (raygen-viewer)

Interactive tuner (Dear ImGui + GLFW + OpenGL 3.2 core). Loads a scene.json, runs raygen on a worker thread, uploads results to a GL texture, and exposes tuning parameters as live sliders.

### Build

Windows: `projects/raygen-viewer-win32/raygen-viewer.sln` (VS 2026). The solution splits the renderer into `raygen-core` (StaticLibrary) so both `raygen.exe` and `raygen-viewer.exe` link it — same layout as the `libraygen.a` on mac/linux.

GLFW defaults to `D:\Libs\glfw\glfw-3.4.bin.WIN64\`; `lib/windows/` carries the rest (libucm, libugm, jpeg, libpng, zlib, glfw3). `zlib.dll` must sit next to the exe.

Dear ImGui is a submodule at `inc/imgui` — `git submodule update --init --recursive`.

macOS (Apple Silicon): `cd build/mac-m && make viewer` produces `raygen-viewer` next to `libraygen.a`. GLFW comes from Homebrew (`brew install glfw`); override the install prefix with `make viewer GLFW_PREFIX=/path/to/glfw` if it isn't at `/opt/homebrew/opt/glfw`. Links against the system `Cocoa`, `OpenGL`, `IOKit`, `CoreVideo`, `CoreFoundation` frameworks; builds with `-DGL_SILENCE_DEPRECATION` since macOS 10.14 deprecated the GL framework (still shipping, still works).

Linux viewer build isn't wired up yet; sources in `src/viewer/` are platform-neutral.

### Run

```shell
raygen-viewer[.exe] path/to/scene.json
```

Scene argument is required; the viewer exits with a usage message otherwise. HiDPI follows the monitor scale; override with `RAYGEN_UI_SCALE`.

### Panels

- **raygen viewer** — status + progress bar + cancel. Four collapsible sections: Quality (samples, threads, denoise), Camera (location / angle / FoV / DoF / aperture / blades / apertureRotation / exposure, targets `scene.mainCamera`), Scene (envmap intensity/rotation), Post-process (bloom threshold/strength/curve/radius). When the scene pins `focusOn`, a "focus lock: <name>" badge appears next to the DoF slider with a `clear` button — the renderer recomputes DoF from the focus object's bbox every frame, so the slider won't stick until cleared.
- **Output** — width/height + Apply + resolution presets (480p–4K). Disabled while rendering.
- **File** — Reload scene, Save render (format from extension).
- **render** — GL texture of the latest frame; mouse-wheel zooms, `1:1` / `fit` reset.

### Architecture

A single worker thread owns `RayRenderer::render()`; the main thread owns the `Scene` (`std::unique_ptr` for reload), renderer, GL, and ImGui. Coordination is one mutex + condvar around a `RenderJob`.

**Job kinds:**
- `Full` — trace + denoise + bloom. Installs a throttled `progressCallback` that packs `renderingImage` mid-render for the progressive preview.
- `PostOnly` — re-runs bloom from the cached `preBloomHdrImage`, tonemaps, done in a few ms.

`onlyPostProcessChanged(...)` decides Full vs PostOnly. Any non-bloom delta forces Full. Dirty handling is one kick site per frame — don't re-split it.

Settings are written into `renderer.settings` directly; the renderer copies-by-value at construction, so a separate struct won't propagate.

Cancellation is cooperative: `RayRenderer::cancelRequested` is checked at row boundaries. Cancelled renders leave `hasPreBloomImage` untouched so PostOnly still works against the last complete frame.

Scene reload swaps the `unique_ptr` while the worker is idle (button disabled otherwise).

### Per-scene tuning (`<scene>.viewer.json`)

Every render kick auto-writes the committed slider state to a sidecar next to the scene JSON: `cubeRoom.json` → `cubeRoom.viewer.json`. On load (startup *or* Reload scene) the sidecar overlays values onto the scene-seeded defaults, so user tweaks win while missing keys fall back to the scene. Sidecars are `.gitignore`-d (`*.viewer.json`) — they're per-user state, not scene authoring.

Fields mirror the sliders plus output resolution: `samples`, `threads`, `denoise`, `denoiseIntensity`, `location`, `angle`, `fieldOfView`, `depthOfField`, `aperture`, `apertureBlades`, `apertureRotation`, `exposure`, `envIntensity`, `envRotation`, `postProcess`, `bloom{Threshold,Strength,Curve,Radius}`, `outputWidth`, `outputHeight`. Camera keys use the same JSON shape as `scene.mainCamera` (arrays for `location`/`angle`) so the sidecar is hand-editable and diffable. Unknown/missing keys are ignored; unknown fields written by a future viewer version get dropped on rewrite.

Why sidecar over writing back into scene.json: JSONC comments + authored structure would be lost on every round-trip, and scene authoring state (objects, lights, materials) has a different ownership than "what I last dialed the UI to".

## Architecture

CPU path tracer: thin CLI → `libraygen.a` → shared C++ utility libs.

### Core flow

1. **`RendererSceneLoader`** (`sceneloader.{h,cpp}`) reads JSONC (`//` comments) into a `Scene`. Meshes: primitives, `.obj`, `.mesh`, or `.toba` bundles.
2. **`Scene`** (`scene.{h,cpp}`) — tree of `SceneObject`s with transforms/meshes/materials. `Camera` is a subclass; `scene.mainCamera` renders. Scene owns the optional envmap + its luminance CDF.
3. **`RayRenderer`** (`rayrenderer.{h,cpp}`) — `init()` flattens to world-space triangles and builds a flat binned-SAH BVH (`bvh.{h,cpp}`). `render()` spawns `settings.threads` workers: `renderPixel → traceEyeRay → tracePath`.
4. **Shader provider** (pluggable via `-d`): `RaySimpleShaderProvider` (0), `RayAmbientOcclusionShaderProvider` (1), `LambertShaderProvider` (2), `LambertWithAOShaderProvider` (3), **`RayBSDFShaderProvider` (5, default)**. The BSDF path mixes `DiffuseShader`, `GlossyShader`, `RefractionShader`, `EmissionShader`, `TransparencyShader` via `MixShader`.
5. `renderPixel` writes both tonemapped preview (`renderingImage`, Reinhard + ≈1/2.2 gamma) and linear HDR (`hdrImage`) at the same pixel.

### BSDF pipeline (shader = 5)

Lobe mix: `diffuse = 1 - glossy - refraction`. Each nonzero lobe runs, weights, accumulates.

- `DiffuseShader` — cos-weighted hemisphere sample + direct-light NEE (area/point lights, envmap importance sample). MIS against tracePath's emission/envmap hits via `BSDFParam::misDiffuse` / `misEnvBsdfPdf` (power heuristic β=2).
- `GlossyShader` — GGX microfacet with Heitz 2018 VNDF sampling. Schlick Fresnel, `F0 = lerp(0.04, albedo, metallic)`. Anisotropy splits `αx, αy = roughness² · (1 ∓ anisotropy)`; `anisoRotation` spins the tangent frame. Roughness < 1e-3 falls back to a delta-mirror path.
- `RefractionShader` — stochastic Fresnel pick (Schlick) between reflect/refract; `refractionRatio` is IOR. `chromaDispersion` commits one of R/G/B per path (via `BSDFParam::chromaChannel`) for unbiased chromatic aberration.
- `EmissionShader` — MIS partner for area-light sampling.
- `TransparencyShader` — straight-through, attenuated by `m.transparency`.

Russian roulette after `MIN_RR_DEPTH=3` using max-channel throughput, clamped `[0.05, 0.95]`. `MAX_TRACE_DEPTH=32`.

### Sampling

- **Halton LDS** (`raycommon.cpp`) with Cranley-Patterson rotation per (pixel, dim). Dim budget: 0–1 AA jitter, 2–3 aperture, 4+ BSDF/NEE. Past `LDS_MAX_DIM=16` falls back to PRNG.
- **Anti-aliasing** — stochastic box-filter jitter on the primary ray direction. `enableAntialias=false` zeroes the jitter but still advances LDS dims so DOF/BSDF stratification is preserved.
- **Depth-of-field** — thin-lens. `Camera::apertureBlades` (0 = round, ≥3 = polygon) shapes bokeh via wedge-then-triangle-fold sampling. `Camera::aperture` behaves like an f-stop (smaller JSON = wider blur).
- **`Camera::fieldOfView`** is vertical FoV in degrees. `viewScaleX == viewScaleY` for square pixels — horizontal FoV follows from row pixel count.

### Lighting

- **Area lights** — emissive triangle meshes (`mat.emission > 0`). Shadow ray predicate skips emissive surfaces so multi-triangle lights don't self-shadow.
- **Point lights** — emissive meshes used as a location.
- **Image-Based Lighting** — envmap block (equirect `texture` or `cubemap` dir with 6 faces):
  ```json
  "envmap": {
      "texture": "../../textures/sky_sun.hdr",
      "cubemap": "/path/to/dir",   // OR: 6 faces px/nx/py/ny/pz/nz
      "ext": "jpg",
      "intensity": 1.5,
      "rotation": 30
  }
  ```
  `.hdr` goes through an RGBE parser and stays linear; LDR textures are sRGB (decoded on sample). NEE + MIS via `Scene::buildEnvmapCDF` (luminance × solid-angle weight).

**Envmap rotation convention:** rotate the *sample direction* by `+yaw` before texture lookup; when inverting (CDF → world direction) rotate by `-yaw`. Keep consistent.

### BVH

`bvh.{h,cpp}`. Flat `std::vector<BVHNode>` (32 B/node). Binned SAH build, iterative traversal.
- `intersectClosest(ray, info)` — closest hit with `info.t` pruning.
- `intersectAny<Pred>(ray, maxT, pred)` — templated any-hit for shadow rays; predicate decides what occludes.

`findNearestTriangle` wraps `intersectClosest`; shadow rays call `bvh.intersectAny` directly.

### TOBA bundle flow

`.toba` is a chunk archive (ucm `Archive`) holding meshes, images, and a JSON manifest. Triggered by `"_bundle": "<path.toba>"` in a SceneObject:

1. `sceneloader.cpp:readSceneObject` opens the archive and parses chunk `0x1` (manifest JSON).
2. The manifest's `_materials` populate the current loading-stack scope.
3. Non-`_materials`/`_models` entries become child SceneObjects referencing `sob://__this__/<uid>` or `tob://...`.
4. The scene's own `_materials` block is read *after* the bundle's, so scene JSON overrides bundle defaults by name.

### Post-processing

Bloom is energy-based and runs in linear HDR radiance (not post-tonemap LDR). Render threads write both `renderingImage` (tonemapped) and `hdrImage` (linear) per pixel. After trace + optional denoise, `applyPostProcess(hdrImage)` extracts excess over `bloomThreshold`:

    L = luma(pixel);
    glow.rgb = pixel.rgb * (L - threshold) / L   if L > threshold else 0;

Downsample → Gaussian blur → upsample → add `glow * bloomStrength` back **unclamped**. `applyTonemapGamma` then produces `renderingImage`. The final add can't use `img::calc` (clamps to [0,1]) — it writes floats directly.

Defaults: `bloomThreshold = 1.0`, `bloomStrength = 1.0`, `bloomCurve = 1.0` (>1 sharpens the knee). Top-level `enableRenderingPostProcess` is off.

## Scene JSON reference

```jsonc
{
    "mainCamera": {
        "location": [0, 1.5, 5],
        "angle": [-8, 0, 0],
        "fieldOfView": 55,          // vertical FoV, degrees
        "depthOfField": 4.5,
        "aperture": 2.0,            // f-stop-like; smaller = wider blur
        "apertureBlades": 0,        // 0 = round, ≥3 = polygon
        "apertureRotation": 0,
        "focusOn": "someObject",    // overrides depthOfField
        "exposure": 1.0
    },

    "envmap": {
        "cubemap": "../../textures/cubemap/city-night",
        "ext": "jpg",
        "intensity": 2.5,
        "rotation": 40
    },

    "_materials": {
        "myMetal": {
            "color": [0.86, 0.86, 0.87],
            "glossy": 1.0,
            "metallic": 1.0,
            "roughness": 0.25,
            "anisotropy": 0.6,      // -1..1, 0 = isotropic
            "anisoRotation": 0
        }
    },

    "world": {
        "chair": {
            "_bundle": "/path/to/chair_adv_01.toba"
            // sibling "_materials" block can override bundle materials
        }
    }
}
```

Material fields — core: `color`, `texture`/`tex`, `texTiling`. Lobes: `diffuse`, `glossy`, `refraction`, `transparency`. GGX: `metallic`, `roughness`, `anisotropy`, `anisoRotation`. Refraction: `refractionRatio`, `chromaDispersion`. Light: `emission`, `spotRange`.

## Dependency modules (under `inc/`)

- **ucm** (`cpp-common-class`) — `ucm::string`, `File`, `Stopwatch`, JSON parser/writer, `Archive`. `ucm::string` is the string type, not `std::string`.
- **ugm** (`cpp-graphics-math`) — `vec3`, `color3/4`, `BoundingBox`, `Image`/`Image3f`/`Image4f`, image codecs.

Prefer these types over STL/third-party equivalents.

## Conventions

- C++14, clang++. Apple Silicon forces `-arch arm64`.
- Engine code in `namespace raygen`; math/graphics in `namespace ugm`; utilities in `namespace ucm`.
- Header guards use `__snake_name_h__` style.
- Scene JSON: object-keyed maps (key = object name), `//` comments allowed, colors accept `[r,g,b]` floats or `"#rrggbb"` hex.
- Scene-level JSON overrides win over bundle defaults. Don't break that order.
- Tone-map is in the renderer (Reinhard × ≈1/2.2 gamma); textures are sRGB-decoded on sample (except `isHDR`). Don't double-encode gamma.
