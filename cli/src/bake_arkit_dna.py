#!/usr/bin/env python3
"""Bake a MetaHuman head DNA to an ARKit GLB using standalone OpenRigLogic.

Requires NumPy and Epic's matching ``dna`` / ``riglogic`` Python bindings.
See cli/docs/BAKING.md for building the bindings and mapping limits.
"""

import argparse
from dataclasses import dataclass
import hashlib
import importlib
import json
from pathlib import Path
import struct
import sys

import numpy as np

from arkit_controls import ARKIT_GUI_CONTROLS


# Same target order as MHARKitExport, including its separately baked MouthClose.
TARGET_NAMES = (
    "EyeBlinkLeft", "EyeLookDownLeft", "EyeLookInLeft", "EyeLookOutLeft",
    "EyeLookUpLeft", "EyeSquintLeft", "EyeWideLeft",
    "EyeBlinkRight", "EyeLookDownRight", "EyeLookInRight", "EyeLookOutRight",
    "EyeLookUpRight", "EyeSquintRight", "EyeWideRight",
    "JawForward", "JawLeft", "JawRight", "JawOpen", "MouthFunnel", "MouthPucker",
    "MouthLeft", "MouthRight", "MouthSmileLeft", "MouthSmileRight",
    "MouthFrownLeft", "MouthFrownRight", "MouthDimpleLeft", "MouthDimpleRight",
    "MouthStretchLeft", "MouthStretchRight", "MouthRollLower", "MouthRollUpper",
    "MouthShrugLower", "MouthShrugUpper", "MouthPressLeft", "MouthPressRight",
    "MouthLowerDownLeft", "MouthLowerDownRight", "MouthUpperUpLeft",
    "MouthUpperUpRight", "BrowDownLeft", "BrowDownRight", "BrowInnerUp",
    "BrowOuterUpLeft", "BrowOuterUpRight", "CheekPuff", "CheekSquintLeft",
    "CheekSquintRight", "NoseSneerLeft", "NoseSneerRight", "TongueOut", "MouthClose",
)


def default_recipes():
    """Use DNA's named GUI sides directly; UE's frame-label swap is separate."""
    recipes = {}
    for name in TARGET_NAMES:
        pairs = ARKIT_GUI_CONTROLS[name[0].lower() + name[1:]]
        recipes[name] = {"controls": dict(pairs)}
    # An additive mouthClose target must cancel lips at an open jaw, rather
    # than sampling the lipsTogether controls on the closed neutral mouth.
    recipes["MouthClose"]["controls"]["CTRL_C_jaw.ty"] = 1.0
    recipes["MouthClose"]["subtract"] = {"CTRL_C_jaw.ty": 1.0}
    # wrinkleUpper alone is a secondary control and can be inert on its own.
    # Drive the primary nose-wrinkle control for an isolated ARKit sneer.
    for side, label in (("L", "Left"), ("R", "Right")):
        recipes[f"NoseSneer{label}"]["controls"] = {f"CTRL_{side}_nose.ty": 1.0}
    return {"control_space": "gui", "targets": recipes}


def validate_recipes(mapping, control_names):
    if mapping.get("control_space") not in ("gui", "raw"):
        raise ValueError("mapping control_space must be 'gui' or 'raw'")
    if set(mapping.get("targets", {})) != set(TARGET_NAMES):
        raise ValueError("mapping must contain exactly the 52 ARKit target names")
    known = set(control_names)
    for name, recipe in mapping["targets"].items():
        if not isinstance(recipe, dict) or not recipe.get("controls"):
            raise ValueError(f"{name}: a nonempty controls dictionary is required")
        for key in ("controls", "subtract"):
            controls = recipe.get(key, {})
            if not isinstance(controls, dict):
                raise ValueError(f"{name}: {key} must be a dictionary")
            missing = set(controls) - known
            if missing:
                raise ValueError(f"{name}: unknown {key}: {sorted(missing)}")
            if any(not isinstance(v, (int, float)) or not np.isfinite(v)
                   for v in controls.values()):
                raise ValueError(f"{name}: {key} values must be finite numbers")


def quaternion_matrices(quaternions):
    """XYZW unit quaternions to matrices for column-vector transforms."""
    q = np.asarray(quaternions, dtype=np.float64)
    length = np.linalg.norm(q, axis=-1, keepdims=True)
    if np.any(length < 1e-12):
        raise ValueError("RigLogic returned a zero-length joint quaternion")
    x, y, z, w = np.moveaxis(q / length, -1, 0)
    result = np.empty(q.shape[:-1] + (3, 3), dtype=np.float64)
    result[..., 0, 0] = 1 - 2 * (y*y + z*z)
    result[..., 0, 1] = 2 * (x*y - z*w)
    result[..., 0, 2] = 2 * (x*z + y*w)
    result[..., 1, 0] = 2 * (x*y + z*w)
    result[..., 1, 1] = 1 - 2 * (x*x + z*z)
    result[..., 1, 2] = 2 * (y*z - x*w)
    result[..., 2, 0] = 2 * (x*z - y*w)
    result[..., 2, 1] = 2 * (y*z + x*w)
    result[..., 2, 2] = 1 - 2 * (x*x + y*y)
    return result


def joint_world_matrices(neutral, parents, deltas=None):
    """Compose joint deltas as in OpenRigLogic's Advanced.cpp example."""
    neutral = np.asarray(neutral, dtype=np.float64).reshape(-1, 10)
    rotations = quaternion_matrices(neutral[:, 3:7])
    translations = neutral[:, :3].copy()
    scales = neutral[:, 7:10].copy()
    if deltas is not None:
        deltas = np.asarray(deltas, dtype=np.float64).reshape(neutral.shape)
        translations += deltas[:, :3]
        rotations = rotations @ quaternion_matrices(deltas[:, 3:7])
        scales += deltas[:, 7:10]
    local = np.tile(np.eye(4), (len(neutral), 1, 1))
    local[:, :3, :3] = rotations * scales[:, None, :]
    local[:, :3, 3] = translations
    world = np.empty_like(local)
    state = np.zeros(len(neutral), dtype=np.uint8)

    def visit(index):
        if state[index] == 1:
            raise ValueError("Cycle in DNA joint hierarchy")
        if state[index] == 2:
            return
        state[index] = 1
        parent = int(parents[index])
        if parent == index or parent == 65535:
            world[index] = local[index]
        elif 0 <= parent < len(neutral):
            visit(parent)
            world[index] = world[parent] @ local[index]
        else:
            raise ValueError(f"Invalid parent index {parent} for joint {index}")
        state[index] = 2

    for index in range(len(neutral)):
        visit(index)
    return world


def skin_positions(positions, weights, joints, skin_matrices):
    homogeneous = np.column_stack([positions, np.ones(len(positions))])
    return np.einsum("vi,viab,vb->va", weights, skin_matrices[joints],
                     homogeneous, optimize=True)[:, :3]


@dataclass
class Mesh:
    name: str
    positions: np.ndarray
    layout_positions: np.ndarray
    uvs: np.ndarray
    triangles: np.ndarray
    weights: np.ndarray
    joints: np.ndarray
    correctives: list

    def deform(self, skin_matrices, blend_weights):
        positions = self.positions.copy()
        for channel, indices, deltas in self.correctives:
            weight = blend_weights[channel]
            if abs(weight) > 1e-10:
                np.add.at(positions, indices, deltas * weight)
        return skin_positions(positions, self.weights, self.joints, skin_matrices)


def read_mesh(reader, index):
    positions = np.column_stack([
        reader.getVertexPositionXs(index), reader.getVertexPositionYs(index),
        reader.getVertexPositionZs(index),
    ]).astype(np.float64)
    if reader.getSkinWeightsCount(index) != len(positions):
        raise ValueError(f"{reader.getMeshName(index)}: missing skin weights")
    max_influences = reader.getMaximumInfluencePerVertex(index)
    weights = np.zeros((len(positions), max_influences))
    joints = np.zeros(weights.shape, dtype=np.int32)
    for vertex in range(len(positions)):
        values = reader.getSkinWeightsValues(index, vertex)
        indices = reader.getSkinWeightsJointIndices(index, vertex)
        if not values or len(values) != len(indices) or len(values) > max_influences:
            raise ValueError(f"Invalid skin weights for mesh {index}, vertex {vertex}")
        weights[vertex, :len(values)] = values
        joints[vertex, :len(values)] = indices
    sums = weights.sum(axis=1, keepdims=True)
    if not np.all(np.isfinite(weights)) or np.any(weights < 0) or np.any(abs(sums - 1) > 1e-3):
        raise ValueError(f"{reader.getMeshName(index)}: invalid weight sums")
    weights /= sums
    layout_positions = np.array(reader.getVertexLayoutPositionIndices(index), dtype=np.int32)
    uv_indices = np.array(reader.getVertexLayoutTextureCoordinateIndices(index), dtype=np.int32)
    uvs = np.column_stack([
        reader.getVertexTextureCoordinateUs(index), reader.getVertexTextureCoordinateVs(index),
    ])[uv_indices].astype(np.float32)
    # glTF uses the top-left texture origin; DNA uses bottom-left.
    uvs[:, 1] = 1 - uvs[:, 1]
    triangles = []
    for face in range(reader.getFaceCount(index)):
        corners = reader.getFaceVertexLayoutIndices(index, face)
        triangles.extend((corners[0], corners[j], corners[j+1])
                         for j in range(1, len(corners)-1))
    correctives = []
    for target in range(reader.getBlendShapeTargetCount(index)):
        vertices = np.array(reader.getBlendShapeTargetVertexIndices(index, target), dtype=np.int32)
        deltas = np.column_stack([
            reader.getBlendShapeTargetDeltaXs(index, target),
            reader.getBlendShapeTargetDeltaYs(index, target),
            reader.getBlendShapeTargetDeltaZs(index, target),
        ])
        correctives.append((reader.getBlendShapeChannelIndex(index, target), vertices, deltas))
    return Mesh(reader.getMeshName(index), positions, layout_positions, uvs,
                np.array(triangles, dtype=np.uint32), weights, joints, correctives)


class NativeRig:
    def __init__(self, path, dna, riglogic):
        self.riglogic = riglogic
        self.stream = dna.FileStream(str(path), dna.AccessMode_Read, dna.OpenMode_Binary)
        config = dna.Configuration()
        config.minLOD = config.maxLOD = 0
        config.coordinateSystemTransformPolicy = dna.CoordinateSystemTransformPolicy_Transform
        coordinate_system = dna.CoordinateSystem()
        coordinate_system.x = dna.Direction_right
        coordinate_system.y = dna.Direction_up
        coordinate_system.z = dna.Direction_back
        config.coordinateSystem = coordinate_system
        self.reader = dna.BinaryStreamReader(self.stream, config)
        self.reader.read()
        if not dna.Status.isOk():
            raise ValueError(f"Cannot read DNA: {dna.Status.get().message}")
        cfg = riglogic.Configuration()
        cfg.rotationType = riglogic.RotationType_Quaternions
        cfg.floatingPointType = riglogic.FloatingPointType_Float
        self.logic = riglogic.RigLogic(self.reader, cfg)
        self.neutral_joints = np.array(self.logic.getNeutralJointValues()).reshape(-1, 10)
        self.parents = [self.reader.getJointParentIndex(i) for i in range(self.reader.getJointCount())]
        self.bind_inverse = np.linalg.inv(joint_world_matrices(self.neutral_joints, self.parents))
        self.scale = 0.01 if self.reader.getTranslationUnit() == dna.TranslationUnit_cm else 1.0
        self.gui_names = [self.reader.getGUIControlName(i) for i in range(self.reader.getGUIControlCount())]
        self.raw_names = [self.reader.getRawControlName(i) for i in range(self.reader.getRawControlCount())]

    def evaluate(self, meshes, controls, space):
        # A fresh instance prevents controls or solver state leaking between poses.
        instance = self.riglogic.RigInstance(self.logic)
        instance.setLOD(0)
        lookup = {name: i for i, name in enumerate(self.gui_names if space == "gui" else self.raw_names)}
        setter = instance.setGUIControl if space == "gui" else instance.setRawControl
        for name, value in controls.items():
            setter(lookup[name], float(value))
        if space == "gui":
            self.logic.mapGUIToRawControls(instance)
        self.logic.calculate(instance)
        world = joint_world_matrices(self.neutral_joints, self.parents, instance.getJointOutputs())
        skin = world @ self.bind_inverse
        blend_weights = np.array(instance.getBlendShapeOutputs())
        return [mesh.deform(skin, blend_weights) * self.scale for mesh in meshes]


def bake_targets(evaluate, recipes, neutral):
    targets = []
    for name in TARGET_NAMES:
        recipe = recipes[name]
        posed = evaluate(recipe["controls"])
        baseline = evaluate(recipe["subtract"]) if "subtract" in recipe else neutral
        targets.append([p - b for p, b in zip(posed, baseline, strict=True)])
    return targets


def write_glb(path, meshes, neutral, targets, static_meshes=()):
    """Write facial morph meshes and optional neutral body meshes."""
    all_meshes = list(meshes) + [mesh for mesh, _positions in static_meshes]
    all_positions = list(neutral) + [positions for _mesh, positions in static_meshes]
    blob = bytearray()
    document = {
        "asset": {"version": "2.0", "generator": "bake_arkit_dna / OpenRigLogic"},
        "scene": 0, "scenes": [{"nodes": list(range(len(all_meshes)))}],
        "nodes": [], "meshes": [], "accessors": [], "bufferViews": [],
    }

    def add(array, kind, indices=False):
        array = np.asarray(array, dtype="<u4" if indices else "<f4")
        if not np.all(np.isfinite(array)):
            raise ValueError("Non-finite values in baked geometry")
        view = len(document["bufferViews"])
        document["bufferViews"].append({
            "buffer": 0, "byteOffset": len(blob), "byteLength": array.nbytes,
            "target": 34963 if indices else 34962,
        })
        blob.extend(array.tobytes())
        blob.extend(b"\0" * (-len(blob) % 4))
        accessor = {"bufferView": view, "componentType": 5125 if indices else 5126,
                    "count": len(array), "type": kind}
        accessor["min"] = np.atleast_1d(array.min(axis=0)).tolist()
        accessor["max"] = np.atleast_1d(array.max(axis=0)).tolist()
        index = len(document["accessors"])
        document["accessors"].append(accessor)
        return index

    for i, mesh in enumerate(all_meshes):
        layout = mesh.layout_positions
        primitive = {
            "attributes": {"POSITION": add(all_positions[i][layout], "VEC3"),
                           "TEXCOORD_0": add(mesh.uvs, "VEC2")},
            "indices": add(mesh.triangles.reshape(-1), "SCALAR", indices=True),
        }
        mesh_entry = {"name": mesh.name, "primitives": [primitive]}
        if i < len(meshes):
            primitive["targets"] = [{"POSITION": add(pose[i][layout], "VEC3")} for pose in targets]
            mesh_entry["weights"] = [0.0] * len(TARGET_NAMES)
            mesh_entry["extras"] = {"targetNames": list(TARGET_NAMES)}
        document["nodes"].append({"mesh": i, "name": mesh.name})
        document["meshes"].append(mesh_entry)
    document["buffers"] = [{"byteLength": len(blob)}]
    encoded = json.dumps(document, separators=(",", ":"), allow_nan=False).encode()
    encoded += b" " * (-len(encoded) % 4)
    total = 12 + 8 + len(encoded) + 8 + len(blob)
    path.write_bytes(struct.pack("<4sII", b"glTF", 2, total)
                     + struct.pack("<I4s", len(encoded), b"JSON") + encoded
                     + struct.pack("<I4s", len(blob), b"BIN\0") + blob)


def main(argv=None, *, standalone=False):
    description = ("Bake 52 ARKit facial shapes from head DNA into a GLB. "
                   "Optionally include the matching body in its neutral pose.") if standalone else __doc__
    parser = argparse.ArgumentParser(description=description,
                                     epilog="Diagnostics: --version, --build-info, --self-test" if standalone else None)
    parser.add_argument("dna", type=Path, help="Complete head DNA file")
    parser.add_argument("--body-dna", type=Path, help="Include LOD0 body geometry in its neutral pose")
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output .glb")
    if standalone:
        parser.set_defaults(bindings=None)
    else:
        parser.add_argument("--bindings", type=Path, help="Directory containing dna.py and riglogic.py")
    parser.add_argument("--mapping", type=Path, help="JSON GUI/raw recipes overriding the built-in mapping")
    parser.add_argument("--mesh", action="append", help="Export only this LOD0 mesh name (repeatable)")
    args = parser.parse_args(argv)
    if args.output.suffix.lower() != ".glb":
        parser.error("output must have a .glb extension")
    if args.bindings:
        sys.path.insert(0, str(args.bindings.resolve()))
    try:
        dna = importlib.import_module("dna")
        riglogic = importlib.import_module("riglogic")
    except ImportError as exc:
        parser.error(f"OpenRigLogic bindings could not load: {exc}. Use --bindings or PYTHONPATH.")
    rig = NativeRig(args.dna, dna, riglogic)
    reader = rig.reader
    mapping = json.loads(args.mapping.read_text()) if args.mapping else default_recipes()
    space = mapping["control_space"]
    validate_recipes(mapping, rig.gui_names if space == "gui" else rig.raw_names)
    indices = list(reader.getMeshIndicesForLOD(0))
    if args.mesh:
        missing = set(args.mesh) - {reader.getMeshName(i) for i in indices}
        if missing:
            parser.error(f"Unknown LOD0 mesh names: {sorted(missing)}")
        indices = [i for i in indices if reader.getMeshName(i) in args.mesh]
    meshes = [read_mesh(reader, i) for i in indices]
    if not meshes:
        raise ValueError("DNA has no LOD0 meshes to bake")
    for mesh in meshes:
        print(f"{mesh.name}: {len(mesh.positions)} positions, {len(mesh.triangles)} triangles, "
              f"{len(mesh.correctives)} DNA corrective targets", flush=True)
    evaluate = lambda controls: rig.evaluate(meshes, controls, space)
    neutral = evaluate({})
    targets = bake_targets(evaluate, mapping["targets"], neutral)
    statistics = {}
    for name, pose in zip(TARGET_NAMES, targets, strict=True):
        maxima = {mesh.name: float(np.linalg.norm(delta, axis=1).max())
                  for mesh, delta in zip(meshes, pose, strict=True)}
        statistics[name] = {"max_displacement_m": max(maxima.values()), "meshes": maxima}
        print(f"{name}: {max(maxima.values()) * 1000:.3f} mm", flush=True)
    if sum(row["max_displacement_m"] > 1e-6 for row in statistics.values()) < 3:
        raise ValueError("Fewer than three nonzero targets; refusing to write an invalid bake")
    static_meshes = []
    body_report = None
    if args.body_dna:
        body_rig = NativeRig(args.body_dna, dna, riglogic)
        body_meshes = [read_mesh(body_rig.reader, i)
                       for i in body_rig.reader.getMeshIndicesForLOD(0)]
        if not body_meshes:
            raise ValueError("Body DNA has no LOD0 meshes")
        body_neutral = body_rig.evaluate(body_meshes, {}, "raw")
        static_meshes = list(zip(body_meshes, body_neutral, strict=True))
        body_bytes = args.body_dna.read_bytes()
        body_report = {
            "source_dna": str(args.body_dna.resolve()),
            "source_sha256": hashlib.sha256(body_bytes).hexdigest(),
            "dna_version": list(struct.unpack_from("<HH", body_bytes, 4)),
            "joint_count": body_rig.reader.getJointCount(),
            "pose": "evaluated neutral",
            "skeletal_skin_exported": False,
        }
        for mesh in body_meshes:
            print(f"{mesh.name}: {len(mesh.positions)} positions, "
                  f"{len(mesh.triangles)} triangles (neutral body)", flush=True)
    source_bytes = args.dna.read_bytes()
    report = {
        "source_dna": str(args.dna.resolve()),
        "source_sha256": hashlib.sha256(source_bytes).hexdigest(),
        "openriglogic_version": riglogic.VersionInfo.getVersionString(),
        # The native reader upgrades old formats in memory, so its version
        # getters identify the adapted representation, not the source file.
        "dna_version": list(struct.unpack_from("<HH", source_bytes, 4)),
        "reader_dna_version": [reader.getFileFormatGeneration(), reader.getFileFormatVersion()],
        "joint_count": reader.getJointCount(), "gui_control_count": len(rig.gui_names),
        "raw_control_count": len(rig.raw_names), "blend_shape_channel_count": reader.getBlendShapeChannelCount(),
        "coordinate_system": "right / up / back; metres; CCW",
        "target_names": list(TARGET_NAMES), "mapping": mapping,
        "mapping_source": str(args.mapping.resolve()) if args.mapping else "repository GUI control recipes",
        "unreal_plugin_parity_verified": False,
        "zero_targets": [name for name, row in statistics.items() if row["max_displacement_m"] <= 1e-6],
        "meshes": [{"name": m.name, "positions": len(m.positions),
                    "glb_vertices": len(m.layout_positions), "triangles": len(m.triangles),
                    "dna_corrective_targets": len(m.correctives)} for m in meshes],
        "targets": statistics,
    }
    if body_report:
        report["body"] = body_report
        report["meshes"].extend({
            "name": mesh.name, "component": "body", "positions": len(mesh.positions),
            "glb_vertices": len(mesh.layout_positions), "triangles": len(mesh.triangles),
            "dna_corrective_targets": len(mesh.correctives), "morph_target_count": 0,
        } for mesh, _positions in static_meshes)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_glb(args.output, meshes, neutral, targets, static_meshes)
    report_path = args.output.with_suffix(".report.json")
    report_path.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(f"Wrote {args.output} ({args.output.stat().st_size / 1024**2:.1f} MiB)")
    print(f"Report: {report_path}")
    if report["zero_targets"]:
        print(f"Zero targets on selected meshes: {report['zero_targets']}")


if __name__ == "__main__":
    main()
