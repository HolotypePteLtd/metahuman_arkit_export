#!/usr/bin/env python3
"""Per-vertex error heatmaps: CLI bake vs UE ground truth.

For each pose pair: evaluate both heads with the pose's shape key at 1.0,
compute per-vertex Euclidean distance (mm), paint it as a 'err' color
attribute on a single head per pose, blue (0 mm) -> red (>= scale).
Grid + legend text + per-pose stats; renders grid and eye/jaw closeups.

Run inside Blender (or via MCP exec with sys.argv set):
    blender --background --factory-startup --python build_heatmap_blend.py -- \
        --cli out/celeste_epic.glb --ue <path>/MH_Celeste_0015_arkit.glb \
        --out out/heatmap_celeste_cli_vs_ue.blend --renders out
"""
import sys
import argparse
import json
import math

import bpy
import mathutils

# Same curated subset as the side-by-side build.
DEFAULT_POSES = [
    "JawOpen", "MouthFunnel", "MouthPucker", "MouthSmileLeft", "MouthFrownRight",
    "CheekPuff", "BrowInnerUp", "BrowOuterUpRight", "EyeBlinkLeft", "EyeBlinkRight",
    "EyeLookDownLeft", "EyeLookUpRight", "EyeSquintLeft", "MouthShrugUpper",
    "TongueOut", "MouthLeft", "JawLeft",
]


def parse_args():
    argv = sys.argv
    argv = argv[argv.index("--") + 1:] if "--" in argv else []
    p = argparse.ArgumentParser()
    p.add_argument("--cli", required=True)
    p.add_argument("--ue", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--renders", default="", help="dir for PNG renders (optional)")
    p.add_argument("--poses", default="")
    p.add_argument("--spacing", type=float, default=0.34)
    p.add_argument("--gap", type=float, default=0.0)
    p.add_argument("--scale-mm", type=float, default=6.0, help="ramp max in mm")
    p.add_argument("--cols", type=int, default=4)
    return p.parse_args(argv)


def import_head(path, prefix):
    """Import GLB, keep only the head mesh, return (object, pose_names)."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    imported = [o for o in bpy.data.objects if o not in before]
    heads = [o for o in imported if o.type == "MESH" and "head" in o.name.lower()]
    keep = heads[0] if heads else next(o for o in imported if o.type == "MESH")
    keep.name = prefix + "_head"
    for ob in imported:
        if ob is not keep:
            for c in list(ob.users_collection):
                c.objects.unlink(ob)
            bpy.data.objects.remove(ob, do_unlink=True)
    sk = keep.data.shape_keys
    if sk is None:
        raise RuntimeError(f"{prefix}: head mesh has no shape keys")
    return keep, [k.name for k in sk.key_blocks[1:]]


def evaluated_world_verts(ob):
    deps = bpy.context.evaluated_depsgraph_get()
    ev = ob.evaluated_get(deps)
    me = ev.to_mesh()
    mw = ob.matrix_world
    verts = [(mw @ v.co) for v in me.vertices]
    ev.to_mesh_clear()
    return verts


def make_heat_material(name, scale_mm):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    bsdf.inputs["Roughness"].default_value = 0.55

    attr = nt.nodes.new("ShaderNodeAttribute")
    attr.attribute_name = "err"

    mapr = nt.nodes.new("ShaderNodeMapRange")
    mapr.inputs["From Min"].default_value = 0.0
    mapr.inputs["From Max"].default_value = scale_mm / 1000.0  # metres
    mapr.clamp = True

    ramp = nt.nodes.new("ShaderNodeValToRGB")
    stops = [
        (0.00, (0.03, 0.03, 0.45, 1.0)),   # ~0 mm dark blue
        (0.25, (0.00, 0.45, 0.85, 1.0)),   # blue
        (0.50, (0.00, 0.75, 0.30, 1.0)),   # green
        (0.75, (1.00, 0.90, 0.00, 1.0)),   # yellow
        (1.00, (0.85, 0.05, 0.05, 1.0)),   # red
    ]
    for pos, col in stops:
        e = ramp.color_ramp.elements.new(pos) if pos > 0 else ramp.color_ramp.elements[0]
        e.color = col

    nt.links.new(attr.outputs["Color"], mapr.inputs["Value"])
    nt.links.new(mapr.outputs["Result"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Emission Strength"].default_value = 0.25
    nt.links.new(ramp.outputs["Color"], bsdf.inputs["Emission Color"])
    return mat


def make_text(name, body, size=0.018, color=(0.05, 0.05, 0.05, 1.0)):
    cu = bpy.data.curves.new(name, type="FONT")
    cu.body = body
    cu.size = size
    cu.align_x = "CENTER"
    ob = bpy.data.objects.new(name, cu)
    ob.rotation_euler = (math.pi / 2, 0, 0)  # upright, facing a -Y camera
    mat = bpy.data.materials.new(name + "_mat")
    mat.use_nodes = True
    b = mat.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = color
    ob.data.materials.append(mat)
    return ob


def build_kdtree(verts):
    kd = mathutils.kdtree.KDTree(len(verts))
    for i, v in enumerate(verts):
        kd.insert(v, i)
    kd.balance()
    return kd


def main():
    args = parse_args()

    bpy.ops.wm.read_homefile(use_empty=True, use_factory_startup=True)
    scene = bpy.context.scene

    ue_obj, ue_poses = import_head(args.ue, "UE")
    cli_obj, cli_poses = import_head(args.cli, "CLI")

    names = [n for n in DEFAULT_POSES if n in ue_poses and n in cli_poses]
    if args.poses:
        wanted = [s.strip() for s in args.poses.split(",") if s.strip()]
        names = [n for n in wanted if n in ue_poses and n in cli_poses]
        missing = [n for n in wanted if n not in ue_poses or n not in cli_poses]
        if missing:
            print(f"WARNING: poses missing on one side: {missing}")
    if not names:
        raise RuntimeError("no common poses found")

    print(f"verts per head: {len(cli_obj.data.vertices)} (CLI) / {len(ue_obj.data.vertices)} (UE)")
    heat_mat = make_heat_material("Heat", args.scale_mm)

    cols = args.cols
    spacing = args.spacing
    stats = {}
    err_attr_name = "err"

    for i, pose in enumerate(names):
        # Two throwaway copies to evaluate both sides with the pose at 1.0.
        dups = {}
        for src, label in ((ue_obj, "UE"), (cli_obj, "CLI")):
            d = src.copy()
            d.data = src.data.copy()
            for c in src.users_collection:
                c.objects.link(d)
            d.name = f"tmp_{label}_{pose}"
            d.data.shape_keys.key_blocks[pose].value = 1.0
            dups[label] = d
        bpy.context.view_layer.update()

        ve = evaluated_world_verts(dups["UE"])
        vc = evaluated_world_verts(dups["CLI"])
        # Different topologies (CLI 24k vs UE 34k verts): directed nearest-
        # neighbour distance CLI -> UE, same metric as the chamfer diff.
        kd_ue = build_kdtree(ve)
        dvals = []
        sumsq = 0.0
        vmax = 0.0
        for v in vc:
            dv = kd_ue.find(v)[2] * 1000.0  # mm
            dvals.append(dv)
            sumsq += dv * dv
            if dv > vmax:
                vmax = dv
        rms = (sumsq / len(vc)) ** 0.5
        stats[pose] = {"max_mm": round(vmax, 3), "rms_mm": round(rms, 3), "verts": len(vc)}

        # Heatmap carrier: reuse the CLI duplicate.
        carrier = dups["CLI"]
        carrier.name = f"ERR_{pose}"
        me = carrier.data
        ca = me.color_attributes.get(err_attr_name)
        if ca is None:
            ca = me.color_attributes.new(err_attr_name, "FLOAT_COLOR", "POINT")
        for a, dv in enumerate(dvals):
            g = dv / args.scale_mm
            ca.data[a].color = (g, g, g, 1.0)
        me.materials.clear()
        me.materials.append(heat_mat)

        # Throw away the UE copy.
        d = dups["UE"]
        for c in list(d.users_collection):
            c.objects.unlink(d)
        bpy.data.objects.remove(d, do_unlink=True)

        col, row = i % cols, i // cols
        carrier.location = (col * spacing, -row * spacing, 0)
        # GLB vertices carry the node offset: recentre each head on its cell.
        mn = mathutils.Vector((1e9,) * 3)
        mx = mathutils.Vector((-1e9,) * 3)
        for v in me.vertices:
            for k in range(3):
                mn[k] = min(mn[k], v.co[k])
                mx[k] = max(mx[k], v.co[k])
        carrier.location -= (mn + mx) / 2

        label = make_text(f"L_{pose}", f"{pose}  max {vmax:.1f}  rms {rms:.1f} mm")
        scene.collection.objects.link(label)
        label.location = (col * spacing, -row * spacing - spacing * 0.38, -0.24)
        print(f"  {pose}: max {vmax:.2f} mm, rms {rms:.2f} mm")

    # Cleanup templates.
    for ob in (ue_obj, cli_obj):
        for c in list(ob.users_collection):
            c.objects.unlink(ob)
        bpy.data.objects.remove(ob, do_unlink=True)

    # Grid extents.
    ny = (len(names) + cols - 1) // cols
    cx = (min(cols, len(names)) - 1) * spacing / 2
    cy = -(ny - 1) * spacing / 2

    # Legend + title.
    legend = make_text("L_legend", f"per-vertex |CLI-UE| distance   0 -> {args.scale_mm:.0f} mm   (blue green yellow red)", size=0.03)
    scene.collection.objects.link(legend)
    legend.location = (cx, spacing * 0.42, 0)

    # Grey world + sun lights (points were tuned for wrong distances).
    world = bpy.data.worlds.get("World") or bpy.data.worlds.new("World")
    scene.world = world
    world.use_nodes = True
    wbg = world.node_tree.nodes.get("Background") or world.node_tree.nodes.new("ShaderNodeBackground")
    wbg.inputs["Color"].default_value = (0.85, 0.85, 0.85, 1.0)
    wbg.inputs["Strength"].default_value = 1.0
    for o in list(bpy.data.objects):
        if o.type == "LIGHT":
            bpy.data.objects.remove(o, do_unlink=True)
    for name, loc, rot, energy in (
        ("key", (1.5, -1.5, 2.0), (math.radians(50), 0, math.radians(30)), 2.5),
        ("fill", (-2.0, -1.0, 1.5), (math.radians(55), 0, math.radians(-35)), 1.2),
        ("rim", (0.0, 1.5, 1.8), (math.radians(120), 0, math.radians(180)), 1.5),
    ):
        ld = bpy.data.lights.new(name, "SUN")
        ld.energy = energy
        lamp = bpy.data.objects.new(name, ld)
        scene.collection.objects.link(lamp)
        lamp.location = loc
        lamp.rotation_euler = rot

    cam_data = bpy.data.cameras.new("HeatCam")
    cam = bpy.data.objects.new("HeatCam", cam_data)
    scene.collection.objects.link(cam)
    cam_data.lens = 55
    scene.camera = cam

    def aim(loc, target):
        cam.location = loc
        d = mathutils.Vector(target) - mathutils.Vector(loc)
        cam.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()

    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 1920
    scene.render.resolution_y = 1080

    # Grid render: from the FRONT (-Y side), slightly above.
    aim((cx, cy - 1.35, 0.15), (cx, cy, 0.0))
    bpy.ops.wm.save_as_mainfile(filepath=args.out)
    print(f"saved {args.out}")

    if args.renders:
        import os
        os.makedirs(args.renders, exist_ok=True)
        grid_png = os.path.join(args.renders, "heatmap_grid.png")
        scene.render.filepath = grid_png
        bpy.ops.render.render(write_still=True)
        print(f"rendered {grid_png}")

        # Closeup: left-eye region of the EyeBlinkLeft cell (facing camera).
        i = names.index("EyeBlinkLeft")
        col, row = i % cols, i // cols
        tgt = (col * spacing, -row * spacing, -0.015)
        aim((tgt[0], tgt[1] - 0.30, 0.02), tgt)
        cam_data.lens = 130
        eye_png = os.path.join(args.renders, "heatmap_eye_EyeBlinkLeft.png")
        scene.render.filepath = eye_png
        bpy.ops.render.render(write_still=True)
        print(f"rendered {eye_png}")

        # Closeup: jaw of the JawOpen cell (row 0, col 0).
        tgt = (0.0, 0.0, -0.075)
        aim((0.0, -0.30, -0.03), tgt)
        cam_data.lens = 130
        jaw_png = os.path.join(args.renders, "heatmap_jaw_JawOpen.png")
        scene.render.filepath = jaw_png
        bpy.ops.render.render(write_still=True)
        print(f"rendered {jaw_png}")

    return {"saved": args.out, "stats": stats, "poses": names,
            "scale_mm": args.scale_mm, "verts": stats[names[0]]["verts"]}


if __name__ == "__main__":
    main()
