"""Signing order, notarization acceptance, and CI credential cleanup."""
import base64
import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build_tools'))
import macos_signing as signing


class SigningTest(unittest.TestCase):
    def test_developer_id_uses_hardened_runtime_timestamp_and_execution_probe(self):
        with patch.object(signing, 'command', return_value='') as command:
            self.assertIsNone(signing.sign_and_notarize(Path('/bundle'), Path('/bundle/bake-arkit-dna'),
                                                        'Developer ID Application: Example'))
        calls = [call.args[0] for call in command.call_args_list]
        self.assertIn('--timestamp', calls[0])
        self.assertEqual(calls[0][calls[0].index('--options') + 1], 'runtime')
        self.assertEqual(calls[1][:3], ['codesign', '--verify', '--strict'])
        self.assertEqual(calls[2], [str(Path('/bundle/bake-arkit-dna')), '--self-test'])

    def test_adhoc_build_needs_no_private_credentials(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(signing, 'command', return_value='') as command:
            self.assertIsNone(signing.sign_and_notarize(Path('/bundle'), Path('/bundle/bake-arkit-dna'), None))
        self.assertEqual(command.call_args_list[0].args[0][:4], ['codesign', '--force', '--sign', '-'])

    def test_notarization_requires_signing_identity(self):
        with patch.object(signing, 'command') as command:
            with self.assertRaisesRegex(ValueError, 'requires a Developer ID'):
                signing.sign_and_notarize(Path('/bundle'), Path('/bundle/bake-arkit-dna'), None, True)
        command.assert_not_called()

    def test_notarization_checks_status_and_removes_api_key_after_success_or_failure(self):
        env = {'NOTARY_API_KEY': base64.b64encode(b'fixture private key').decode(),
               'NOTARY_KEY_ID': 'fixture-key-id', 'NOTARY_ISSUER_ID': 'fixture-issuer'}
        for status, returncode in [('Accepted', 0), ('Invalid', 0), ('Accepted', 1)]:
            with self.subTest(status=status, returncode=returncode):
                paths = []

                def submit(args, **kwargs):
                    key = Path(args[args.index('--key') + 1])
                    self.assertEqual(key.read_bytes(), b'fixture private key')
                    if os.name != 'nt':
                        self.assertEqual(key.stat().st_mode & 0o777, 0o600)
                    paths.append(key)
                    return subprocess.CompletedProcess(args, returncode,
                        '{"id":"fixture-submission","status":"' + status + '"}', '')

                with (patch.dict(os.environ, env, clear=True),
                      patch.object(signing, 'command', return_value='{}') as command,
                      patch.object(signing.subprocess, 'run', side_effect=submit),
                      contextlib.redirect_stdout(io.StringIO())):
                    if status == 'Accepted' and not returncode:
                        result = signing.sign_and_notarize(Path('/bundle'), Path('/bundle/binary'), 'Developer ID', True)
                        self.assertEqual(result, {'id': 'fixture-submission', 'status': 'Accepted'})
                    else:
                        with self.assertRaisesRegex(ValueError, 'not accepted'):
                            signing.sign_and_notarize(Path('/bundle'), Path('/bundle/binary'), 'Developer ID', True)
                self.assertTrue(paths)
                self.assertFalse(paths[0].exists())
                calls = [call.args[0] for call in command.call_args_list]
                self.assertEqual(calls[-1][:4], ['xcrun', 'notarytool', 'log', 'fixture-submission'])

    def test_security_command_failures_do_not_disclose_passwords(self):
        result = subprocess.CompletedProcess([], 1, '', 'sensitive diagnostic')
        with patch.object(signing.subprocess, 'run', return_value=result):
            with self.assertRaises(ValueError) as error:
                signing.command(['security', 'import', 'fixture.p12', '-P', 'never-print-this'])
        self.assertNotIn('never-print-this', str(error.exception))
        self.assertNotIn('sensitive diagnostic', str(error.exception))

    def test_keychain_lifecycle_restores_search_list_and_removes_certificate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'bake-arkit-signing'
            calls = []

            def command(args):
                args = list(map(str, args))
                calls.append(args)
                if args == ['security', 'list-keychains', '-d', 'user']:
                    return '"/original/login.keychain-db"\n'
                if args[:2] == ['security', 'create-keychain']:
                    Path(args[-1]).touch()
                return ''

            env = {'RUNNER_TEMP': directory, 'MACOS_SIGNING_P12': base64.b64encode(b'fixture certificate').decode(),
                   'MACOS_SIGNING_P12_PASSWORD': 'fixture-password'}
            with patch.dict(os.environ, env, clear=True), patch.object(signing, 'command', side_effect=command):
                signing.prepare_keychain()
                self.assertFalse((root / 'certificate.p12').exists())
                self.assertTrue((root / 'search-list.json').exists())
                signing.cleanup_keychain()
            self.assertFalse(root.exists())
            self.assertIn(['security', 'list-keychains', '-d', 'user', '-s', '/original/login.keychain-db'], calls)
            self.assertIn(['security', 'delete-keychain', str(root / 'signing.keychain-db')], calls)


if __name__ == '__main__':
    unittest.main()
