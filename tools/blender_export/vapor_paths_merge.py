#!/usr/bin/env python3
# Merge `vapor.<group>.<index>` Empty markers from a .blend into an existing
# raygen scene JSON, in place, without touching anything else.
#
# Run via Blender in background mode:
#
#   blender -b path/to/scene.blend -P vapor_paths_merge.py -- \
#       --target path/to/scene.json
#
# Anything after `--` is forwarded to this script. Unlike blend_to_raygen.py
# which writes a fresh JSON from scratch, this tool surgically replaces the
# region of the JSON delimited by:
#
#   // <BLENDER_VAPOR_BEGIN>
#   ... (auto-generated wingVapor_* SceneObjects) ...
#   // <BLENDER_VAPOR_END>
#
# Hand-tuned blocks outside that region (engineFlame, custom materials, etc.)
# are preserved byte-for-byte. The user is expected to add the marker pair
# once, inside the owning SceneObject (typically the `f2` block), at the spot
# where the wing vapors should appear.
#
# Empty naming convention:
#   vapor.<groupName>.<index>   e.g. vapor.wingR.001, vapor.wingL.003
#
# `<index>` is any run of digits — Blender's auto-suffix on Ctrl+D
# (`.001`, `.002`, ...) drops in directly. Wider widths still sort
# correctly since we parse the digits as integers.
#
# Each Empty:
#   - must be parented to the owning mesh (so its `matrix_local` is in the
#     mesh's local frame; that frame also matches raygen `pathFollowObject`)
#   - optional Custom Property "radius" (defaults to empty_display_size)
#
# Per-group tunable defaults are in TUNING below — edit there, not in the
# generated JSON, since the merge re-emits the block on each run.

import argparse
import json
import math
import os
import re
import sys
from collections import OrderedDict

import bpy
from mathutils import Matrix, Vector


# ---------------------------------------------------------------------------
# Tunable defaults (group-level)
# ---------------------------------------------------------------------------
#
# Each entry is the default medium-block payload for a wingVapor_<group>
# SceneObject. Override per-group by adding a key whose name matches the
# Empty's `<groupName>`. Anything not present falls through to "_default".

TUNING = {
    "_default": {
        "pathInner":          [3.0, 3.2, 3.6],
        "pathOuter":          [0.6, 0.7, 0.9],
        "pathIntensity":      1.6,
        "pathFalloffPower":   2.5,
        "pathEmissionSamples": 8,
        "noiseFrequency":     4.5,
        "noiseOctaves":       4,
        "noiseGain":          0.55,
        "noiseLacunarity":    2.1,
        "noiseAmplitude":     1.8,
        "noiseBias":         -0.4,
        # Padding (multiplier on AABB extent) for the bounding cube. >1 gives
        # the FBm modulation room to fade out at the edges; 1.0 hugs tight.
        "bboxPadding":        1.4,
        # Minimum cube extent in any axis — prevents zero-volume cubes when
        # all Empties happen to share a coordinate (e.g. two markers stacked
        # along just X). Small enough not to dominate normal layouts.
        "minExtent":          0.4,
    },
    # Example group override:
    # "wingR": { "pathIntensity": 2.4, "noiseAmplitude": 2.1 },
}


# ---------------------------------------------------------------------------
# Coordinate conversion (Blender → raygen)
# ---------------------------------------------------------------------------
#
# Same convention as blend_to_raygen.py. Vapor Empties are read in their
# parent mesh's *local* frame so that pathFollowObject=true works correctly:
# raygen composes the owning SceneObject's modelMatrix on top of the points,
# so positions need to be in the same local frame the OBJ vertices were
# baked in.

B2R = Matrix(((1, 0, 0, 0),
              (0, 0, 1, 0),
              (0, -1, 0, 0),
              (0, 0, 0, 1)))


def b2r_local_position(empty_obj):
    """Position of `empty_obj` in its parent's local frame, axis-converted
    into raygen space. Falls back to world-space (axis-converted) when the
    Empty has no parent — in that case the user should set the owning
    SceneObject to identity in raygen JSON."""
    if empty_obj.parent is not None:
        local = empty_obj.parent.matrix_world.inverted() @ empty_obj.matrix_world
    else:
        local = empty_obj.matrix_world
    p = local.translation
    out = B2R @ Vector((p.x, p.y, p.z, 1.0))
    return Vector((out.x, out.y, out.z))


# ---------------------------------------------------------------------------
# Vapor marker collection
# ---------------------------------------------------------------------------

# Group name allows letters/digits/underscore but NOT bare digits (the index
# is the digit-only suffix, separated by the last dot). The regex matches
# anything from `vapor.wingR.1` through `vapor.wingR.0001` — index width is
# whatever Blender's duplicate auto-suffix produces, which depends on the
# starting name (`vapor.wingR` → `vapor.wingR.001`; `vapor.wingR.001` →
# `vapor.wingR.002`). Both styles sort by integer value of the suffix.
VAPOR_RE = re.compile(r'^vapor\.([A-Za-z_][A-Za-z0-9_]*)\.(\d+)$')


def collect_vapor_groups():
    """Walk bpy.data.objects, return { groupName: [(index, posLocal, radius), ...] }
    sorted by index inside each group."""
    groups = {}
    for obj in bpy.data.objects:
        m = VAPOR_RE.match(obj.name)
        if not m or obj.type != 'EMPTY':
            continue
        group = m.group(1)
        idx = int(m.group(2))
        pos = b2r_local_position(obj)
        radius = float(obj.get("radius", obj.empty_display_size))
        groups.setdefault(group, []).append((idx, pos, radius))
    for g in groups.values():
        g.sort(key=lambda x: x[0])
    return groups


# ---------------------------------------------------------------------------
# SceneObject construction
# ---------------------------------------------------------------------------

def group_tuning(group_name):
    out = dict(TUNING["_default"])
    if group_name in TUNING:
        out.update(TUNING[group_name])
    return out


def build_vapor_scene_object(group_name, points):
    """`points` = [(idx, Vector pos_local, radius), ...] sorted by idx.
    Returns an OrderedDict shaped like a raygen SceneObject ready to be
    pretty-printed."""
    if len(points) < 2:
        raise ValueError("group %r needs at least 2 markers (got %d)"
                         % (group_name, len(points)))

    cfg = group_tuning(group_name)

    # Compute AABB padded by per-point radius — the cube must enclose every
    # tube, not just the centerline. Tracks min/max in each axis separately.
    min_v = Vector(( math.inf,  math.inf,  math.inf))
    max_v = Vector((-math.inf, -math.inf, -math.inf))
    for _idx, p, r in points:
        for axis in range(3):
            min_v[axis] = min(min_v[axis], p[axis] - r)
            max_v[axis] = max(max_v[axis], p[axis] + r)

    # Pad and clamp minimum extent so the cube never collapses to zero.
    pad = float(cfg["bboxPadding"])
    min_ext = float(cfg["minExtent"])
    centre = (min_v + max_v) * 0.5
    extent = Vector((max(max_v[i] - min_v[i], min_ext) for i in range(3))) * pad

    cube_loc = [round(centre.x, 6), round(centre.y, 6), round(centre.z, 6)]
    cube_scale = [round(extent.x, 6), round(extent.y, 6), round(extent.z, 6)]

    # Normalise each control point into cube-local [-0.5, 0.5]^3. radius is
    # in the same world units as the parent's local frame, NOT in cube-local
    # units, so it doesn't get the same divide. raygen interprets pathPoint
    # `radius` as a length in the cube-local frame after the cube's scale is
    # applied (i.e. world-distance), so we leave it as authored.
    n = len(points)
    inv_scale = Vector((1.0 / extent.x if extent.x > 1e-9 else 0.0,
                        1.0 / extent.y if extent.y > 1e-9 else 0.0,
                        1.0 / extent.z if extent.z > 1e-9 else 0.0))
    path_points = []
    for i, (_idx, p, r) in enumerate(points):
        rel = p - centre
        # `radius` here is in *world* (parent-local) units. The cube's scale
        # converts cube-local → world, so when raygen evaluates the path
        # the supplied radius is interpreted as world-distance directly.
        local = [round(rel.x * inv_scale.x, 6),
                 round(rel.y * inv_scale.y, 6),
                 round(rel.z * inv_scale.z, 6)]
        t = (i / (n - 1)) if n > 1 else 0.0
        path_points.append(OrderedDict([
            ("p", local),
            ("radius", round(r, 6)),
            ("t", round(t, 4)),
        ]))

    medium = OrderedDict([
        ("sigma_a", [0, 0, 0]),
        ("sigma_s", [0, 0, 0]),
        ("emissionMode", "path"),
        ("pathFollowObject", True),
        ("pathInner", cfg["pathInner"]),
        ("pathOuter", cfg["pathOuter"]),
        ("pathIntensity", cfg["pathIntensity"]),
        ("pathFalloffPower", cfg["pathFalloffPower"]),
        ("pathEmissionSamples", cfg["pathEmissionSamples"]),
        ("pathPoints", path_points),
        ("densityField", "fbm"),
        ("noiseFrequency", cfg["noiseFrequency"]),
        ("noiseOctaves", cfg["noiseOctaves"]),
        ("noiseGain", cfg["noiseGain"]),
        ("noiseLacunarity", cfg["noiseLacunarity"]),
        ("noiseAmplitude", cfg["noiseAmplitude"]),
        ("noiseBias", cfg["noiseBias"]),
    ])

    return OrderedDict([
        ("location", cube_loc),
        ("scale", cube_scale),
        ("mesh", OrderedDict([("type", "cube")])),
        ("mat", OrderedDict([("color", "#333333"), ("transparency", 1.0)])),
        ("medium", medium),
    ])


# ---------------------------------------------------------------------------
# Marker-block merge
# ---------------------------------------------------------------------------

BEGIN_MARKER = "// <BLENDER_VAPOR_BEGIN>"
END_MARKER   = "// <BLENDER_VAPOR_END>"


def render_block(scene_objs, indent="      "):
    """Render the SceneObject dict as JSONC fragment with the given leading
    indent on each line. Trailing newline omitted; caller handles join."""
    lines = []
    n = len(scene_objs)
    for i, (key, body) in enumerate(scene_objs.items()):
        body_text = json.dumps(body, indent=2, ensure_ascii=False)
        # Re-indent multi-line JSON: prefix every line after the first with
        # `indent`, since json.dumps is left-flush.
        body_lines = body_text.split('\n')
        prefixed = [body_lines[0]] + [indent + ln for ln in body_lines[1:]]
        comma = "," if i + 1 < n else ""
        lines.append('%s"%s": %s%s' % (indent, key, '\n'.join(prefixed), comma))
    return '\n'.join(lines)


def merge_into_target(target_path, vapor_scene_objs):
    with open(target_path, 'r', encoding='utf-8') as f:
        text = f.read()

    begin_idx = text.find(BEGIN_MARKER)
    end_idx   = text.find(END_MARKER)
    if begin_idx < 0 or end_idx < 0 or end_idx < begin_idx:
        raise RuntimeError(
            "marker pair not found in %s — add this once inside the owning "
            "SceneObject (typically `f2`), at the position where wing vapor "
            "blocks should appear:\n\n"
            "      %s\n"
            "      %s\n"
            % (target_path, BEGIN_MARKER, END_MARKER))

    # The replacement region is from the END of the BEGIN marker line up to
    # the START of the END marker line — preserves the marker comments and
    # whatever leading indent the user used on them.
    begin_line_end = text.find('\n', begin_idx)
    if begin_line_end < 0:
        raise RuntimeError("malformed BEGIN marker (missing newline)")

    # Walk back from the END marker to the start of its line so we preserve
    # the user's chosen indent on the END marker line.
    end_line_start = text.rfind('\n', 0, end_idx) + 1

    # Indent inferred from the BEGIN marker's column.
    begin_line_start = text.rfind('\n', 0, begin_idx) + 1
    indent = text[begin_line_start:begin_idx]

    if vapor_scene_objs:
        block_body = render_block(vapor_scene_objs, indent=indent)
        # Trailing comma on the last entry: required because what follows
        # the END marker is either another property (then comma is needed)
        # or the closing brace of the owning object (then it's tolerated by
        # the parser, since trailing commas are accepted in JSONC). Easier
        # to always emit a trailing comma than to peek ahead.
        replacement = '\n' + block_body + ',\n' + indent
    else:
        # Empty group set: collapse the region to a blank line so the
        # markers stay but no JSON content is generated. Re-running with
        # markers present and zero Empties resets the block cleanly.
        replacement = '\n' + indent

    new_text = text[:begin_line_end + 1] + replacement + text[end_line_start:]

    with open(target_path, 'w', encoding='utf-8') as f:
        f.write(new_text)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args():
    if '--' in sys.argv:
        argv = sys.argv[sys.argv.index('--') + 1:]
    else:
        argv = []
    p = argparse.ArgumentParser(prog='vapor_paths_merge')
    p.add_argument('--target', required=True,
                   help='path to the existing raygen scene .json to merge into')
    p.add_argument('--dry-run', action='store_true',
                   help='print the would-be replacement block to stdout, do not write')
    return p.parse_args(argv)


def main():
    args = parse_args()
    target = os.path.abspath(args.target)
    if not os.path.isfile(target):
        sys.exit("[vapor_paths_merge] target not found: %s" % target)

    groups = collect_vapor_groups()
    print('[vapor_paths_merge] found %d vapor group(s):' % len(groups))
    for g, pts in groups.items():
        print('  - %s (%d points)' % (g, len(pts)))

    scene_objs = OrderedDict()
    for name in sorted(groups.keys()):
        try:
            so = build_vapor_scene_object(name, groups[name])
            scene_objs['wingVapor_' + name] = so
        except ValueError as e:
            print('[vapor_paths_merge] skipping %s: %s' % (name, e))

    if args.dry_run:
        print('--- replacement block ---')
        print(render_block(scene_objs, indent='      '))
        return

    merge_into_target(target, scene_objs)
    print('[vapor_paths_merge] merged %d group(s) into %s'
          % (len(scene_objs), target))


if __name__ == '__main__':
    main()
