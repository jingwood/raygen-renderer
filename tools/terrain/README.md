# Terrain mesh generator

Stand-alone Python tool that bakes a heightmap into a Wavefront `.obj`
the raygen scene loader can read. Pure stdlib — no numpy / Pillow / etc.

## Why a generator (vs. authoring a `.toba` in Blender)

Procedural terrain wants to be a *function* of seed + parameters, not a
hand-sculpted asset. This script gives you reproducible heights, a
heightmap-image fallback for hand-authored topography, and direct control
over the grid resolution / world-space size — without round-tripping
through DCC tools every iteration.

The result is a static mesh: raygen flattens it into BVH-resident
triangles at scene load and never touches the per-vertex math again. So
you can crank the resolution up to a few hundred thousand triangles
without per-frame cost.

## Quickstart

```sh
# 20 × 20 mesh-local units, 192×192 grid, 1.2 amp. Scale up in the JSON.
python3 tools/terrain/terrain_gen.py --output /tmp/island.obj
```

Then in scene JSON — match Blender's convention of authoring small meshes
and scaling them in the parent SceneObject:

```jsonc
"Terrain": {
    "location": [0, 0, 0],
    "scale":    [3, 3, 3],
    "mesh": "/tmp/island.obj",
    "mat": { "color": "#5e6447", "diffuse": 0.85, "roughness": 0.6 }
}
```

> **Why small mesh + JSON scale, not large mesh?** raygen has a global
> `MAX_RAY_DISTANCE = 999.9` BVH-traversal cap (in `raycommon.h`), and
> grazing rays on very large flat meshes lose floating-point precision in
> the slab/triangle math — terrains hundreds of units across don't render
> reliably. Authoring in the 10-unit range and scaling up in the scene
> matches what the Blender exporter does.

## Useful flags

| flag | what it does |
|---|---|
| `--size X Z` | terrain extent in mesh-local units (default 20×20) |
| `--res NX NZ` | grid resolution (default 192×192 → ~73k tris) |
| `--amplitude A` | max elevation in mesh-local units (default 1.2) |
| `--frequency F` | base noise frequency, cycles/unit (default 0.12) |
| `--octaves N` | fBm octaves (default 6) |
| `--ridges` | swap fBm for ridged-multifractal (sharp peaks) |
| `--seed N` | re-roll the noise lattice |
| `--seabed Y` | clamp heights below `Y` to a flat floor |
| `--falloff F` | taper height toward edges (0..1, fraction of side) |
| `--bias B` | constant Y offset added to every vertex |
| `--heightmap path.pgm` | use a PGM grayscale image as the heightmap |

## Conventions

- World axes: +Y up. Terrain spans X ∈ [-size_x/2, +size_x/2],
  Z ∈ [-size_z/2, +size_z/2]; centre is at origin.
- UVs: (0,0) at the −X,−Z corner, (1,1) at +X,+Z. Single-tile texture
  coverage; multiply with `texTiling` in the material to repeat.
- Normals are generated from the height field via central differences
  (cheap, smooth, and consistent across mesh resolutions).
- Triangulation alternates the diagonal direction quad-by-quad so a
  non-axis-aligned ridge isn't artificially sharper along one diagonal.

## Heightmap input

`--heightmap` reads PGM (P5 binary or P2 ASCII), grayscale only. The
sample value is normalised to [0,1] and multiplied by `--amplitude`.
Generate PGMs from any image editor: GIMP / ImageMagick / Pillow.

```sh
# ImageMagick: convert any image to a 16-bit PGM.
magick heightmap.png -colorspace Gray -depth 16 heightmap.pgm
```

## Ocean / coastline scenes

For a coastline where the terrain meets a water plane at Y=0, common
recipe is:

```sh
python3 tools/terrain/terrain_gen.py \
    --size 20 20 --res 256 256 \
    --amplitude 2.5 --ridges \
    --seabed -0.4 --falloff 0.15 \
    --output /tmp/coast.obj
```

`--seabed -0.4` flat-floors the underwater portion (so a transparent
water plane isn't cluttered by needless underwater detail), and
`--falloff 0.15` tapers the outer 15% so the rectangle blends into a
surrounding ocean plane instead of ending in a vertical cliff. Scale
up in the scene's `"Terrain"` SceneObject (e.g. `"scale": [10, 10,
10]`) for a 200×200-unit island world-space footprint.
