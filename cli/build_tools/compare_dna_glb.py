#!/usr/bin/env python3
"""Compare a baked DNA's RigLogic output against a matching baked GLB."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from bake_arkit_dna import NativeRig, TARGET_NAMES, read_mesh
from baked_dna import control_names


def compare(dna_path, glb_path):
    import dna
    import riglogic

    rig = NativeRig(dna_path, dna, riglogic)
    meshes = [read_mesh(rig.reader, i) for i in rig.reader.getMeshIndicesForLOD(0)]
    raw = glb_path.read_bytes()
    if struct.unpack_from("<4sII", raw) != (b"glTF", 2, len(raw)):
        raise ValueError("Invalid GLB header")
    length = struct.unpack_from("<I", raw, 12)[0]
    document = json.loads(raw[20:20+length])
    binary = raw[28+length:]

    def positions(index):
        accessor = document["accessors"][index]
        if accessor["componentType"] != 5126 or accessor["type"] != "VEC3":
            raise ValueError("Expected float32 VEC3 positions")
        view = document["bufferViews"][accessor["bufferView"]]
        offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        return np.frombuffer(binary, dtype="<f4", count=accessor["count"] * 3,
                             offset=offset).reshape(-1, 3).astype(np.float64)

    if len(meshes) != len(document["meshes"]):
        raise ValueError("Mesh counts differ (compare head-only GLB and DNA)")
    neutral, targets = [], [[] for _ in TARGET_NAMES]
    for mesh, entry in zip(meshes, document["meshes"], strict=True):
        if entry["name"] != mesh.name or entry["extras"]["targetNames"] != list(TARGET_NAMES):
            raise ValueError("GLB mesh names or ARKit target order differ")
        primitive = entry["primitives"][0]
        neutral.append(positions(primitive["attributes"]["POSITION"]))
        for deltas, target in zip(targets, primitive["targets"], strict=True):
            deltas.append(positions(target["POSITION"]))
    if rig.raw_names != control_names(TARGET_NAMES):
        raise ValueError("DNA raw controls do not match the ARKit contract")
    rng = np.random.default_rng(0)
    samples = [np.zeros(52), *np.eye(52), *rng.random((3, 52))]
    maximum = 0.0
    for weights in samples:
        controls = {name: float(weight) for name, weight in zip(rig.raw_names, weights, strict=True)}
        evaluated = rig.evaluate(meshes, controls, "raw")
        for index, (mesh, actual) in enumerate(zip(meshes, evaluated, strict=True)):
            expected = neutral[index].copy()
            for weight, target in zip(weights, targets, strict=True):
                expected += weight * target[index]
            actual = actual[mesh.layout_positions]
            if (actual.shape != expected.shape or not np.all(np.isfinite(actual))
                    or not np.all(np.isfinite(expected))):
                raise ValueError("DNA produced invalid vertex positions")
            error = float(np.linalg.norm(actual - expected, axis=1).max())
            maximum = max(maximum, error)
    return {"verified": maximum <= 2e-6, "meshes": len(meshes), "targets": 52,
            "poses": len(samples), "max_error_m": maximum, "tolerance_m": 2e-6,
            "dna_sha256": hashlib.sha256(dna_path.read_bytes()).hexdigest(),
            "glb_sha256": hashlib.sha256(raw).hexdigest()}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dna", type=Path)
    parser.add_argument("glb", type=Path)
    args = parser.parse_args()
    result = compare(args.dna, args.glb)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["verified"] else 1)
