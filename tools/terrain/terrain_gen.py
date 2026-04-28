#!/usr/bin/env python3
"""Generate a terrain heightmap mesh as a Wavefront .obj.

Designed as a stand-alone scene-prep tool for raygen. Pure Python (stdlib
only) so the system interpreter Just Works — no numpy, no Pillow. Output
matches what the .obj loader at src/raygen/objreader.cpp expects:
v / vt / vn / f triplets in 1-indexed Wavefront form.

Usage examples:

    # 200m × 200m island, 256×256 grid, 8m amplitude, default seed:
    python3 terrain_gen.py --output island.obj

    # Bigger ocean basin with submerged seabed, ridged peaks above:
    python3 terrain_gen.py --size 400 400 --res 512 512 --amplitude 18 \\
        --ridges --seabed -2 --output coast.obj

    # Heightmap PGM (grayscale) instead of procedural:
    python3 terrain_gen.py --heightmap height.pgm --amplitude 12 --output map.obj

The terrain spans X ∈ [-size_x/2, +size_x/2], Z ∈ [-size_z/2, +size_z/2],
with Y as elevation (raygen's +Y up convention). UVs map (0,0) at the
−X,−Z corner to (1,1) at the +X,+Z corner so a single texture tiles
across the whole sheet.
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from typing import List, Tuple


# --- Noise basis ----------------------------------------------------------

def _hash3(x: int, y: int, z: int) -> float:
    """Wang-style 3-int hash → uniform [0,1). Mirrors raygen/medium.cpp."""
    h = ((x & 0xFFFFFFFF) * 374761393
         + (y & 0xFFFFFFFF) * 668265263
         + (z & 0xFFFFFFFF) * 2147483647) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    h ^= h >> 16
    return (h & 0x00FFFFFF) / float(0x01000000)


def _smoothstep(x: float) -> float:
    return x * x * (3.0 - 2.0 * x)


def value_noise_2d(x: float, y: float, seed: int) -> float:
    """Trilinear-smoothstep value noise on the XY plane. Returns [0,1].

    Seed shifts the integer lattice by a deterministic offset so two runs
    with different seeds produce different terrains without re-keying the
    underlying hash."""
    sx = seed * 0x9E3779B1
    sy = seed * 0x85EBCA77
    x += (sx & 0xFFFF) / 65536.0
    y += (sy & 0xFFFF) / 65536.0
    xi, yi = math.floor(x), math.floor(y)
    xf, yf = _smoothstep(x - xi), _smoothstep(y - yi)
    xi, yi = int(xi), int(yi)
    c00 = _hash3(xi,     yi,     0)
    c10 = _hash3(xi + 1, yi,     0)
    c01 = _hash3(xi,     yi + 1, 0)
    c11 = _hash3(xi + 1, yi + 1, 0)
    a = c00 * (1.0 - xf) + c10 * xf
    b = c01 * (1.0 - xf) + c11 * xf
    return a * (1.0 - yf) + b * yf


def fbm(x: float, y: float, *, freq: float, octaves: int, gain: float,
        lacunarity: float, seed: int) -> float:
    """Fractal Brownian Motion in [0,1]. Standard amplitude-normalised sum."""
    total = 0.0
    amp_sum = 0.0
    amp = 1.0
    fx, fy = x * freq, y * freq
    for _ in range(octaves):
        total += amp * value_noise_2d(fx, fy, seed)
        amp_sum += amp
        amp *= gain
        fx *= lacunarity
        fy *= lacunarity
    return total / amp_sum if amp_sum > 0.0 else 0.0


def ridged(x: float, y: float, *, freq: float, octaves: int, gain: float,
           lacunarity: float, seed: int) -> float:
    """Ridged multifractal — sharp ridges instead of rounded hills. Each
    octave is folded around 0.5 (|2·n − 1|) and squared so the high-amp
    bands accumulate at the ridges. Output normalised to [0,1]."""
    total = 0.0
    amp_sum = 0.0
    amp = 1.0
    fx, fy = x * freq, y * freq
    for _ in range(octaves):
        n = value_noise_2d(fx, fy, seed)
        n = 1.0 - abs(2.0 * n - 1.0)
        n = n * n
        total += amp * n
        amp_sum += amp
        amp *= gain
        fx *= lacunarity
        fy *= lacunarity
    return total / amp_sum if amp_sum > 0.0 else 0.0


# --- Heightmap loaders ----------------------------------------------------

def load_pgm(path: str) -> Tuple[List[List[float]], int, int]:
    """Minimal PGM (P5 binary or P2 ASCII) loader. Returns rows × cols of
    floats in [0,1]. Skips comment lines (#)."""
    with open(path, "rb") as f:
        magic = f.readline().strip()
        if magic not in (b"P5", b"P2"):
            raise ValueError(f"expected PGM (P5/P2), got {magic!r}")

        def _next_token() -> str:
            buf = b""
            while True:
                c = f.read(1)
                if not c:
                    raise ValueError("unexpected EOF in PGM header")
                if c == b"#":
                    f.readline()  # comment to end of line
                    continue
                if c.isspace():
                    if buf:
                        return buf.decode("ascii")
                    continue
                buf += c

        w = int(_next_token())
        h = int(_next_token())
        maxval = int(_next_token())
        # Header tokens are whitespace-separated; the byte after `maxval`
        # is one whitespace, then pixel data starts (P5) / continues (P2).
        rows: List[List[float]] = []
        if magic == b"P5":
            byte_size = 1 if maxval < 256 else 2
            data = f.read(w * h * byte_size)
            if len(data) != w * h * byte_size:
                raise ValueError("PGM body shorter than declared")
            inv = 1.0 / float(maxval)
            for y in range(h):
                row = []
                for x in range(w):
                    if byte_size == 1:
                        row.append(data[y * w + x] * inv)
                    else:
                        # PGM is big-endian for 16-bit
                        v = struct.unpack_from(">H", data, (y * w + x) * 2)[0]
                        row.append(v * inv)
                rows.append(row)
        else:  # P2 ASCII
            text = f.read().split()
            inv = 1.0 / float(maxval)
            for y in range(h):
                rows.append([int(text[y * w + x]) * inv for x in range(w)])
    return rows, w, h


# --- Mesh build -----------------------------------------------------------

def build_grid_mesh(
    *,
    size_x: float, size_z: float,
    res_x: int, res_z: int,
    height_fn,
    edge_falloff: float = 0.0,
) -> Tuple[List[Tuple[float, float, float]],
           List[Tuple[float, float]],
           List[Tuple[float, float, float]],
           List[Tuple[int, int, int]]]:
    """Tessellate a rectangular grid into two triangles per quad. Returns
    (positions, uvs, normals, faces) with faces as 0-indexed vertex tuples.

    `height_fn(u, v)` must accept normalised UV in [0,1]² and return Y.
    `edge_falloff` ∈ [0,1] tapers the height toward the rectangle border so
    the terrain doesn't have a hard cliff at its edge — useful for placing
    a finite mesh inside a "sea" plane that extends further out."""

    if res_x < 2 or res_z < 2:
        raise ValueError("resolution must be ≥ 2 along each axis")

    nx, nz = res_x, res_z
    dx = size_x / (nx - 1)
    dz = size_z / (nz - 1)
    x0 = -size_x * 0.5
    z0 = -size_z * 0.5

    # --- Heights, with optional edge falloff to avoid wall-like borders.
    heights = [[0.0] * nx for _ in range(nz)]
    for j in range(nz):
        v = j / (nz - 1)
        for i in range(nx):
            u = i / (nx - 1)
            h = height_fn(u, v)
            if edge_falloff > 0.0:
                # Smooth edge mask: 1 at centre, 0 at borders; raised cosine
                # over the outer `edge_falloff` fraction of each axis.
                eu = min(u, 1.0 - u) / edge_falloff if edge_falloff > 0 else 1.0
                ev = min(v, 1.0 - v) / edge_falloff if edge_falloff > 0 else 1.0
                m = min(1.0, eu) * min(1.0, ev)
                m = 0.5 - 0.5 * math.cos(math.pi * m)
                h *= m
            heights[j][i] = h

    # --- Vertex positions + UVs.
    positions: List[Tuple[float, float, float]] = []
    uvs: List[Tuple[float, float]] = []
    for j in range(nz):
        z = z0 + j * dz
        v = j / (nz - 1)
        for i in range(nx):
            x = x0 + i * dx
            u = i / (nx - 1)
            positions.append((x, heights[j][i], z))
            uvs.append((u, v))

    # --- Per-vertex normals via central differences on the height field.
    normals: List[Tuple[float, float, float]] = []
    for j in range(nz):
        for i in range(nx):
            ip = min(nx - 1, i + 1)
            im = max(0, i - 1)
            jp = min(nz - 1, j + 1)
            jm = max(0, j - 1)
            hx = (heights[j][ip] - heights[j][im]) / ((ip - im) * dx if ip != im else dx)
            hz = (heights[jp][i] - heights[jm][i]) / ((jp - jm) * dz if jp != jm else dz)
            # Surface gradient is (hx, 1, hz); normal is the gradient with
            # X/Z negated (points up). Normalise.
            nxv, nyv, nzv = -hx, 1.0, -hz
            l = math.sqrt(nxv * nxv + nyv * nyv + nzv * nzv)
            normals.append((nxv / l, nyv / l, nzv / l))

    # --- Triangulate. Two tris per quad; alternate diagonal direction so
    # the wireframe stays balanced and noise lobes don't always tile along
    # the same diagonal.
    faces: List[Tuple[int, int, int]] = []
    for j in range(nz - 1):
        for i in range(nx - 1):
            a = j * nx + i           # (i,   j  )
            b = j * nx + (i + 1)     # (i+1, j  )
            c = (j + 1) * nx + i     # (i,   j+1)
            d = (j + 1) * nx + (i + 1)  # (i+1, j+1)
            if (i + j) & 1:
                faces.append((a, b, d))
                faces.append((a, d, c))
            else:
                faces.append((a, b, c))
                faces.append((b, d, c))
    return positions, uvs, normals, faces


def write_obj(path: str,
              positions: List[Tuple[float, float, float]],
              uvs: List[Tuple[float, float]],
              normals: List[Tuple[float, float, float]],
              faces: List[Tuple[int, int, int]],
              *, object_name: str = "terrain") -> None:
    """Wavefront .obj writer. raygen's loader reads v / vt / vn / f and
    expects 1-indexed face refs in v/vt/vn or v//vn form."""
    with open(path, "w") as f:
        f.write(f"# Generated by tools/terrain/terrain_gen.py — {len(faces)} tris\n")
        f.write(f"o {object_name}\n")
        for x, y, z in positions:
            f.write(f"v {x:.6f} {y:.6f} {z:.6f}\n")
        for u, v in uvs:
            f.write(f"vt {u:.6f} {v:.6f}\n")
        for nx, ny, nz in normals:
            f.write(f"vn {nx:.6f} {ny:.6f} {nz:.6f}\n")
        f.write(f"s 1\n")
        for a, b, c in faces:
            # Wavefront .obj is 1-indexed.
            ai, bi, ci = a + 1, b + 1, c + 1
            f.write(f"f {ai}/{ai}/{ai} {bi}/{bi}/{bi} {ci}/{ci}/{ci}\n")


# --- CLI ------------------------------------------------------------------

def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--output", "-o", default="terrain.obj",
                   help="output .obj path (default: terrain.obj)")
    p.add_argument("--size", nargs=2, type=float, default=[20.0, 20.0],
                   metavar=("X", "Z"),
                   help="terrain extent in mesh-local units (default: 20 20). "
                        "Match Blender's convention of authoring small meshes "
                        "and scaling them up in the scene JSON — raygen's "
                        "MAX_RAY_DISTANCE caps geometry that's hundreds of "
                        "units across, and floating-point precision degrades "
                        "for grazing rays on very flat large meshes.")
    p.add_argument("--res", nargs=2, type=int, default=[192, 192],
                   metavar=("NX", "NZ"),
                   help="grid resolution; vertex count is NX·NZ (default: 192 192)")
    p.add_argument("--amplitude", "-a", type=float, default=1.2,
                   help="max elevation amplitude in mesh-local units (default: 1.2)")
    p.add_argument("--frequency", "-f", type=float, default=0.12,
                   help="base noise frequency in cycles/mesh-unit (default: 0.12)")
    p.add_argument("--octaves", type=int, default=6,
                   help="fBm octaves (default: 6)")
    p.add_argument("--gain", type=float, default=0.5,
                   help="amplitude ratio between octaves (default: 0.5)")
    p.add_argument("--lacunarity", type=float, default=2.05,
                   help="frequency ratio between octaves (default: 2.05)")
    p.add_argument("--seed", type=int, default=1,
                   help="RNG seed for the noise lattice offset (default: 1)")
    p.add_argument("--ridges", action="store_true",
                   help="use ridged-multifractal instead of fBm (sharp peaks)")
    p.add_argument("--seabed", type=float, default=None, metavar="Y",
                   help="clamp heights below this Y to a flat seabed (useful for "
                        "ocean scenes where terrain dips under a water plane)")
    p.add_argument("--bias", type=float, default=0.0,
                   help="constant offset added to every vertex's Y (default: 0.0)")
    p.add_argument("--falloff", type=float, default=0.0,
                   help="edge falloff fraction [0,1] — taper height toward the "
                        "border so the rectangle blends into a surrounding plane "
                        "(default: 0 = no falloff)")
    p.add_argument("--heightmap", default=None,
                   help="load heights from a PGM image instead of generating; "
                        "amplitude still scales the [0,1] sample range")
    p.add_argument("--name", default="terrain",
                   help="object name written to the .obj (default: terrain)")
    args = p.parse_args(argv)

    size_x, size_z = args.size
    res_x, res_z = args.res

    # Build the height function.
    if args.heightmap:
        rows, hw, hh = load_pgm(args.heightmap)
        amp = args.amplitude

        def height_fn(u: float, v: float, _rows=rows, _hw=hw, _hh=hh, _amp=amp) -> float:
            # Bilinear sample of the loaded heightmap.
            x = u * (_hw - 1)
            y = v * (_hh - 1)
            xi, yi = int(x), int(y)
            xf, yf = x - xi, y - yi
            xp = min(_hw - 1, xi + 1)
            yp = min(_hh - 1, yi + 1)
            a = _rows[yi][xi] * (1 - xf) + _rows[yi][xp] * xf
            b = _rows[yp][xi] * (1 - xf) + _rows[yp][xp] * xf
            return _amp * (a * (1 - yf) + b * yf)
    else:
        # World-space coordinates the noise basis sees: independent of the
        # (res_x, res_z) grid resolution so refining the mesh doesn't change
        # the terrain shape — the lower octaves stay put and the higher ones
        # just get sampled more accurately.
        amp = args.amplitude
        freq = args.frequency
        octv = args.octaves
        gain = args.gain
        lac = args.lacunarity
        seed = args.seed
        ridge = args.ridges
        sx, sz = size_x, size_z

        def height_fn(u: float, v: float,
                      _amp=amp, _freq=freq, _oct=octv, _gain=gain, _lac=lac,
                      _seed=seed, _ridge=ridge, _sx=sx, _sz=sz) -> float:
            x = (u - 0.5) * _sx
            z = (v - 0.5) * _sz
            if _ridge:
                n = ridged(x, z, freq=_freq, octaves=_oct, gain=_gain,
                           lacunarity=_lac, seed=_seed)
            else:
                n = fbm(x, z, freq=_freq, octaves=_oct, gain=_gain,
                        lacunarity=_lac, seed=_seed)
            # Centre on zero so half the terrain is above mean sea level
            # and half below — a useful default when the result will sit
            # alongside a water plane at Y=0.
            return _amp * (n - 0.5) * 2.0

    bias = args.bias
    seabed = args.seabed

    def biased(u: float, v: float) -> float:
        h = height_fn(u, v) + bias
        if seabed is not None and h < seabed:
            return seabed
        return h

    print(f"Generating {res_x}×{res_z} grid over {size_x}×{size_z} m …", file=sys.stderr)
    positions, uvs, normals, faces = build_grid_mesh(
        size_x=size_x, size_z=size_z,
        res_x=res_x, res_z=res_z,
        height_fn=biased,
        edge_falloff=args.falloff,
    )

    write_obj(args.output, positions, uvs, normals, faces, object_name=args.name)
    ymin = min(p[1] for p in positions)
    ymax = max(p[1] for p in positions)
    print(f"Wrote {args.output}: {len(positions)} verts, {len(faces)} tris, "
          f"Y ∈ [{ymin:.3f}, {ymax:.3f}]", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
