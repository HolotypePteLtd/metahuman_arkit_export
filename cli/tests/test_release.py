"""Release failure cases: wrong architectures, damaged bundles, and stale inputs."""
import argparse
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build_tools'))
import build
import release
import release_utils as common
import verify


def elf(machine=62):
    data = bytearray(64)
    data[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<H', data, 18, machine)
    return bytes(data)


class ReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bundle = self.root / 'mh-arkit-export-0.1.0-linux-x64'
        self.bundle.mkdir()
        (self.bundle / common.PRODUCT).write_bytes(elf())
        (self.bundle / common.PRODUCT).chmod(0o755)
        (self.bundle / '_internal').mkdir()
        (self.bundle / '_internal/MANIFEST.json').write_text('also hash this file')
        self.record = {
            'schema': 1, 'application': common.PRODUCT, 'version': '0.1.0',
            'platform': 'linux-x64', 'openriglogic': {'version': '13.2.9'},
            'build_info': {'source_dirty': False},
            'release_eligible': True, 'compatibility': {'required_glibc': '2.28'},
            'self_test': {'ok': True, 'targets': 52, 'body': True,
                          'version': '0.1.0', 'openriglogic_version': '13.2.9'},
        }

    def archive(self):
        self.record['files'] = common.file_inventory(self.bundle)
        (self.bundle / 'MANIFEST.json').write_text(json.dumps(self.record))
        path = self.root / (self.bundle.name + '.tar.gz')
        build.make_archive(self.bundle, path, 'linux-x64')
        return path

    def sidecar(self):
        path = self.archive()
        return {'schema': 1, 'application': common.PRODUCT, 'platform': 'linux-x64',
                'version': '0.1.0', 'filename': path.name, 'size': path.stat().st_size,
                'sha256': common.sha256(path), 'release_eligible': True,
                'manifest': self.record}

    def test_relocated_archive_is_verified_without_execution_when_requested(self):
        path = self.archive()
        with patch.object(verify, 'verify_execution', side_effect=AssertionError('No foreign execution')):
            result = verify.verify_archive(path, expected_platform='linux-x64',
                                           expected_version='0.1.0', execute=False)
        self.assertEqual(result['files'], common.file_inventory(self.bundle))

    def test_corrupt_extra_and_missing_files_are_rejected(self):
        inventory = common.file_inventory(self.bundle)
        (self.bundle / '_internal/MANIFEST.json').write_text('tampered')
        with self.assertRaisesRegex(ValueError, 'inventory'):
            common.verify_inventory(self.bundle, inventory)
        (self.bundle / '_internal/MANIFEST.json').unlink()
        with self.assertRaisesRegex(ValueError, 'inventory'):
            common.verify_inventory(self.bundle, inventory)
        (self.bundle / '_internal/MANIFEST.json').write_text('also hash this file')
        (self.bundle / 'unexpected.txt').write_text('extra')
        with self.assertRaisesRegex(ValueError, 'inventory'):
            common.verify_inventory(self.bundle, inventory)

    def test_archive_architecture_is_checked_even_without_execution(self):
        (self.bundle / common.PRODUCT).write_bytes(elf(machine=183))
        with self.assertRaisesRegex(ValueError, 'architecture'):
            verify.verify_archive(self.archive(), execute=False)

    def test_deterministic_archive_metadata(self):
        first = self.archive().read_bytes()
        for path in self.bundle.rglob('*'):
            if path.is_file():
                import os
                os.utime(path, (1234567890, 1234567890))
        self.assertEqual(self.archive().read_bytes(), first)

    def test_release_refuses_failed_smoke_test_or_newer_glibc(self):
        self.record['self_test']['ok'] = False
        with self.assertRaisesRegex(ValueError, 'native bake'):
            release.check_artifact(self.sidecar(), self.root, '0.1.0', 'linux-x64')
        self.record['self_test']['ok'] = True
        self.record['compatibility']['required_glibc'] = '2.41'
        with self.assertRaisesRegex(ValueError, 'baseline'):
            release.check_artifact(self.sidecar(), self.root, '0.1.0', 'linux-x64')

    def test_release_refuses_development_or_dirty_source(self):
        self.record['release_eligible'] = False
        with self.assertRaisesRegex(ValueError, 'Development-only'):
            release.check_artifact(self.sidecar(), self.root, '0.1.0', 'linux-x64')
        self.record['release_eligible'] = True
        self.record['build_info']['source_dirty'] = True
        with self.assertRaisesRegex(ValueError, 'clean source'):
            release.check_artifact(self.sidecar(), self.root, '0.1.0', 'linux-x64')

    def test_release_refuses_changed_archive_bytes(self):
        record = self.sidecar()
        with (self.root / record['filename']).open('ab') as stream:
            stream.write(b'corruption')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            release.check_artifact(record, self.root, '0.1.0', 'linux-x64')

    def test_release_requires_the_entire_requested_matrix(self):
        with self.assertRaisesRegex(ValueError, 'Missing required platform'):
            release.assemble(self.root, self.root / 'out', '0.1.0', ['linux-x64', 'macos-arm64'])
        self.assertFalse((self.root / 'out').exists())

    def test_complete_matrix_assembles_and_mixed_sources_are_rejected(self):
        revision = subprocess.check_output(['git', '-C', str(common.ROOT), 'rev-parse', 'HEAD'], text=True).strip()
        runtime = json.loads((common.ROOT / 'runtime.json').read_text())
        info = {'source_dirty': False, 'source_revision': revision,
                'sources': {'src/bake_arkit_dna.py': common.sha256(common.ROOT / 'src/bake_arkit_dna.py')},
                'toolchain_sha256': 'locked-tools', 'python': '3.13.9', 'packages': {'numpy': '2.3.4'}}
        artifact_root = self.root / 'all-platforms'
        artifact_root.mkdir()
        sidecars = []
        for target in common.PLATFORMS:
            bundle = self.root / ('mh-arkit-export-0.1.0-' + target)
            bundle.mkdir(exist_ok=True)
            extension = '.exe' if target.startswith('windows-') else ''
            binary = bundle / (common.PRODUCT + extension)
            if target.startswith('linux-'):
                binary.write_bytes(elf(62 if target.endswith('x64') else 183))
            elif target.startswith('windows-'):
                data = bytearray(70)
                data[:2] = b'MZ'
                struct.pack_into('<I', data, 60, 64)
                struct.pack_into('<4sH', data, 64, b'PE\0\0', 0x8664 if target.endswith('x64') else 0xAA64)
                binary.write_bytes(data)
            else:
                cpu = 0x01000007 if target.endswith('x64') else 0x0100000C
                binary.write_bytes(b'\xcf\xfa\xed\xfe' + struct.pack('<I', cpu) + b'\0' * 56)
            manifest = dict(self.record, platform=target, openriglogic=runtime, build_info=info)
            manifest['files'] = common.file_inventory(bundle)
            (bundle / 'MANIFEST.json').write_text(json.dumps(manifest))
            suffix = '.zip' if target.startswith('windows-') else '.tar.gz'
            archive = artifact_root / (bundle.name + suffix)
            build.make_archive(bundle, archive, target)
            record = {'schema': 1, 'application': common.PRODUCT, 'version': '0.1.0',
                      'platform': target, 'filename': archive.name, 'size': archive.stat().st_size,
                      'sha256': common.sha256(archive), 'release_eligible': True, 'manifest': manifest}
            sidecar = artifact_root / (archive.name + '.json')
            sidecar.write_text(json.dumps(record))
            sidecars.append(sidecar)
        result = release.assemble(artifact_root, self.root / 'release', '0.1.0', list(common.PLATFORMS))
        self.assertEqual(len(result['artifacts']), 6)
        self.assertTrue((self.root / 'release/SHA256SUMS').is_file())
        # A platform assembled from a different source commit cannot be mixed in.
        record = json.loads(sidecars[-1].read_text())
        record['manifest']['build_info']['source_revision'] = 'a' * 40
        path = artifact_root / record['filename']
        source_bundle = self.root / path.name.removesuffix('.tar.gz')
        (source_bundle / 'MANIFEST.json').write_text(json.dumps(record['manifest']))
        build.make_archive(source_bundle, path, record['platform'])
        record['sha256'], record['size'] = common.sha256(path), path.stat().st_size
        sidecars[-1].write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, 'inputs differ'):
            release.assemble(artifact_root, self.root / 'invalid-release', '0.1.0', list(common.PLATFORMS))

    def test_tar_and_zip_reject_paths_and_links(self):
        archive_path = self.root / 'unsafe.tar.gz'
        for name, link in [('../escape', False), ('absolute/link', True)]:
            with tarfile.open(archive_path, 'w:gz') as archive:
                member = tarfile.TarInfo(name)
                if link:
                    member.type = tarfile.SYMTYPE
                    member.linkname = '/tmp/escape'
                else:
                    member.size = 1
                archive.addfile(member, None if link else io.BytesIO(b'x'))
            with self.assertRaises(ValueError):
                verify.extract_archive(archive_path, self.root / 'unpacked')
        archive_path = self.root / 'unsafe.zip'
        with zipfile.ZipFile(archive_path, 'w') as archive:
            archive.writestr('C:/escape', 'x')
        with self.assertRaisesRegex(ValueError, 'Unsafe'):
            verify.extract_archive(archive_path, self.root / 'unpacked')

    def test_offline_missing_source_does_not_fetch_or_create_source_directory(self):
        args = argparse.Namespace(source=None, offline=True, build_dir=self.root)
        with patch.object(build, 'run', side_effect=AssertionError('No downloads')):
            with self.assertRaisesRegex(ValueError, 'not cached'):
                build.source_tree(args, {'commit': 'a' * 40})
        self.assertFalse((self.root / 'sources').exists())

    def test_cross_platform_build_is_rejected_before_work_starts(self):
        args = argparse.Namespace(platform='windows-arm64')
        with patch.object(build, 'host_platform', return_value='linux-x64'):
            with self.assertRaisesRegex(ValueError, 'matching host'):
                build.build(args)

    def test_windows_records_msvc_instead_of_an_unrelated_mingw_compiler(self):
        with (patch.object(build.platform, 'system', return_value='Windows'),
              patch.object(build.Path, 'is_file', return_value=True),
              patch.object(build.shutil, 'which', side_effect=AssertionError('Do not use MinGW')),
              patch.object(build.subprocess, 'check_output', return_value='[{"installationVersion":"17.14"}]') as query):
            self.assertIn('17.14', build.compiler_identity())
        self.assertIn('[17.0,18.0)', query.call_args.args[0])

    def test_manifest_versions_cannot_traverse_paths(self):
        for value in ['../1.0.0', '1.0', '1.0.0/path', '01.0.0', '1.0.0\n']:
            with self.assertRaises(ValueError):
                common.version(value)

    def test_executable_headers_identify_all_target_families(self):
        path = self.root / 'binary'
        path.write_bytes(elf(183))
        self.assertEqual(common.binary_target(path), 'linux-arm64')
        for machine, expected in [(0x8664, 'windows-x64'), (0xAA64, 'windows-arm64')]:
            data = bytearray(70)
            data[:2] = b'MZ'
            struct.pack_into('<I', data, 60, 64)
            struct.pack_into('<4sH', data, 64, b'PE\0\0', machine)
            path.write_bytes(data)
            self.assertEqual(common.binary_target(path), expected)
        for cpu, expected in [(0x01000007, 'macos-x64'), (0x0100000C, 'macos-arm64')]:
            path.write_bytes(b'\xcf\xfa\xed\xfe' + struct.pack('<I', cpu) + b'\0' * 56)
            self.assertEqual(common.binary_target(path), expected)

    def test_recipient_probe_does_not_use_host_python_or_native_library_paths(self):
        with patch.dict('os.environ', {'PATH': '/host/python/bin', 'PYTHONHOME': '/host/python',
                                     'PYTHONPATH': '/host/packages', 'LD_LIBRARY_PATH': '/host/native'}):
            environment = common.isolated_environment()
        self.assertEqual(environment['PATH'], '')
        for key in ['PYTHONHOME', 'PYTHONPATH', 'LD_LIBRARY_PATH']:
            self.assertNotIn(key, environment)

    def test_macos_versions_and_library_paths_are_checked(self):
        path = self.bundle / 'native.dylib'
        path.write_bytes(b'\xcf\xfa\xed\xfe' + struct.pack('<I', 0x0100000C) + b'\0' * 56)

        def output(command, **kwargs):
            if '-l' in command:
                return 'Load command 0\n cmd LC_BUILD_VERSION\n minos 14.0.0\n sdk 15.0\n'
            return str(path) + ':\n /usr/lib/libSystem.B.dylib (compatibility version 1.0)\n'

        with patch.object(build.subprocess, 'check_output', side_effect=output):
            self.assertEqual(build.required_macos(self.bundle, '14.0'), '14.0.0')
            with self.assertRaisesRegex(ValueError, 'newer macOS'):
                build.required_macos(self.bundle, '13.0')

        with patch.object(build.subprocess, 'check_output',
                          return_value='native:\n /Users/builder/cache/libfoo.dylib (compatibility version 1.0)\n'):
            with self.assertRaisesRegex(ValueError, 'External non-system'):
                build.required_macos(self.bundle, '14.0')


if __name__ == '__main__':
    unittest.main()
