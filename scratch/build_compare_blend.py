#!/usr/bin/env python3
"""Build a side-by-side comparison .blend of the CLI bake vs the UE ground truth.

Both GLBs contain the same ARKit-52 morph target set on the head mesh
(UE adds 14 corrective targets). For each pose we create two objects side by
side -- UE (ground truth) on the left, CLI (bake-arkit-dna) on the right --
with a single morph target set to 1.0, laid out in a grid.

Run inside Blender:
    blender --background --factory-startup --python build_compare_blend.py -- \
        --cli out/celeste_epic.glb --ue <path>/MH_Celeste_0015_arkit.glb \
        --out out/compare_celeste_cli_vs_ue.blend [--poses JawOpen,EyeBlinkLeft]
"""
import sys
import argparse

import bpy

# Curated subset when --poses is not given: the interesting/valid rows from
# the chamfer diff, spanning good matches and the known eye-region residual.
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
    p.add_argument("--cli", required=True, help="GLB baked by bake-arkit-dna")
    p.add_argument("--ue", required=True, help="UE plugin ground-truth GLB")
    p.add_argument("--out", required=True, help="output .blend path")
    p.add_argument("--poses", default="", help="comma-separated subset; default curated 17")
    p.add_argument("--spacing", type=float, default=0.32, help="metres between poses")
    p.add_argument("--gap", type=float, default=0.06, help="metres between UE and CLI copy")
    p.add_argument("--max-poses", type=int, default=17)
    args = p.parse_args(argv)
    return args


def import_glb_scene(path, prefix, mirror=False):
    """Import a GLB and return {pose_name: object} for the head mesh only.

    Other meshes (teeth, eyes, shells) are deleted after pinning morphs --
    the head carries the evaluation surface.
    """
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    imported = [o for o in bpy.data.objects if o not in before]

    heads = [o for o in imported if o.type == "MESH" and "head" in o.name.lower()]
    keep = heads[0] if heads else next(o for o in imported if o.type == "MESH")
    keep.name = prefix + "_head"

    others = [o for o in imported if o is not keep]
    for ob in others:
        for c in list(ob.users_collection):
            c.objects.unlink(ob)
        bpy.data.objects.remove(ob, do_unlink=True)

    # GLB morph targets import as shape keys automatically.
    poses = {}
    sk = keep.data.shape_keys
    if sk is None:
        # Import creates shape keys from morph targets automatically.
        raise RuntimeError(f"{prefix}: head mesh has no shape keys")
    for k in sk.key_blocks[1:]:
        poses[k.name] = k.name
    return keep, poses


def main():
    args = parse_args()

    bpy.ops.wm.read_homefile(use_empty=True, use_factory_startup=True)

    ue_obj, ue_poses = import_glb_scene(args.ue, "UE")
    cli_obj, cli_poses = import_glb_scene(args.cli, "CLI")

    names = [n for n in DEFAULT_POSES if n in ue_poses and n in cli_poses]
    if args.poses:
        wanted = [s.strip() for s in args.poses.split(",") if s.strip()]
        names = [n for n in wanted if n in ue_poses and n in cli_poses]
        missing = [n for n in wanted if n not in ue_poses or n not in cli_poses]
        if missing:
            print(f"WARNING: poses missing on one side: {missing}")
    names = names[: args.max_poses]
    if not names:
        raise RuntimeError("no common poses found")

    print(f"UE head: {len(ue_poses)} shape keys; CLI head: {len(cli_poses)} shape keys")
    print(f"building {len(names)} pose pairs: {names}")

    # Duplicate the head pair per pose; set the pose's shape key value to 1.
    cols = 4
    spacing, gap = args.spacing, args.gap
    for i, pose in enumerate(names):
        for src_obj, xoff, label in ((ue_obj, -gap / 2 - 0.0, "UE"), (cli_obj, gap / 2, "CLI")):
            dup = src_obj.copy()
            dup.data = src_obj.data.copy()
            for c in src_obj.users_collection:
                c.objects.link(dup)
            dup.name = f"{label}_{pose}"
            kb = dup.data.shape_keys.key_blocks[pose]
            kb.value = 1.0
            # mute nothing; all others stay 0
            col, row = i % cols, i // cols
            dup.location = (
                xoff + col * spacing,
                -row * spacing,
                0,
            )
            dup.rotation_mode = "XYZ"
        print(f"  pose {i+1}/{len(names)}: {pose}")

    # Remove the template objects (all keys zero) after duplication.
    for ob in (ue_obj, cli_obj):
        for c in list(ob.users_collection):
            c.objects.unlink(ob)
        bpy.data.objects.remove(ob, do_unlink=True)

    # Materials: make UE semi-transparent red, CLI blue, so overlaps read.
    def tint(prefix, rgba):
        mat = bpy.data.materials.new(prefix + "_tint")
        mat.use_nodes = True
        bsdf = mat.node_tree.nodes["Principled BSDF"]
        bsdf.inputs["Base Color"].default_value = rgba
        bsdf.inputs["Roughness"].default_value = 0.6
        return mat

    ue_mat = tint("UE", (0.8, 0.15, 0.1, 1.0))
    cli_mat = tint("CLI", (0.1, 0.35, 0.9, 1.0))
    for m in (ue_mat, cli_mat):
        try:
            m.surface_render_method = "BLENDED"  # Blender 4.2+ / 5.x
        except AttributeError:
            m.blend_method = "BLEND"  # legacy EEVEE
    for ob in bpy.data.objects:
        if ob.name.startswith("UE_"):
            ob.data.materials.clear()
            ob.data.materials.append(ue_mat)
        elif ob.name.startswith("CLI_"):
            ob.data.materials.clear()
            ob.data.materials.append(cli_mat)

    # Lights + camera: simple three-point lighting rig aimed at grid centre.
    scene = bpy.context.scene

    def add_light(name, loc, energy):
        ld = bpy.data.lights.new(name, "POINT")
        ld.energy = 300
        lamp = bpy.data.objects.new(name, ld)
        scene.collection.objects.link(lamp)
        lamp.location = loc
        return lamp

    nx = min(cols, len(names))
    ny = (len(names) + cols - 1) // cols
    cx = (nx - 1) * spacing / 2
    cy = -(ny - 1) * spacing / 2
    add_light("key", (cx + 2, cy + 2, 3), 1200)
    add_light("fill", (cx - 3, cy + 2, 2), 400)
    add_light("rim", (cx, cy - 3, 2.5), 500)

    cam_data = bpy.data.cameras.new("CompareCam")
    cam = bpy.data.objects.new("CompareCam", cam_data)
    scene.collection.objects.link(cam)
    cam.location = (cx, cy + 2.2, 0.6)
    # look at grid centre
    import mathutils
    direction = mathutils.Vector((cx, cy, 0.02)) - cam.location
    rot_quat = direction.to_track_quat("-Z", "Y")
    cam.rotation_euler = rot_quat.to_euler()
    cam_data.lens = 60
    scene.camera = cam

    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 1920
    scene.render.resolution_y = 1080

    bpy.ops.wm.save_as_mainfile(filepath=args.out)
    print(f"saved {args.out}")
    result = {"saved": args.out, "pose_pairs": len(names), "poses": names}
    return result


if __name__ == "__main__":
    main()
