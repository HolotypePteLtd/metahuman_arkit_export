"""Read-only/offline guarantees of the pinned release environment."""
import json
import os
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
tools = runpy.run_path(str(ROOT / 'build_tools/run.py'))
ensure = tools['ensure']
globals_ = ensure.__globals__


class EnvironmentTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.environment = self.root / 'environment'
        self.context = patch.dict(os.environ, MHARKIT_TOOLS_CACHE=str(self.root / 'cache'),
                                  MHARKIT_TOOLS_ENV=str(self.environment))
        self.context.start()
        self.addCleanup(self.context.stop)

    def record(self):
        python = tools['python_path'](self.environment)
        python.parent.mkdir(parents=True)
        python.write_text('fixture interpreter')
        installed = {'python': '3.13.9', 'packages': {'uv': '0.11.4'}}
        tools['marker_path'](self.environment).write_text(json.dumps({
            'fingerprint': tools['fingerprint'](), 'installed': installed}))
        return installed

    def test_offline_missing_cache_never_installs(self):
        with patch.dict(globals_, uv_executable=lambda *args: self.fail('No downloads')):
            with self.assertRaisesRegex(ValueError, 'missing or changed'):
                ensure(offline=True)
        self.assertFalse((self.root / 'cache').exists())

    def test_read_only_check_reuses_verified_environment(self):
        installed = self.record()
        before = tools['marker_path'](self.environment).read_bytes()
        with patch.dict(globals_, probe=lambda *args: installed,
                        cache_lock=lambda *args: self.fail('No cache mutation')):
            self.assertEqual(ensure(check=True), self.environment)
        self.assertEqual(tools['marker_path'](self.environment).read_bytes(), before)

    def test_changed_interpreter_is_rejected_offline(self):
        self.record()
        with patch.dict(globals_, probe=lambda *args: {'python': '3.14.0'}):
            with self.assertRaisesRegex(ValueError, 'missing or changed'):
                ensure(offline=True)

    def test_unmanaged_environment_is_preserved(self):
        self.environment.mkdir()
        sentinel = self.environment / 'keep.txt'
        sentinel.write_text('keep')
        with self.assertRaisesRegex(ValueError, 'unmanaged environment'):
            ensure()
        self.assertEqual(sentinel.read_text(), 'keep')


if __name__ == '__main__':
    unittest.main()
