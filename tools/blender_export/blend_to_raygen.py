#!/usr/bin/env python3
# Convert a .blend file to a raygen scene JSON (+ companion .obj/.mtl).
#
# Run via Blender in background mode:
#
#   blender -b path/to/scene.blend -P blend_to_raygen.py -- \
#       --out path/to/raygen_export/scene.json
#
# Anything after `--` is forwarded to this script. Blender swallows everything
# before it. Output file basename is used for the .obj/.mtl pair next to the
# .json. The script writes a baseline scene that almost always needs hand
# tuning (emission strengths, glossy/roughness sliders, envmap intensity etc.).
# That's intentional — see CLAUDE.md F2.json for a worked example of what the
# final, hand-tuned JSON looks like.

import argparse
import json
import math
import os
import sys
from collections import OrderedDict

import bpy
from mathutils import Matrix, Vector


# ---------------------------------------------------------------------------
# Coordinate conversion
# ---------------------------------------------------------------------------
#
# Blender:   X right, Y forward, Z up   (right-handed)
# raygen:    X right, Y up,      -Z forward  (right-handed, GL-style)
#
# The OBJ exporter handles per-vertex axis conversion for the mesh data
# itself, so the world entry can sit at the origin with identity transform.
# We still need the *camera* and *light* transforms in raygen-space because
# those live in the JSON, not in the OBJ.

# Rotation that takes a Blender-space vector into raygen-space:
#   raygen.x =  blender.x
#   raygen.y =  blender.z
#   raygen.z = -blender.y
B2R = Matrix(((1, 0, 0, 0),
              (0, 0, 1, 0),
              (0, -1, 0, 0),
              (0, 0, 0, 1)))


def b2r_location(v):
    p = B2R @ Vector((v[0], v[1], v[2], 1.0))
    return [round(p.x, 6), round(p.y, 6), round(p.z, 6)]


def b2r_euler_from_matrix(mat_world, extra_local=None):
    # Convert a Blender world matrix to raygen Euler angles in degrees.
    # `extra_local` (optional 3x3) is post-multiplied in local space — used for
    # cameras (Blender camera looks down -Z, same as raygen, no extra rotation
    # needed) and for area lights (Blender area light emits along -Z too).
    m = mat_world.to_3x3()
    if extra_local is not None:
        m = m @ extra_local
    # Apply the basis change: M_raygen = B2R * M_blender * B2R^-1
    b2r3 = B2R.to_3x3()
    m = b2r3 @ m @ b2r3.transposed()
    e = m.to_euler('XYZ')
    return [round(math.degrees(e.x), 4),
            round(math.degrees(e.y), 4),
            round(math.degrees(e.z), 4)]


# ---------------------------------------------------------------------------
# Material extraction
# ---------------------------------------------------------------------------

def find_principled(mat):
    if not mat or not mat.use_nodes or not mat.node_tree:
        return None
    for n in mat.node_tree.nodes:
        if n.type == 'BSDF_PRINCIPLED':
            return n
    return None


def socket_value(node, name, default=None):
    s = node.inputs.get(name)
    if s is None:
        return default
    if s.is_linked:
        # Texture or other input feeding this socket. We don't traverse the
        # node graph here — texture filenames are picked up separately below.
        return None
    return s.default_value


def find_image_texture(mat, base_socket_names=('Base Color',)):
    if not mat or not mat.use_nodes:
        return None
    bsdf = find_principled(mat)
    if not bsdf:
        return None
    for name in base_socket_names:
        s = bsdf.inputs.get(name)
        if not s or not s.is_linked:
            continue
        link = s.links[0]
        node = link.from_node
        # walk through one optional ColorRamp / Mix
        for _ in range(4):
            if node.type == 'TEX_IMAGE':
                if node.image and node.image.filepath:
                    return os.path.basename(bpy.path.abspath(node.image.filepath))
                return None
            if node.inputs and len(node.inputs) > 0:
                forwarded = next((i for i in node.inputs if i.is_linked), None)
                if forwarded is None:
                    return None
                node = forwarded.links[0].from_node
            else:
                return None
    return None


def material_to_dict(mat):
    out = OrderedDict()
    bsdf = find_principled(mat)
    if bsdf is None:
        # Plain diffuse fallback.
        c = getattr(mat, 'diffuse_color', (0.8, 0.8, 0.8, 1.0))
        out['color'] = [round(c[0], 4), round(c[1], 4), round(c[2], 4)]
        return out

    base = socket_value(bsdf, 'Base Color', (0.8, 0.8, 0.8, 1.0))
    if base is not None:
        out['color'] = [round(base[0], 4), round(base[1], 4), round(base[2], 4)]

    tex = find_image_texture(mat)
    if tex:
        out['tex'] = tex

    metallic = socket_value(bsdf, 'Metallic', 0.0)
    if metallic is not None and metallic > 0.001:
        out['metallic'] = round(float(metallic), 4)

    roughness = socket_value(bsdf, 'Roughness', 0.5)
    if roughness is not None:
        out['roughness'] = round(float(roughness), 4)

    # Specular gives a rough proxy for the glossy lobe weight in raygen.
    # Blender 4.x renamed 'Specular' → 'Specular IOR Level'; try both.
    spec = socket_value(bsdf, 'Specular IOR Level', None)
    if spec is None:
        spec = socket_value(bsdf, 'Specular', None)
    if spec is not None:
        out['glossy'] = round(float(spec), 4)
    else:
        out['glossy'] = 0.5

    # Transmission → refraction lobe.
    transmission = socket_value(bsdf, 'Transmission Weight', None)
    if transmission is None:
        transmission = socket_value(bsdf, 'Transmission', 0.0)
    if transmission is not None and transmission > 0.001:
        out['refraction'] = round(float(transmission), 4)
        ior = socket_value(bsdf, 'IOR', 1.45)
        if ior is not None:
            out['refractionRatio'] = round(float(ior), 4)

    # Alpha < 1 → transparency lobe (straight-through, not refractive).
    alpha = socket_value(bsdf, 'Alpha', 1.0)
    if alpha is not None and alpha < 0.999:
        out['transparency'] = round(1.0 - float(alpha), 4)

    # Emission. Blender splits emission color and strength.
    em_col = socket_value(bsdf, 'Emission Color', None)
    if em_col is None:
        em_col = socket_value(bsdf, 'Emission', None)
    em_str = socket_value(bsdf, 'Emission Strength', 0.0)
    if em_col is not None and em_str and em_str > 0.001:
        # Fold strength into color magnitude when color is colored, else just
        # use strength as the scalar emission and keep the color separately.
        col3 = [em_col[0], em_col[1], em_col[2]]
        # Normalize to a unit-ish color and put magnitude in `emission` so the
        # raygen JSON stays readable.
        peak = max(col3) if max(col3) > 0 else 1.0
        norm = [c / peak for c in col3]
        out['color'] = [round(c, 4) for c in norm]
        out['emission'] = round(float(em_str) * peak, 4)

    return out


# ---------------------------------------------------------------------------
# Camera
# ---------------------------------------------------------------------------

def camera_to_dict(scene):
    cam_obj = scene.camera
    if cam_obj is None:
        for o in scene.objects:
            if o.type == 'CAMERA':
                cam_obj = o
                break
    if cam_obj is None:
        return None

    cam = cam_obj.data
    loc = cam_obj.matrix_world.to_translation()
    angle = b2r_euler_from_matrix(cam_obj.matrix_world)

    # Blender stores horizontal field of view via lens/sensor_width by default.
    # raygen wants vertical FoV in degrees.
    sensor_h = cam.sensor_height if cam.sensor_fit == 'VERTICAL' else \
        cam.sensor_width * (scene.render.resolution_y / max(scene.render.resolution_x, 1))
    fov_v_deg = math.degrees(2.0 * math.atan((sensor_h * 0.5) / cam.lens))

    out = OrderedDict()
    out['location'] = b2r_location(loc)
    out['angle'] = angle
    out['fieldOfView'] = round(fov_v_deg, 3)
    out['exposure'] = round(float(scene.view_settings.exposure) if hasattr(scene.view_settings, 'exposure') else 1.0, 3) or 1.0

    # DoF
    if cam.dof.use_dof:
        out['depthOfField'] = round(float(cam.dof.focus_distance), 4)
        # Blender aperture_fstop maps directly to raygen `aperture`.
        out['aperture'] = round(float(cam.dof.aperture_fstop), 4)
        if cam.dof.aperture_blades >= 3:
            out['apertureBlades'] = int(cam.dof.aperture_blades)
        if cam.dof.aperture_rotation:
            out['apertureRotation'] = round(math.degrees(cam.dof.aperture_rotation), 4)

    return out


# ---------------------------------------------------------------------------
# Envmap
# ---------------------------------------------------------------------------

def envmap_to_dict(scene, out_dir):
    world = scene.world
    if not world or not world.use_nodes or not world.node_tree:
        return None
    out_node = next((n for n in world.node_tree.nodes
                     if n.type == 'OUTPUT_WORLD'), None)
    if not out_node:
        return None
    surf = out_node.inputs.get('Surface')
    if not surf or not surf.is_linked:
        return None

    bg = surf.links[0].from_node
    if bg.type != 'BACKGROUND':
        return None

    intensity = bg.inputs['Strength'].default_value if 'Strength' in bg.inputs else 1.0
    color_in = bg.inputs.get('Color')
    if not color_in or not color_in.is_linked:
        return None

    n = color_in.links[0].from_node
    rotation_deg = 0.0
    # Optional Mapping node before the Environment Texture
    if n.type == 'MAPPING':
        rot_in = n.inputs.get('Rotation')
        if rot_in and not rot_in.is_linked:
            rotation_deg = round(math.degrees(rot_in.default_value[2]), 3)
        # walk one level deeper
        v_in = n.inputs.get('Vector')
        # find the texture
        for inp in (v_in,) if v_in else ():
            pass
        for sub in n.inputs:
            if sub.is_linked and sub.links[0].from_node.type == 'TEX_ENVIRONMENT':
                n = sub.links[0].from_node
                break
        else:
            # search the whole tree as a fallback
            n = next((x for x in world.node_tree.nodes
                      if x.type == 'TEX_ENVIRONMENT'), None)
    if n is None or n.type != 'TEX_ENVIRONMENT' or not n.image:
        return None

    abs_path = bpy.path.abspath(n.image.filepath)
    # Prefer a relative path next to the JSON if the file lives in out_dir,
    # else write the basename and let the user move/copy the HDR alongside.
    try:
        rel = os.path.relpath(abs_path, out_dir)
        # Avoid ugly ../../.. paths — fall back to basename when the texture
        # lives far from the export directory.
        if rel.count('..') > 2:
            tex_path = os.path.basename(abs_path)
        else:
            tex_path = rel.replace('\\', '/')
    except ValueError:
        tex_path = os.path.basename(abs_path)

    out = OrderedDict()
    out['texture'] = tex_path
    out['intensity'] = round(float(intensity), 4)
    if abs(rotation_deg) > 1e-3:
        out['rotation'] = rotation_deg
    return out


# ---------------------------------------------------------------------------
# Lights → emissive primitives
# ---------------------------------------------------------------------------

def light_to_dict(obj):
    light = obj.data
    loc = obj.matrix_world.to_translation()
    out = OrderedDict()
    out['location'] = b2r_location(loc)

    # Blender energy is in Watts. There's no clean conversion to raygen's
    # unit-less `emission` multiplier — we copy `energy` straight through and
    # let the user dial it in. Document this with a comment in the JSON.
    energy = float(light.energy)
    color = [round(light.color[0], 4), round(light.color[1], 4), round(light.color[2], 4)]

    if light.type == 'POINT':
        # Small emissive sphere. Radius from light.shadow_soft_size if > 0.
        r = float(light.shadow_soft_size) if light.shadow_soft_size > 0 else 0.05
        out['scale'] = [round(r, 4)] * 3
        out['mesh'] = {'type': 'sphere'}
        out['mat'] = OrderedDict([
            ('color', color),
            ('emission', round(energy, 4)),
        ])
        return out

    if light.type == 'AREA':
        # Blender area lights emit from a 2D shape along -Z in their local
        # frame. raygen 'plane' is XY-aligned (normal = +Y) by convention in
        # the sample scenes, so we rotate the area frame into that.
        if light.shape == 'SQUARE':
            sx = sy = float(light.size)
        elif light.shape == 'RECTANGLE':
            sx = float(light.size)
            sy = float(light.size_y)
        elif light.shape in {'DISK', 'ELLIPSE'}:
            sx = sy = float(light.size)
        else:
            sx = sy = float(light.size)
        # Rotate Blender's -Z emission direction so it matches the raygen
        # plane (which faces +Y). That's a -90deg around X.
        align = Matrix.Rotation(math.radians(-90.0), 3, 'X')
        out['angle'] = b2r_euler_from_matrix(obj.matrix_world, extra_local=align)
        # raygen `plane` primitive is unit, scaled by [sx, 1, sy].
        out['scale'] = [round(sx * 0.5, 4), 1, round(sy * 0.5, 4)]
        out['mesh'] = {'type': 'plane'}
        out['mat'] = OrderedDict([
            ('color', color),
            ('emission', round(energy, 4)),
        ])
        return out

    if light.type == 'SUN':
        # No directional light analogue in raygen. Skip with a hint comment.
        return {'__skip__': 'SUN light is unsupported (use envmap rotation/intensity instead)'}

    if light.type == 'SPOT':
        r = float(light.shadow_soft_size) if light.shadow_soft_size > 0 else 0.05
        out['scale'] = [round(r, 4)] * 3
        out['mesh'] = {'type': 'sphere'}
        out['mat'] = OrderedDict([
            ('color', color),
            ('emission', round(energy, 4)),
            ('spotRange', round(math.degrees(light.spot_size) * 0.5, 3)),
        ])
        return out

    return {'__skip__': 'unsupported light type: ' + light.type}


# ---------------------------------------------------------------------------
# OBJ export (single bundle)
# ---------------------------------------------------------------------------

def export_single_obj(out_dir, basename):
    obj_path = os.path.join(out_dir, basename + '.obj')
    # Only objects in the active view layer can be selected. Build a set of
    # those object names up front so we can skip data-block-only objects
    # (linked-but-excluded collections, hidden-from-viewlayer, etc.).
    view_layer_objs = {o.name for o in bpy.context.view_layer.objects}
    bpy.ops.object.select_all(action='DESELECT')
    any_mesh = False
    for o in bpy.context.scene.objects:
        if o.type != 'MESH' or o.hide_render:
            continue
        if o.name not in view_layer_objs:
            print('[blend_to_raygen] skipping mesh "%s" (not in active view layer)' % o.name)
            continue
        o.select_set(True)
        any_mesh = True
    if not any_mesh:
        print('[blend_to_raygen] WARN: no visible mesh objects to export')
        return None

    # Blender 4.x: bpy.ops.wm.obj_export. Older API: export_scene.obj.
    #
    # We deliberately leave triangulation OFF — raygen's objreader fan-
    # triangulates n-gons/quads on import using the original per-face-vertex
    # `v/vt/vn` indices, which preserves split normals across quad diagonals.
    # Triangulating at export time can break smooth shading on non-planar
    # quads because the chosen diagonal mixes per-face-vertex normals
    # differently from how the original quad's vertex normals were authored,
    # producing visible triangle-shaped shading seams.
    op = getattr(bpy.ops.wm, 'obj_export', None)
    if op is not None:
        kwargs = dict(
            filepath=obj_path,
            export_selected_objects=True,
            apply_modifiers=True,
            # Bake transforms into vertices so the raygen world entry stays
            # at origin. Forward/Up drive the axis conversion at export time.
            forward_axis='NEGATIVE_Z',
            up_axis='Y',
            export_uv=True,
            export_normals=True,
            export_materials=True,
            export_triangulated_mesh=False,
            path_mode='STRIP',
        )
        # `export_smooth_groups` exists on Blender 4.x+. Add it only if the
        # operator advertises it, to stay forward/backward compatible.
        try:
            if 'export_smooth_groups' in op.get_rna_type().properties.keys():
                kwargs['export_smooth_groups'] = True
        except Exception:
            pass
        op(**kwargs)
    else:
        bpy.ops.export_scene.obj(filepath=obj_path,
                                 use_selection=True,
                                 use_mesh_modifiers=True,
                                 axis_forward='-Z', axis_up='Y',
                                 use_uvs=True, use_normals=True,
                                 use_materials=True,
                                 use_triangles=False,
                                 use_smooth_groups=True,
                                 path_mode='STRIP')
    return basename + '.obj'


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args():
    # Forward only the args after `--` (Blender args before that).
    if '--' in sys.argv:
        argv = sys.argv[sys.argv.index('--') + 1:]
    else:
        argv = []
    p = argparse.ArgumentParser(prog='blend_to_raygen')
    p.add_argument('--out', required=True,
                   help='output scene .json path (companion .obj/.mtl written next to it)')
    return p.parse_args(argv)


def jdumps(obj):
    return json.dumps(obj, indent=2, ensure_ascii=False)


def main():
    args = parse_args()
    out_path = os.path.abspath(args.out)
    out_dir = os.path.dirname(out_path)
    basename = os.path.splitext(os.path.basename(out_path))[0]
    os.makedirs(out_dir, exist_ok=True)

    scene = bpy.context.scene
    view_layer_objs = {o.name for o in bpy.context.view_layer.objects}

    # 1) Export geometry.
    obj_filename = export_single_obj(out_dir, basename)

    # 2) Materials (only those used by an exported mesh).
    used_mats = OrderedDict()
    for o in scene.objects:
        if o.type != 'MESH' or o.hide_render:
            continue
        if o.name not in view_layer_objs:
            continue
        for slot in o.material_slots:
            if slot.material and slot.material.name not in used_mats:
                used_mats[slot.material.name] = slot.material
    materials_block = OrderedDict()
    for name, mat in used_mats.items():
        materials_block[name] = material_to_dict(mat)

    # 3) Lights.
    lights_block = OrderedDict()
    for o in scene.objects:
        if o.type != 'LIGHT' or o.hide_render:
            continue
        if o.name not in view_layer_objs:
            print('[blend_to_raygen] skipping light "%s" (not in active view layer)' % o.name)
            continue
        d = light_to_dict(o)
        if isinstance(d, dict) and '__skip__' in d:
            print('[blend_to_raygen] skipping light "%s": %s' % (o.name, d['__skip__']))
            continue
        lights_block[o.name] = d

    # 4) Camera.
    main_cam = camera_to_dict(scene)

    # 5) Envmap.
    env = envmap_to_dict(scene, out_dir)

    # 6) Compose JSON.
    world = OrderedDict()
    if materials_block:
        world['_materials'] = materials_block
    if obj_filename:
        # Single root entry referencing the bundled .obj. Transforms baked in.
        world[basename] = OrderedDict([
            ('location', [0, 0, 0]),
            ('angle', [0, 0, 0]),
            ('mesh', obj_filename),
        ])
    for name, ld in lights_block.items():
        # Avoid name clashes with the mesh root.
        key = name if name != basename else name + '_light'
        world[key] = ld

    root = OrderedDict()
    if main_cam:
        root['mainCamera'] = main_cam
    if env:
        root['envmap'] = env
    root['world'] = world

    header = (
        "// Generated by tools/blender_export/blend_to_raygen.py from %s.\n"
        "// Baseline scene — emission strengths, glossy/roughness values, and\n"
        "// envmap intensity will almost always need hand tuning. Light energy\n"
        "// values are copied straight from Blender (Watts) and rarely match\n"
        "// raygen's unit-less emission scale 1:1.\n"
    ) % os.path.basename(bpy.data.filepath or '<unsaved>')

    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(header)
        f.write(jdumps(root))
        f.write('\n')

    print('[blend_to_raygen] wrote %s' % out_path)
    if obj_filename:
        print('[blend_to_raygen] wrote %s' % os.path.join(out_dir, obj_filename))


if __name__ == '__main__':
    main()
