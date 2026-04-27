# blend → raygen scene converter

Convert a `.blend` file into a baseline raygen scene JSON plus a companion
`.obj` / `.mtl` pair.

## Usage

```bash
blender -b path/to/scene.blend -P blend_to_raygen.py -- \
    --out path/to/raygen_export/scene.json
```

Anything before `--` is consumed by Blender; everything after is forwarded to
the script. Output basename drives the `.obj`/`.mtl` filenames written next
to the JSON.

### Example (F2)

```bash
blender -b "F:/3D Models/F2/F2.blend" -P blend_to_raygen.py -- \
    --out "F:/3D Models/F2/raygen_export/F2.json"
```

## What gets exported

- **Geometry** — every visible `MESH` object is bundled into a single OBJ
  (`<basename>.obj`) with `forward = -Z, up = Y` axis conversion. Transforms
  are baked into the vertex stream, so the JSON `world.<basename>` entry sits
  at the origin with identity rotation/scale.
- **Camera** — `scene.camera` becomes `mainCamera` (location, Euler angle in
  degrees, vertical FoV, exposure, optional DoF).
- **World envmap** — Background → Environment Texture (with optional Mapping
  rotation) becomes the `envmap` block. Texture path is written relative to
  the output directory when sensible.
- **Materials** — Principled BSDF nodes are mapped to raygen material fields:
  - Base Color → `color` (and `tex` when a TEX_IMAGE feeds the socket)
  - Metallic → `metallic`
  - Roughness → `roughness`
  - Specular IOR Level → `glossy` (Blender 4.x naming; falls back to
    pre-4.x `Specular`)
  - Transmission → `refraction`, with IOR → `refractionRatio`
  - Alpha < 1 → `transparency`
  - Emission Color × Strength → `color` + `emission`
- **Lights** — `POINT` → tiny emissive sphere; `AREA` → emissive plane sized
  to the Blender shape; `SPOT` → emissive sphere with `spotRange`; `SUN` is
  skipped (use the envmap rotation/intensity instead).

## Caveats

- Light `energy` is in Watts and copies straight through to `emission`. It
  rarely matches the unit-less raygen scale 1:1 — expect to tune.
- Material → JSON mapping is a baseline. Glossy/roughness values often need
  manual nudges (see [F2.json](../../../../3D Models/F2/raygen_export/F2.json)
  for a reference of what hand-tuned output looks like).
- Volumetric / participating-media settings (`engineFlame`, vapor cones,
  etc.) are **not** generated. Add those by hand after the converter runs.
- No reverse merge — re-running the script overwrites the JSON. Keep
  hand-edits in a separate file or commit each version.
