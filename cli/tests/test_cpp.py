"""Compare the native product with the Python reference, including serialized output."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

import numpy as np
import test_baked_dna as reference_tests
import bake_arkit_dna as reference

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build_tools'))
BINARY = os.environ.get('MHARKIT_CPP_BINARY')
BINDINGS = all(importlib.util.find_spec(name) is not None for name in ('dna', 'riglogic'))


class MappingTest(unittest.TestCase):
    def test_native_default_recipes_match_reference(self):
        self.assertEqual(json.loads((ROOT / 'native/default_mapping.json').read_text()),
                         reference.default_recipes())


def glb(path):
    data = path.read_bytes()
    magic, version, length = struct.unpack_from('<4sII', data)
    assert (magic, version, length) == (b'glTF', 2, len(data))
    json_length = struct.unpack_from('<I', data, 12)[0]
    doc = json.loads(data[20:20 + json_length])
    binary = data[28 + json_length:]

    def accessor(index):
        item = doc['accessors'][index]
        view = doc['bufferViews'][item['bufferView']]
        width = {'VEC3': 3, 'VEC2': 2, 'SCALAR': 1}[item['type']]
        dtype = '<f4' if item['componentType'] == 5126 else '<u4'
        return np.frombuffer(binary, dtype=dtype, count=item['count'] * width,
                             offset=view['byteOffset']).reshape(-1, width)
    return doc, accessor


@unittest.skipUnless(BINARY and BINDINGS, 'requires C++ executable and reference bindings')
class CppParityTest(unittest.TestCase):
    # Reuse the established two-mesh, two-LOD fixture with nonzero rest rotations.
    setUp = reference_tests.NativeDNATest.setUp

    def run_bake(self, *extra):
        self.cpp(self.root / 'baked.dna', *extra)

    def cpp(self, output, *extra, check=True):
        return subprocess.run([BINARY, str(self.root / 'source.dna'), '--mapping',
                               str(self.root / 'mapping.json'), '-o', str(output), *extra],
                              capture_output=True, text=True, check=check)

    def test_baked_dna_all_targets_mixed_poses_skeleton_topology_skin(self):
        reference_tests.NativeDNATest.test_all_targets_mixed_poses_and_preserved_skeleton_topology_skin(self)

    def test_mesh_selection_remaps_source_indices(self):
        reference_tests.NativeDNATest.test_mesh_selection_remaps_source_mesh_index(self)

    def test_glb_vertices_uvs_indices_targets_body_and_reports_match_reference(self):
        from smoke_fixture import write_fixture
        body = self.root / 'body.dna'
        write_fixture(body, body=True)
        for space in ('raw', 'gui'):
            mapping = json.loads((self.root / 'mapping.json').read_text())
            if space == 'gui':
                mapping['control_space'] = 'gui'
                for recipe in mapping['targets'].values():
                    for key in ('controls', 'subtract'):
                        if key in recipe:
                            recipe[key] = {'CTRL_test.ty': next(iter(recipe[key].values()))}
            (self.root / 'mapping.json').write_text(json.dumps(mapping))
            with self.subTest(space=space), contextlib.redirect_stdout(io.StringIO()):
                reference.main([str(self.root / 'source.dna'), '--mapping', str(self.root / 'mapping.json'),
                                '--body-dna', str(body), '-o', str(self.root / 'python.glb')])
                self.cpp(self.root / 'native.glb', '--body-dna', str(body))
                expected, a = glb(self.root / 'python.glb')
                actual, b = glb(self.root / 'native.glb')
                self.assertEqual(len(actual['meshes']), len(expected['meshes']))
                for x, y in zip(expected['meshes'], actual['meshes'], strict=True):
                    self.assertEqual(x.get('extras'), y.get('extras'))
                    self.assertEqual(x['name'], y['name'])
                    p, q = x['primitives'][0], y['primitives'][0]
                    self.assertEqual(len(p.get('targets', [])), len(q.get('targets', [])))
                    np.testing.assert_array_equal(a(p['indices']), b(q['indices']))
                    for attr in ('POSITION', 'TEXCOORD_0'):
                        np.testing.assert_allclose(a(p['attributes'][attr]), b(q['attributes'][attr]), atol=1e-7)
                    for t, u in zip(p.get('targets', []), q.get('targets', []), strict=True):
                        np.testing.assert_allclose(a(t['POSITION']), b(u['POSITION']), atol=1e-7)
                native_report = json.loads((self.root / 'native.report.json').read_text())
                python_report = json.loads((self.root / 'python.report.json').read_text())
                for key in ('source_sha256', 'dna_version', 'reader_dna_version', 'target_names',
                            'mapping', 'zero_targets', 'meshes', 'body'):
                    self.assertEqual(native_report[key], python_report[key], key)
                self.assertEqual(native_report['source_sha256'], hashlib.sha256((self.root / 'source.dna').read_bytes()).hexdigest())

    def test_invalid_mapping_preserves_existing_output(self):
        output = self.root / 'baked.dna'
        output.write_bytes(b'keep existing output')
        self.mapping['targets']['JawOpen']['controls'] = {'missing_control': 1}
        (self.root / 'mapping.json').write_text(json.dumps(self.mapping))
        result = self.cpp(output, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing_control', result.stderr)
        self.assertEqual(output.read_bytes(), b'keep existing output')
        self.assertFalse((self.root / 'baked.report.json').exists())

    def test_multijoint_rotations_scales_weights_and_uv_seams(self):
        dna = self.dna
        stream = dna.FileStream(str(self.root / 'source.dna'), dna.AccessMode_Read, dna.OpenMode_Binary)
        reader = dna.BinaryStreamReader(stream)
        reader.read()
        output = dna.FileStream(str(self.root / 'complex.dna'), dna.AccessMode_Write, dna.OpenMode_Binary)
        writer = dna.BinaryStreamWriter(output)
        writer.setFrom(reader)
        writer.setJointName(0, 'child')
        writer.setJointName(1, 'parent')
        writer.setJointHierarchy([1, 1])
        writer.setJointIndices(0, [0, 1])
        writer.setJointIndices(1, [0, 1])
        writer.setNeutralJointTranslations([[2., 3., 4.], [10., 20., 30.]])
        writer.setNeutralJointRotations([[15., 20., 10.], [10., -25., 30.]])
        writer.setJointRowCount(18)
        writer.setJointGroupLODs(0, [18, 18])
        writer.setJointGroupOutputIndices(0, list(range(18)))
        writer.setJointGroupValues(0, [1., -2., 3., 10., -5., 7., .1, -.2, .3,
                                     -3., 2., 1., -4., 6., -8., -.1, .2, .1])
        writer.setJointGroupJointIndices(0, [0, 1])
        for mesh in range(2):
            writer.setMaximumInfluencePerVertex(mesh, 2)
            for vertex in range(3):
                writer.setSkinWeightsValues(mesh, vertex, [.25, .75])
                writer.setSkinWeightsJointIndices(mesh, vertex, [0, 1])
            writer.setVertexTextureCoordinates(mesh, [[0., 0.], [1., 0.], [0., 1.], [.5, .5]])
            writer.setVertexLayouts(mesh, [[0, 0, 0], [1, 1, 1], [2, 2, 2], [0, 3, 0]])
            writer.setFaceVertexLayoutIndices(mesh, 0, [0, 1, 2, 3])
        writer.write()
        self.assertTrue(dna.Status.isOk())
        del writer, output, reader, stream
        (self.root / 'complex.dna').replace(self.root / 'source.dna')
        self.test_glb_vertices_uvs_indices_targets_body_and_reports_match_reference()
        self.cpp(self.root / 'baked.dna')
        with contextlib.redirect_stdout(io.StringIO()):
            reference.main([str(self.root / 'source.dna'), '--mapping',
                            str(self.root / 'mapping.json'), '-o', str(self.root / 'head.glb')])
        from compare_dna_glb import compare
        self.assertTrue(compare(self.root / 'baked.dna', self.root / 'head.glb')['verified'])

    def test_unicode_input_and_output_paths(self):
        directory = self.root / 'character café 角色'
        directory.mkdir()
        source = directory / 'tête.dna'
        shutil.copy2(self.root / 'source.dna', source)
        output = directory / '烘焙.glb'
        subprocess.run([BINARY, str(source), '--mapping', str(self.root / 'mapping.json'),
                        '-o', str(output)], capture_output=True, text=True, check=True)
        self.assertTrue(output.exists())
        report = json.loads(output.with_suffix('.report.json').read_text())
        self.assertEqual(Path(report['source_dna']), source.resolve())

    def test_source_cannot_be_overwritten_and_body_requires_glb(self):
        original = (self.root / 'source.dna').read_bytes()
        self.assertNotEqual(self.cpp(self.root / 'source.dna', check=False).returncode, 0)
        self.assertEqual((self.root / 'source.dna').read_bytes(), original)
        self.assertNotEqual(self.cpp(self.root / 'baked.dna', '--body-dna', str(self.root / 'source.dna'), check=False).returncode, 0)

    def test_executable_runs_alone_with_empty_path_and_no_internal_directory(self):
        alone = self.root / 'single executable with spaces'
        alone.mkdir()
        executable = alone / Path(BINARY).name
        shutil.copy2(BINARY, executable)
        env = os.environ.copy()
        for key in list(env):
            if key.startswith(('PYTHON', 'MHARKIT_', 'UV_', 'PIP_')) or key in ('LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH'):
                env.pop(key, None)
        env['PATH'] = ''
        result = subprocess.run([str(executable), '--self-test'], cwd=alone, env=env,
                                capture_output=True, text=True, check=True)
        self.assertTrue(json.loads(result.stdout)['ok'])
        self.assertEqual(list(alone.iterdir()), [executable])
