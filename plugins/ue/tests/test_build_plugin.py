"""AutomationTool staging and native artifact publication boundaries."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_plugin


class NativePluginBuildTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='ue build test ')
        self.addCleanup(self.temporary.cleanup)
        self.repository = Path(self.temporary.name)
        self.plugin = self.repository / 'repo/plugins/ue'
        self.plugin.mkdir(parents=True)
        self.descriptor = {'Version': 31, 'VersionName': '9.8.7'}
        (self.plugin / 'MHARKitExport.uplugin').write_text(json.dumps(self.descriptor))
        for directory in ('Source', 'Config', 'Resources', 'Docs', 'python'):
            (self.plugin / directory).mkdir()
            (self.plugin / directory / 'included.txt').write_text('source')
        (self.plugin / 'dist').mkdir()
        (self.plugin / 'dist/old.zip').write_text('previous source build')
        self.engine = self.repository / 'SDK/Engine'
        (self.engine / 'Build/BatchFiles').mkdir(parents=True)
        (self.engine / 'Build/Build.version').write_text(json.dumps({
            'MajorVersion': 5, 'MinorVersion': 8, 'PatchVersion': 1}))
        (self.engine / 'Build/BatchFiles' / ('RunUAT.bat' if os.name == 'nt' else 'RunUAT.sh')).touch()
        self.host = {'Windows': 'Win64', 'Darwin': 'Mac', 'Linux': 'Linux'}[build_plugin.platform.system()]
        self.output = self.plugin / 'dist' / f'MHARKitExport-9.8.7-UE5.8.1-{self.host}.zip'
        context = patch.object(build_plugin, 'ROOT', self.plugin)
        context.start()
        self.addCleanup(context.stop)

    def automation_tool(self, command, **kwargs):
        self.assertIn('-StrictIncludes', command)
        self.assertIn('-NoTargetPlatforms', command)
        source = Path(next(arg.removeprefix('-Plugin=') for arg in command if arg.startswith('-Plugin='))).parent
        package = Path(next(arg.removeprefix('-Package=') for arg in command if arg.startswith('-Package=')))
        self.assertTrue((source / 'Source/included.txt').is_file())
        self.assertFalse((source / 'dist').exists())
        self.assertFalse(package.exists())
        package.mkdir()
        (package / 'MHARKitExport.uplugin').write_text(json.dumps({
            **self.descriptor, 'EngineVersion': '5.8.0'}))
        binaries = package / 'Binaries' / self.host
        binaries.mkdir(parents=True)
        (binaries / 'UnrealEditor.modules').write_text(json.dumps({
            'BuildId': 'test-sdk', 'Modules': {'MHARKitExport': 'module.bin'}}))
        (binaries / 'module.bin').write_bytes(b'compiled module')
        return package

    def test_native_archive_uses_selected_sdk_and_excludes_local_outputs(self):
        with patch.object(build_plugin.subprocess, 'run', side_effect=self.automation_tool):
            output = build_plugin.build_plugin(self.engine.parent)
        self.assertEqual(output, self.output)
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(archive.read(f'Binaries/{self.host}/module.bin'), b'compiled module')
            self.assertEqual(json.loads(archive.read('MHARKitExport.uplugin'))['VersionName'], '9.8.7')

    def test_failed_build_preserves_previous_archive(self):
        self.output.write_bytes(b'previous native build')
        with patch.object(build_plugin.subprocess, 'run', side_effect=subprocess.CalledProcessError(6, ['RunUAT'])):
            with self.assertRaises(subprocess.CalledProcessError):
                build_plugin.build_plugin(self.engine)
        self.assertEqual(self.output.read_bytes(), b'previous native build')

    def test_missing_binary_is_rejected_before_publication(self):
        def incomplete_build(command, **kwargs):
            package = self.automation_tool(command, **kwargs)
            (package / 'Binaries' / self.host / 'module.bin').unlink()
        with patch.object(build_plugin.subprocess, 'run', side_effect=incomplete_build):
            with self.assertRaisesRegex(ValueError, 'binary is missing'):
                build_plugin.build_plugin(self.engine)
        self.assertFalse(self.output.exists())

    def test_missing_engine_fails_before_invoking_automation_tool(self):
        with patch.object(build_plugin.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'Set UE_ROOT'):
                build_plugin.build_plugin('')
        run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
