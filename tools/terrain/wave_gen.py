#!/usr/bin/env python3
"""Generate a Gerstner-displaced water mesh as a Wavefront .obj.

Gerstner waves (also called trochoid waves) are the textbook ocean
surface model: particles trace circles whose radius is the wave
amplitude, producing sharp crests and rounded troughs — visually more
ocean-like than a plain sine sum. The script bakes a *single time
sample* of an N-wave Gerstner sum into a static mesh raygen can BVH;
re-run with `--phase` shifted to pick a different "frame" of the same
wave field, or with a different `--seed` to roll a different sea state.

Each wave i is parameterised by:

    direction Di — unit 2D vector in the XZ plane
    wavelength Li, hence wavenumber ω_i = 2π / L_i
    amplitude  A_i
    phase      φ_i (in radians; this is what `--phase` shifts uniformly)
    steepness  Q_i ∈ [0, 1]; 1 = sharp crests, 0 = pure sine

Displacement at horizontal (x, z):

    θ = ω · (D · (x,z)) + φ
    Δx = -(D.x / ω) · A · Q · sin θ
    Δz = -(D.y / ω) · A · Q · sin θ
    y  =  A · cos θ

summed across all waves. The "base" wavelength / amplitude / direction
arguments seed an exponential spectrum: wave i's wavelength is
`base * lambda_falloff^i` and its amplitude scales with sqrt(L) (rough
deep-water proxy). Q is shared across waves to keep the steepness budget
sane — past Q≈0.7 the surface starts self-intersecting, which
manifests as inverted normals and is usually wrong.

Output mesh is a regular X/Z grid (like the terrain script), so it
sits cleanly under a coast scene and the underwater BVH path through
it stays straight (the displacement only affects the surface — the
seabed is a separate mesh).

Usage examples:

    # Default 200×200 m calm-ish sea, 4 waves:
    python3 wave_gen.py --output /tmp/sea.obj

    # Bigger swell, only 3 long-period waves, sharper crests:
    python3 wave_gen.py --waves 3 --base-wavelength 24 \\
        --base-amplitude 0.6 --steepness 0.55 \\
        --size 400 400 --res 384 384 --output /tmp/swell.obj

    # Same sea state at a different "time":
    python3 wave_gen.py --seed 7 --phase 1.7 --output /tmp/sea_t1.obj
"""

from __future__ import annotations

import argparse
import math
import random
import sys
from typing import List, Tuple


# --- Wave spectrum --------------------------------------------------------

class GerstnerWave:
    __slots__ = ("dx", "dz", "k", "amp", "phase", "Q_over_k")

    def __init__(self, dir_xz: Tuple[float, float], wavelength: float,
                 amplitude: float, phase: float, steepness: float):
        # Pre-bake the (Q/ω) and (D/ω) terms — every vertex evaluation
        # uses them, and they don't depend on (x,z).
        l = math.hypot(*dir_xz)
        if l < 1e-9:
            dir_xz = (1.0, 0.0)
            l = 1.0
        self.dx = dir_xz[0] / l
        self.dz = dir_xz[1] / l
        self.k = 2.0 * math.pi / max(1e-6, wavelength)
        self.amp = amplitude
        self.phase = phase
        # Q controls horizontal motion magnitude. Per-wave normalisation
        # by 1/(k·N) would rigorously cap total steepness, but in
        # practice scaling by an external `--steepness` knob and
        # eyeballing self-intersections is what artists do.
        self.Q_over_k = steepness / self.k

    def displace(self, x: float, z: float) -> Tuple[float, float, float]:
        theta = self.k * (self.dx * x + self.dz * z) + self.phase
        s = math.sin(theta)
        c = math.cos(theta)
        return (-self.dx * self.amp * self.Q_over_k * self.k * s,
                self.amp * c,
                -self.dz * self.amp * self.Q_over_k * self.k * s)
        # Note: Δx = -D.x · A · Q · sinθ, since Q_over_k = Q/k and we
        # multiply by k → Δx = -D.x · A · Q · sinθ. (The pre-bake keeps
        # the inner loop branch-free.)


def build_spectrum(*, count: int, base_wavelength: float, base_amplitude: float,
                   lambda_falloff: float, base_dir_deg: float,
                   dir_spread_deg: float, steepness: float, seed: int,
                   phase_shift: float) -> List[GerstnerWave]:
    rng = random.Random(seed)
    waves: List[GerstnerWave] = []
    for i in range(count):
        # Wavelength shrinks geometrically; amplitude follows ~sqrt(L)
        # so longer waves are taller (matches deep-water dispersion).
        L = base_wavelength * (lambda_falloff ** i)
        A = base_amplitude * math.sqrt(max(1e-6, L / base_wavelength))
        # Direction jittered around the wind heading.
        ang = math.radians(base_dir_deg + rng.uniform(-1.0, 1.0) * dir_spread_deg)
        # Phase: random per wave, plus the global phase shift the user
        # passes in to "advance time" without re-seeding.
        ph = rng.uniform(0.0, 2.0 * math.pi) + phase_shift * (i + 1)
        waves.append(GerstnerWave(
            dir_xz=(math.cos(ang), math.sin(ang)),
            wavelength=L,
            amplitude=A,
            phase=ph,
            steepness=steepness,
        ))
    return waves


# --- Mesh build -----------------------------------------------------------

def build_water_mesh(
    *,
    size_x: float, size_z: float,
    res_x: int, res_z: int,
    waves: List[GerstnerWave],
) -> Tuple[List[Tuple[float, float, float]],
           List[Tuple[float, float]],
           List[Tuple[float, float, float]],
           List[Tuple[int, int, int]]]:
    if res_x < 2 or res_z < 2:
        raise ValueError("resolution must be ≥ 2 along each axis")

    nx, nz = res_x, res_z
    dx = size_x / (nx - 1)
    dz = size_z / (nz - 1)
    x0 = -size_x * 0.5
    z0 = -size_z * 0.5

    # --- Pass 1: displaced positions + UVs at undisturbed grid points.
    # The UV uses the ORIGINAL grid coords so a tiling texture stays
    # aligned with the world XZ axes regardless of the wave field.
    positions: List[Tuple[float, float, float]] = []
    uvs: List[Tuple[float, float]] = []
    for j in range(nz):
        z = z0 + j * dz
        v = j / (nz - 1)
        for i in range(nx):
            x = x0 + i * dx
            u = i / (nx - 1)
            sx = sy = sz = 0.0
            for w in waves:
                ddx, ddy, ddz = w.displace(x, z)
                sx += ddx
                sy += ddy
                sz += ddz
            positions.append((x + sx, sy, z + sz))
            uvs.append((u, v))

    # --- Pass 2: per-vertex normals via finite differences on the
    # displaced surface. We use the *displaced* neighbours so the normal
    # captures the true surface orientation, not the analytical
    # ∂y/∂x of the height field (which ignores the horizontal motion).
    # Border vertices fall back to one-sided differences.
    normals: List[Tuple[float, float, float]] = []
    for j in range(nz):
        for i in range(nx):
            ip = min(nx - 1, i + 1)
            im = max(0, i - 1)
            jp = min(nz - 1, j + 1)
            jm = max(0, j - 1)
            pxp = positions[j * nx + ip]
            pxm = positions[j * nx + im]
            pzp = positions[jp * nx + i]
            pzm = positions[jm * nx + i]
            tx = (pxp[0] - pxm[0], pxp[1] - pxm[1], pxp[2] - pxm[2])
            tz = (pzp[0] - pzm[0], pzp[1] - pzm[1], pzp[2] - pzm[2])
            # Cross(tz, tx) gives a +Y-up normal for a non-inverted patch.
            nxv = tz[1] * tx[2] - tz[2] * tx[1]
            nyv = tz[2] * tx[0] - tz[0] * tx[2]
            nzv = tz[0] * tx[1] - tz[1] * tx[0]
            l = math.sqrt(nxv * nxv + nyv * nyv + nzv * nzv)
            if l < 1e-9:
                normals.append((0.0, 1.0, 0.0))
            else:
                normals.append((nxv / l, nyv / l, nzv / l))

    # --- Triangulate, alternating diagonal direction quad-by-quad.
    faces: List[Tuple[int, int, int]] = []
    for j in range(nz - 1):
        for i in range(nx - 1):
            a = j * nx + i
            b = j * nx + (i + 1)
            c = (j + 1) * nx + i
            d = (j + 1) * nx + (i + 1)
            if (i + j) & 1:
                faces.append((a, b, d))
                faces.append((a, d, c))
            else:
                faces.append((a, b, c))
                faces.append((b, d, c))
    return positions, uvs, normals, faces


def write_obj(path: str,
              positions, uvs, normals, faces,
              *, object_name: str = "water") -> None:
    with open(path, "w") as f:
        f.write(f"# Generated by tools/terrain/wave_gen.py — {len(faces)} tris\n")
        f.write(f"o {object_name}\n")
        for x, y, z in positions:
            f.write(f"v {x:.6f} {y:.6f} {z:.6f}\n")
        for u, v in uvs:
            f.write(f"vt {u:.6f} {v:.6f}\n")
        for nx, ny, nz in normals:
            f.write(f"vn {nx:.6f} {ny:.6f} {nz:.6f}\n")
        f.write(f"s 1\n")
        for a, b, c in faces:
            ai, bi, ci = a + 1, b + 1, c + 1
            f.write(f"f {ai}/{ai}/{ai} {bi}/{bi}/{bi} {ci}/{ci}/{ci}\n")


# --- CLI ------------------------------------------------------------------

def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--output", "-o", default="water.obj",
                   help="output .obj path (default: water.obj)")
    p.add_argument("--size", nargs=2, type=float, default=[200.0, 200.0],
                   metavar=("X", "Z"),
                   help="water-plane extent in metres (default: 200 200)")
    p.add_argument("--res", nargs=2, type=int, default=[256, 256],
                   metavar=("NX", "NZ"),
                   help="grid resolution (default: 256 256). Higher = sharper "
                        "crests; below ~0.5 vertices per wavelength the wave "
                        "aliases into low-frequency noise.")
    p.add_argument("--waves", "-n", type=int, default=4,
                   help="number of summed Gerstner waves (default: 4). 3-6 "
                        "covers most practical sea states.")
    p.add_argument("--base-wavelength", type=float, default=12.0,
                   help="longest wave's wavelength in metres (default: 12)")
    p.add_argument("--base-amplitude", type=float, default=0.35,
                   help="longest wave's amplitude in metres (default: 0.35). "
                        "This is the half-height; peak-to-trough is 2× this.")
    p.add_argument("--lambda-falloff", type=float, default=0.62,
                   help="ratio between successive wavelengths (default: 0.62). "
                        "0.5 = each wave is half the previous; >0.65 keeps more "
                        "energy at long periods.")
    p.add_argument("--steepness", "-Q", type=float, default=0.45,
                   help="Q ∈ [0,1] (default: 0.45). 0 = pure sine; ~0.7 "
                        "self-intersects.")
    p.add_argument("--direction", type=float, default=0.0,
                   help="prevailing wind heading in degrees from +X (default: 0)")
    p.add_argument("--direction-spread", type=float, default=45.0,
                   help="±degrees random spread around the wind heading "
                        "(default: 45). 0 = all waves co-aligned (clean swell); "
                        "90 = chaotic chop.")
    p.add_argument("--phase", type=float, default=0.0,
                   help="global phase advance, in radians (default: 0). "
                        "Re-bake with the same --seed and a shifted phase to "
                        "produce a 'next frame' of the same sea state.")
    p.add_argument("--seed", type=int, default=1,
                   help="RNG seed for per-wave direction / phase jitter (default: 1)")
    p.add_argument("--y-offset", type=float, default=0.0,
                   help="add a constant Y offset to every vertex (default: 0)")
    p.add_argument("--name", default="water",
                   help="object name written to the .obj (default: water)")
    args = p.parse_args(argv)

    waves = build_spectrum(
        count=args.waves,
        base_wavelength=args.base_wavelength,
        base_amplitude=args.base_amplitude,
        lambda_falloff=args.lambda_falloff,
        base_dir_deg=args.direction,
        dir_spread_deg=args.direction_spread,
        steepness=args.steepness,
        seed=args.seed,
        phase_shift=args.phase,
    )

    print(f"Spectrum: {len(waves)} waves", file=sys.stderr)
    for i, w in enumerate(waves):
        print(f"  [{i}] λ={2*math.pi/w.k:.2f} m  A={w.amp:.3f} m  "
              f"dir=({w.dx:+.3f}, {w.dz:+.3f})", file=sys.stderr)

    print(f"Generating {args.res[0]}×{args.res[1]} grid over "
          f"{args.size[0]}×{args.size[1]} m …", file=sys.stderr)
    positions, uvs, normals, faces = build_water_mesh(
        size_x=args.size[0], size_z=args.size[1],
        res_x=args.res[0], res_z=args.res[1],
        waves=waves,
    )

    if args.y_offset != 0.0:
        positions = [(p[0], p[1] + args.y_offset, p[2]) for p in positions]

    write_obj(args.output, positions, uvs, normals, faces,
              object_name=args.name)
    ymin = min(p[1] for p in positions)
    ymax = max(p[1] for p in positions)
    print(f"Wrote {args.output}: {len(positions)} verts, {len(faces)} tris, "
          f"Y ∈ [{ymin:.3f}, {ymax:.3f}]", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
