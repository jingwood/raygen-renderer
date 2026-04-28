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
# 200 m × 200 m island, 256×256 grid, 8 m amplitude.
python3 tools/terrain/terrain_gen.py --output /tmp/island.obj
```

Then in scene JSON:

```jsonc
"Terrain": {
    "location": [0, 0, 0],
    "mesh": "/tmp/island.obj",
    "mat": { "color": "#5e6447", "diffuse": 0.85, "roughness": 0.6 }
}
```

## Useful flags

| flag | what it does |
|---|---|
| `--size X Z` | terrain extent in metres (default 200×200) |
| `--res NX NZ` | grid resolution (default 256×256 → ~130k tris) |
| `--amplitude A` | max elevation in metres (default 8) |
| `--frequency F` | base noise frequency, cycles/m (default 0.012) |
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
    --size 400 400 --res 512 512 \
    --amplitude 18 --ridges \
    --seabed -3 --falloff 0.15 \
    --output /tmp/coast.obj
```

`--seabed -3` flat-floors the underwater portion (so a transparent water
plane isn't cluttered by needless underwater detail), and `--falloff
0.15` tapers the outer 15% so the rectangle blends into a surrounding
ocean plane instead of ending in a vertical cliff.
