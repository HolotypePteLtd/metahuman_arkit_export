"""Standalone plugin packaging and the Unreal Python entry point."""

import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'python'))
import package_plugin
import bake_arkit_plugin


class PluginToolsTest(unittest.TestCase):
    def test_plugin_zip_excludes_baker_builds_and_character_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'MHARKitExport.uplugin').write_text(json.dumps({'VersionName': '0.2.0'}))
            for name in ('Source', 'Config', 'Resources', 'Docs', 'python'):
                (root / name).mkdir()
                (root / name / 'included.txt').write_text(name)
            for name in ('Source/Intermediate/generated.cpp', 'Source/__pycache__/x.pyc',
                         'cli/native/baker.cpp', 'cli/private.dna', 'build/runtime.so'):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('excluded')
            with zipfile.ZipFile(package_plugin.package_plugin(root)) as archive:
                self.assertEqual(set(archive.namelist()), {'MHARKitExport.uplugin',
                    'Source/included.txt', 'Config/included.txt', 'Resources/included.txt', 'Docs/included.txt', 'python/included.txt'})

    def test_unreal_failure_raises(self):
        api = Mock(return_value=False)
        with tempfile.TemporaryDirectory() as directory, patch.dict(sys.modules, {
                'unreal': SimpleNamespace(MHARKitExportBPLibrary=SimpleNamespace(bake_ar_kit_shape_keys=api))}):
            with self.assertRaisesRegex(RuntimeError, 'Unreal Output Log'):
                bake_arkit_plugin.bake('Scott', directory)

    def test_unreal_success_returns_export_path(self):
        def export(character, output, *, exclude_eyelashes):
            self.assertEqual(character, 'Scott')
            self.assertFalse(exclude_eyelashes)
            Path(output).write_bytes(b'glTF')
            return True
        with tempfile.TemporaryDirectory() as directory, patch.dict(sys.modules, {
                'unreal': SimpleNamespace(MHARKitExportBPLibrary=SimpleNamespace(bake_ar_kit_shape_keys=export))}):
            self.assertEqual(bake_arkit_plugin.bake('Scott', directory, exclude_eyelashes=False),
                             Path(directory).resolve() / 'Scott_arkit.glb')


if __name__ == '__main__':
    unittest.main()
