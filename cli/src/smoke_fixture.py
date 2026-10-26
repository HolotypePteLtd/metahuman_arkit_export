"""Generate synthetic DNA for runtime verification; contains no character assets."""
import contextlib
import io
import json
from pathlib import Path
import struct
import tempfile


def write_fixture(path, *, body=False):
    import dna

    stream = dna.FileStream(str(path), dna.AccessMode_Write, dna.OpenMode_Binary)
    writer = dna.BinaryStreamWriter(stream)
    writer.setName("Synthetic ARKit release fixture")
    writer.setLODCount(1)
    writer.setDBMaxLOD(0)
    writer.setTranslationUnit(dna.TranslationUnit_cm)
    writer.setRotationUnit(dna.RotationUnit_degrees)
    axes = dna.CoordinateSystem()
    axes.x, axes.y, axes.z = dna.Direction_left, dna.Direction_up, dna.Direction_front
    writer.setCoordinateSystem(axes)
    writer.setGUIControlName(0, "CTRL_test.ty")
    writer.setRawControlName(0, "CTRL_expressions.test")
    writer.setGUIToRawInputIndices([0])
    writer.setGUIToRawOutputIndices([0])
    writer.setGUIToRawFromValues([0.0])
    writer.setGUIToRawToValues([1.0])
    writer.setGUIToRawSlopeValues([1.0])
    writer.setGUIToRawCutValues([0.0])
    writer.setPSDCount(0)
    writer.setJointName(0, "root")
    writer.setJointHierarchy([0])
    writer.setJointIndices(0, [0])
    writer.setLODJointMapping(0, 0)
    writer.setNeutralJointTranslations([[0.0, 0.0, 0.0]])
    writer.setNeutralJointRotations([[0.0, 0.0, 0.0]])
    writer.setJointRowCount(9)
    writer.setJointColumnCount(1)
    writer.setJointGroupLODs(0, [1])
    writer.setJointGroupInputIndices(0, [0])
    writer.setJointGroupOutputIndices(0, [0])
    writer.setJointGroupValues(0, [1.0])
    writer.setJointGroupJointIndices(0, [0])
    writer.setBlendShapeChannelName(0, "test_corrective")
    writer.setBlendShapeChannelIndices(0, [0])
    writer.setLODBlendShapeChannelMapping(0, 0)
    writer.setBlendShapeChannelLODs([1])
    writer.setBlendShapeChannelInputIndices([0])
    writer.setBlendShapeChannelOutputIndices([0])
    writer.setMeshName(0, "body_lod0_mesh" if body else "head_lod0_mesh")
    writer.setMeshIndices(0, [0])
    writer.setLODMeshMapping(0, 0)
    writer.setMeshBlendShapeChannelMapping(0, 0, 0)
    height = 0.0 if body else 150.0
    writer.setVertexPositions(0, [[0., height, 0.], [10., height, 0.], [0., height+10., 0.]])
    writer.setVertexTextureCoordinates(0, [[0., 0.], [1., 0.], [0., 1.]])
    writer.setVertexNormals(0, [[0., 0., 1.]] * 3)
    writer.setVertexLayouts(0, [[0, 0, 0], [1, 1, 1], [2, 2, 2]])
    writer.setFaceVertexLayoutIndices(0, 0, [0, 1, 2])
    writer.setMaximumInfluencePerVertex(0, 1)
    for vertex in range(3):
        writer.setSkinWeightsValues(0, vertex, [1.0])
        writer.setSkinWeightsJointIndices(0, vertex, [0])
    writer.setBlendShapeChannelIndex(0, 0, 0)
    writer.setBlendShapeTargetVertexIndices(0, 0, [1])
    writer.setBlendShapeTargetDeltas(0, 0, [[0., 0., 1.]])
    writer.write()
    if not dna.Status.isOk():
        raise ValueError("Synthetic DNA write failed: " + dna.Status.get().message)


def self_test():
    import numpy as np
    import riglogic
    from bake_arkit_dna import TARGET_NAMES, main

    with tempfile.TemporaryDirectory(prefix="arkit self test ") as directory:
        root = Path(directory)
        write_fixture(root / "head.dna")
        write_fixture(root / "body.dna", body=True)
        mapping = {"control_space": "raw", "targets": {
            name: {"controls": {"CTRL_expressions.test": 1.0}} for name in TARGET_NAMES}}
        (root / "mapping.json").write_text(json.dumps(mapping))
        with contextlib.redirect_stdout(io.StringIO()):
            main([str(root / "head.dna"), "--body-dna", str(root / "body.dna"),
                  "--mapping", str(root / "mapping.json"), "-o", str(root / "bake.glb")])
            main([str(root / "head.dna"), "--mapping", str(root / "mapping.json"),
                  "-o", str(root / "baked.dna")])
        dna_report = json.loads((root / "baked.report.json").read_text())
        if not dna_report["baked_dna"]["verification"]["verified"]:
            raise ValueError("Synthetic baked DNA failed RigLogic verification")
        report = json.loads((root / "bake.report.json").read_text())
        if report["zero_targets"] or len(report["target_names"]) != 52:
            raise ValueError("Synthetic bake did not produce 52 nonzero targets")
        raw = (root / "bake.glb").read_bytes()
        if struct.unpack_from("<4sII", raw) != (b"glTF", 2, len(raw)):
            raise ValueError("Invalid synthetic GLB header")
        length = struct.unpack_from("<I", raw, 12)[0]
        doc = json.loads(raw[20:20+length])
        binary = raw[28+length:]
        if len(doc["meshes"]) != 2 or "targets" in doc["meshes"][1]["primitives"][0]:
            raise ValueError("Synthetic body is missing or has facial targets")
        primitive = doc["meshes"][0]["primitives"][0]
        if len(primitive["targets"]) != 52:
            raise ValueError("Synthetic GLB is missing facial targets")
        for target in primitive["targets"]:
            accessor = doc["accessors"][target["POSITION"]]
            view = doc["bufferViews"][accessor["bufferView"]]
            values = np.frombuffer(binary, dtype="<f4", count=9,
                                   offset=view["byteOffset"]).reshape(3, 3)
            # One centimetre of joint translation for all vertices, plus one
            # centimetre of pre-skin corrective displacement for vertex 1.
            if not np.allclose(values, [[-.01, 0, 0], [-.01, 0, -.01], [-.01, 0, 0]], atol=1e-7):
                raise ValueError('Synthetic joint or corrective deformation is incorrect')
    return {"ok": True, "test": "synthetic-DNA/RigLogic/skinning/GLB/baked-DNA",
            "targets": 52, "body": True, "baked_dna": True,
            "openriglogic_version": riglogic.VersionInfo.getVersionString()}
