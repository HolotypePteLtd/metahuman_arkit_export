"""Native serialization, topology, pass-through controls, and failure atomicity."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import bake_arkit_dna as bake
import baked_dna

NATIVE = all(importlib.util.find_spec(name) is not None for name in ("dna", "riglogic"))


class ArgumentTest(unittest.TestCase):
    def test_dna_input_cannot_be_overwritten(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            bake.main(["head.dna", "-o", "head.dna"])

    def test_body_option_requires_glb(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            bake.main(["head.dna", "--body-dna", "body.dna", "-o", "baked.dna"])


@unittest.skipUnless(NATIVE, "requires the pinned OpenRigLogic bindings")
class NativeDNATest(unittest.TestCase):
    def setUp(self):
        import dna
        import riglogic
        from smoke_fixture import write_fixture

        self.dna, self.riglogic = dna, riglogic
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        write_fixture(self.root / "fixture.dna")
        stream = dna.FileStream(str(self.root / "fixture.dna"), dna.AccessMode_Read, dna.OpenMode_Binary)
        reader = dna.BinaryStreamReader(stream)
        reader.read()
        self.assertTrue(dna.Status.isOk())
        output = dna.FileStream(str(self.root / "source.dna"), dna.AccessMode_Write, dna.OpenMode_Binary)
        writer = dna.BinaryStreamWriter(output)
        writer.setFrom(reader)
        writer.setMetaData("test", "two LODs and nonzero rest rotations")
        writer.setNeutralJointRotations([[15.0, 20.0, 10.0]])
        writer.setLODCount(2)
        writer.setJointIndices(1, [0])
        writer.setLODJointMapping(1, 1)
        writer.setJointGroupLODs(0, [1, 1])
        writer.setBlendShapeChannelIndices(1, [0])
        writer.setLODBlendShapeChannelMapping(1, 1)
        writer.setBlendShapeChannelLODs([1, 1])
        writer.setMeshName(1, "second_mesh")
        writer.setMeshIndices(0, [0, 1])
        writer.setMeshIndices(1, [1])
        writer.setLODMeshMapping(1, 1)
        writer.setVertexPositions(1, [[0., 10., 0.], [10., 10., 0.], [0., 20., 0.]])
        writer.setVertexTextureCoordinates(1, [[0., 0.], [1., 0.], [0., 1.]])
        writer.setVertexNormals(1, [[0., 0., 1.]] * 3)
        writer.setVertexLayouts(1, [[0, 0, 0], [1, 1, 1], [2, 2, 2]])
        writer.setFaceVertexLayoutIndices(1, 0, [0, 1, 2])
        writer.setMaximumInfluencePerVertex(1, 1)
        for vertex in range(3):
            writer.setSkinWeightsValues(1, vertex, [1.0])
            writer.setSkinWeightsJointIndices(1, vertex, [0])
        writer.write()
        self.assertTrue(dna.Status.isOk())
        self.mapping = {"control_space": "raw", "targets": {
            name: {"controls": {"CTRL_expressions.test": (index + 1) / 52}}
            for index, name in enumerate(bake.TARGET_NAMES)}}
        for name in bake.TARGET_NAMES[-2:]:
            self.mapping["targets"][name]["subtract"] = self.mapping["targets"][name]["controls"]
        (self.root / "mapping.json").write_text(json.dumps(self.mapping))

    def run_bake(self, *extra):
        with contextlib.redirect_stdout(io.StringIO()):
            bake.main([str(self.root / "source.dna"), "--mapping", str(self.root / "mapping.json"),
                       "-o", str(self.root / "baked.dna"), *extra])

    def test_all_targets_mixed_poses_and_preserved_skeleton_topology_skin(self):
        self.run_bake()
        source = bake.NativeRig(self.root / "source.dna", self.dna, self.riglogic)
        result = bake.NativeRig(self.root / "baked.dna", self.dna, self.riglogic)
        reader = result.reader
        self.assertEqual(reader.getLODCount(), 1)
        self.assertEqual(reader.getMeshCount(), 2)
        self.assertEqual(reader.getJointGroupCount(), 0)
        self.assertEqual(reader.getPSDCount(), 0)
        self.assertEqual(result.raw_names, baked_dna.control_names(bake.TARGET_NAMES))
        self.assertEqual(result.gui_names, list(bake.TARGET_NAMES))
        np.testing.assert_allclose(result.neutral_joints, source.neutral_joints, atol=1e-7)
        for mesh in range(2):
            self.assertEqual(reader.getBlendShapeTargetCount(mesh), 52)
            for getter in ("getVertexTextureCoordinateUs", "getVertexTextureCoordinateVs",
                           "getVertexNormalXs", "getVertexNormalYs", "getVertexNormalZs",
                           "getVertexLayoutPositionIndices", "getVertexLayoutTextureCoordinateIndices",
                           "getVertexLayoutNormalIndices"):
                self.assertEqual(getattr(reader, getter)(mesh), getattr(source.reader, getter)(mesh))
            self.assertEqual(reader.getFaceVertexLayoutIndices(mesh, 0),
                             source.reader.getFaceVertexLayoutIndices(mesh, 0))
            for vertex in range(3):
                self.assertEqual(reader.getSkinWeightsValues(mesh, vertex), source.reader.getSkinWeightsValues(mesh, vertex))
                self.assertEqual(reader.getSkinWeightsJointIndices(mesh, vertex), source.reader.getSkinWeightsJointIndices(mesh, vertex))
            self.assertEqual(reader.getBlendShapeTargetDeltaCount(mesh, 51), 0)
        meshes = [bake.read_mesh(reader, i) for i in range(2)]
        raw = result.evaluate(meshes, {result.raw_names[7]: 0.75}, "raw")
        gui = result.evaluate(meshes, {result.gui_names[7]: 0.75}, "gui")
        for a, b in zip(raw, gui, strict=True):
            np.testing.assert_allclose(a, b, atol=1e-8)
        report = json.loads((self.root / "baked.report.json").read_text())
        self.assertEqual(report["zero_targets"], list(bake.TARGET_NAMES[-2:]))
        self.assertEqual(report["baked_dna"]["verification"]["poses"], 56)
        self.assertTrue(report["baked_dna"]["verification"]["verified"])

    def test_mesh_selection_remaps_source_mesh_index(self):
        self.run_bake("--mesh", "second_mesh")
        rig = bake.NativeRig(self.root / "baked.dna", self.dna, self.riglogic)
        self.assertEqual(rig.reader.getMeshCount(), 1)
        self.assertEqual(rig.reader.getMeshName(0), "second_mesh")
        self.assertEqual(list(rig.reader.getMeshIndicesForLOD(0)), [0])
        for i in range(52):
            mapping = rig.reader.getMeshBlendShapeChannelMapping(i)
            self.assertEqual((mapping.meshIndex, mapping.blendShapeChannelIndex), (0, i))

    def test_failed_verification_preserves_existing_output(self):
        output = self.root / "baked.dna"
        output.write_bytes(b"existing output")
        with patch.object(baked_dna, "verify_baked_dna", side_effect=ValueError("failed verification")):
            with self.assertRaisesRegex(ValueError, "failed verification"):
                self.run_bake()
        self.assertEqual(output.read_bytes(), b"existing output")
        self.assertFalse((self.root / "baked.report.json").exists())

    def test_glb_comparison_checks_serialized_vertices(self):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build_tools"))
        from compare_dna_glb import compare

        self.run_bake()
        reference = self.root / "reference.glb"
        with contextlib.redirect_stdout(io.StringIO()):
            bake.main([str(self.root / "source.dna"), "--mapping", str(self.root / "mapping.json"),
                       "-o", str(reference)])
        self.assertTrue(compare(self.root / "baked.dna", reference)["verified"])
        raw = bytearray(reference.read_bytes())
        json_length = struct.unpack_from("<I", raw, 12)[0]
        document = json.loads(raw[20:20+json_length])
        accessor = document["meshes"][0]["primitives"][0]["targets"][0]["POSITION"]
        view = document["bufferViews"][document["accessors"][accessor]["bufferView"]]
        struct.pack_into("<f", raw, 28 + json_length + view["byteOffset"], 10.0)
        reference.write_bytes(raw)
        self.assertFalse(compare(self.root / "baked.dna", reference)["verified"])

    def test_native_roundtrip_preserves_every_byte(self):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build_tools"))
        from roundtrip_dna import roundtrip

        verdict = roundtrip(self.root / "source.dna")
        self.assertTrue(verdict["fidelity"])
        self.assertEqual(verdict["lods"], 2)
        self.assertEqual(verdict["source_sha256"], verdict["roundtrip_sha256"])


if __name__ == "__main__":
    unittest.main()
