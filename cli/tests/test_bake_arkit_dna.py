"""Coordinate, corrective, and additive-morph contracts for the native baker."""

import json
from dataclasses import replace
from pathlib import Path
import struct
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
import bake_arkit_dna as bake


def joints(count=1):
    neutral = np.zeros((count, 10))
    neutral[:, 6] = 1  # quaternion W
    neutral[:, 7:] = 1
    delta = np.zeros((count, 10))
    delta[:, 6] = 1
    return neutral, delta


class JointCompositionTest(unittest.TestCase):
    def test_translation_is_added_in_parent_axes(self):
        neutral, delta = joints()
        neutral[0, 3:7] = [0, 0, np.sqrt(0.5), np.sqrt(0.5)]
        neutral[0, :3] = [10, 20, 30]
        delta[0, :3] = [1, 0, 0]
        result = bake.joint_world_matrices(neutral, [0], delta)
        # Multiplying bind_local @ delta would incorrectly rotate this offset.
        np.testing.assert_allclose(result[0, :3, 3], [11, 20, 30])

    def test_hierarchy_supports_parent_after_child_and_rejects_cycles(self):
        neutral, _ = joints(3)
        neutral[:, :3] = [[1, 0, 0], [0, 2, 0], [0, 0, 3]]
        result = bake.joint_world_matrices(neutral, [2, 1, 1])
        np.testing.assert_allclose(result[0, :3, 3], [1, 2, 3])
        with self.assertRaisesRegex(ValueError, "Cycle"):
            bake.joint_world_matrices(neutral, [1, 2, 0])

    def test_corrective_is_applied_before_skinning(self):
        mesh = bake.Mesh("test", np.array([[1., 0, 0]]), np.array([0]),
                         np.zeros((1, 2)), np.zeros((0, 3), dtype=np.uint32),
                         np.ones((1, 1)), np.zeros((1, 1), dtype=np.int32),
                         [(0, np.array([0]), np.array([[1., 0, 0]]))])
        neutral, delta = joints()
        delta[0, 3:7] = [0, 0, np.sqrt(0.5), np.sqrt(0.5)]
        skin = bake.joint_world_matrices(neutral, [0], delta)
        np.testing.assert_allclose(mesh.deform(skin, [1]), [[0, 2, 0]], atol=1e-14)


class RecipeTest(unittest.TestCase):
    def test_mouth_close_cancels_lips_at_an_open_jaw(self):
        mapping = bake.default_recipes()

        def evaluate(controls):
            jaw = controls.get("CTRL_C_jaw.ty", 0)
            lips = controls.get("CTRL_L_mouth_lipsTogetherU.ty", 0)
            # A corrective that only exists when jaw and lips are active.
            return [np.array([[jaw, jaw * lips, 0.]])]

        targets = bake.bake_targets(evaluate, mapping["targets"], evaluate({}))
        np.testing.assert_array_equal(targets[-1][0], [[0, 1, 0]])

    def test_direct_gui_side_names_and_missing_control_failure(self):
        mapping = bake.default_recipes()
        self.assertEqual(mapping["targets"]["EyeBlinkLeft"]["controls"],
                         {"CTRL_L_eye_blink.ty": 1})
        names = {name for recipe in mapping["targets"].values()
                 for group in recipe.values() for name in group}
        bake.validate_recipes(mapping, names)
        names.remove("CTRL_L_eye_blink.ty")
        with self.assertRaisesRegex(ValueError, "EyeBlinkLeft: unknown controls"):
            bake.validate_recipes(mapping, names)


class GlbTest(unittest.TestCase):
    def test_body_is_in_scene_without_facial_morph_targets(self):
        face = bake.Mesh("head", np.zeros((3, 3)), np.arange(3),
                         np.zeros((3, 2)), np.array([[0, 1, 2]], dtype=np.uint32),
                         np.zeros((3, 1)), np.zeros((3, 1), dtype=np.int32), [])
        body = replace(face, name="body")
        head_positions = np.array([[0., 1.7, 0], [0.1, 1.7, 0], [0, 1.8, 0]])
        body_positions = np.array([[0., 0, 0], [0.1, 0, 0], [0, 1.5, 0]])
        targets = [[np.zeros((3, 3))] for _ in bake.TARGET_NAMES]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "body.glb"
            bake.write_glb(path, [face], [head_positions], targets,
                           [(body, body_positions)])
            raw = path.read_bytes()
        json_length = struct.unpack_from("<I", raw, 12)[0]
        document = json.loads(raw[20:20+json_length])
        self.assertEqual(document["scenes"][0]["nodes"], [0, 1])
        self.assertEqual(document["nodes"][1], {"mesh": 1, "name": "body"})
        self.assertEqual(len(document["meshes"][0]["primitives"][0]["targets"]), 52)
        body_entry = document["meshes"][1]
        self.assertNotIn("weights", body_entry)
        self.assertNotIn("targets", body_entry["primitives"][0])
        accessor = document["accessors"][body_entry["primitives"][0]["attributes"]["POSITION"]]
        self.assertEqual(accessor["min"], [0, 0, 0])
        self.assertAlmostEqual(accessor["max"][1], 1.5)

    def test_uv_seams_expand_all_targets_and_retain_zero_targets(self):
        mesh = bake.Mesh("test", np.zeros((3, 3)), np.array([0, 1, 2, 0]),
                         np.array([[0., 0], [1, 0], [0, 1], [1, 1]]),
                         np.array([[3, 1, 2]], dtype=np.uint32),
                         np.zeros((3, 1)), np.zeros((3, 1), dtype=np.int32), [])
        neutral = [np.array([[0., 0, 0], [1, 0, 0], [0, 1, 0]])]
        targets = [[np.zeros((3, 3))] for _ in bake.TARGET_NAMES]
        targets[0][0][0] = [0, 0, 1]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.glb"
            bake.write_glb(path, [mesh], neutral, targets)
            raw = path.read_bytes()
        self.assertEqual(struct.unpack_from("<4sII", raw), (b"glTF", 2, len(raw)))
        json_length, chunk = struct.unpack_from("<I4s", raw, 12)
        self.assertEqual(chunk, b"JSON")
        document = json.loads(raw[20:20+json_length])
        primitive = document["meshes"][0]["primitives"][0]
        self.assertEqual(len(primitive["targets"]), 52)
        self.assertEqual(document["meshes"][0]["extras"]["targetNames"], list(bake.TARGET_NAMES))
        accessor = document["accessors"][primitive["targets"][0]["POSITION"]]
        view = document["bufferViews"][accessor["bufferView"]]
        self.assertEqual(accessor["count"], 4)
        binary_start = 20 + json_length + 8
        values = np.frombuffer(raw, dtype="<f4", count=12,
                               offset=binary_start+view["byteOffset"]).reshape(-1, 3)
        np.testing.assert_array_equal(values[[0, 3]], [[0, 0, 1], [0, 0, 1]])


if __name__ == "__main__":
    unittest.main()
