"""CMake-only download integrity, cache preservation, and offline behavior."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build_tools'))
import toolchain


class ToolchainTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.cache = self.root / 'cache'
        self.archive = self.root / 'fixture.whl'
        name = 'cmake.exe' if os.name == 'nt' else 'cmake'
        with zipfile.ZipFile(self.archive, 'w') as wheel:
            info = zipfile.ZipInfo('cmake/data/bin/' + name)
            info.external_attr = 0o100755 << 16
            wheel.writestr(info, b'fixture CMake executable')
        digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.distribution = {'url': 'https://example.invalid/cmake.whl', 'sha256': digest}
        self.spec = {'schema': 2, 'cmake': '4.2.3', 'downloads': {toolchain.host_platform(): self.distribution}}
        for context in (patch.object(toolchain, 'configuration', return_value=self.spec),
                        patch.object(toolchain.shutil, 'which', return_value=None),
                        patch.object(toolchain, 'version_of', return_value='4.2.3'),
                        patch.dict(os.environ, {'CMAKE': ''})):
            context.start()
            self.addCleanup(context.stop)

    def install(self):
        with patch.object(toolchain.urllib.request, 'urlretrieve',
                          side_effect=lambda url, path: shutil.copy2(self.archive, path)):
            return toolchain.ensure(self.cache)

    def test_offline_missing_cache_never_downloads_or_mutates(self):
        with patch.object(toolchain.urllib.request, 'urlretrieve', side_effect=AssertionError('No downloads')):
            with self.assertRaisesRegex(ValueError, 'not installed or cached'):
                toolchain.ensure(self.cache, offline=True)
        self.assertFalse(self.cache.exists())

    def test_verified_download_is_reused_read_only(self):
        executable = self.install()
        self.assertTrue(executable.is_file())
        before = toolchain.file_inventory(self.cache)
        with patch.object(toolchain.urllib.request, 'urlretrieve', side_effect=AssertionError('No downloads')):
            self.assertEqual(toolchain.ensure(self.cache, check=True), executable)
        self.assertEqual(before, toolchain.file_inventory(self.cache))

    def test_modified_cache_is_rejected_before_execution(self):
        executable = self.install()
        executable.write_bytes(b'changed executable')
        with patch.object(toolchain, 'version_of', side_effect=AssertionError('Do not execute tampered tools')):
            with self.assertRaisesRegex(ValueError, 'modified'):
                toolchain.ensure(self.cache, offline=True)

    def test_checksum_mismatch_never_installs(self):
        self.distribution['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.install()
        self.assertFalse((self.cache / ('cmake-' + '0' * 16)).exists())

    def test_unmanaged_cache_is_preserved(self):
        target = self.cache / ('cmake-' + self.distribution['sha256'][:16])
        target.mkdir(parents=True)
        sentinel = target / 'keep.txt'
        sentinel.write_text('preserve')
        with self.assertRaisesRegex(ValueError, 'unmanaged'):
            self.install()
        self.assertEqual(sentinel.read_text(), 'preserve')

    def test_archive_traversal_is_rejected(self):
        with zipfile.ZipFile(self.archive, 'a') as wheel:
            wheel.writestr('../escape', b'do not write this')
        self.distribution['sha256'] = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        with self.assertRaisesRegex(ValueError, 'Unsafe'):
            self.install()
        self.assertFalse((self.cache / 'escape').exists())

    def test_matching_system_cmake_needs_no_tool_cache(self):
        installed = self.root / 'cmake'
        with patch.object(toolchain.shutil, 'which', return_value=str(installed)):
            self.assertEqual(toolchain.ensure(self.cache, offline=True), installed.resolve())
        self.assertFalse(self.cache.exists())


if __name__ == '__main__':
    unittest.main()
