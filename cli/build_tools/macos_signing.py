#!/usr/bin/env python3
"""CI keychain lifecycle and Developer ID signing/notarization for the CLI."""
import argparse
import base64
import json
import os
from pathlib import Path
import secrets
import shlex
import shutil
import subprocess
import tempfile


def command(args):
    # In particular, never print security import passwords or exception argv.
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True)
    if result.returncode:
        raise ValueError(f'{args[0]} {args[1]} failed (exit {result.returncode})')
    return result.stdout


def decode_secret(name, destination):
    value = os.environ.get(name, '')
    if not value:
        raise ValueError('Missing signing secret: ' + name)
    contents = base64.b64decode(''.join(value.split()), validate=True)
    with destination.open('xb') as stream:
        os.chmod(destination, 0o600)
        stream.write(contents)


def prepare_keychain():
    root = Path(os.environ['RUNNER_TEMP']) / 'bake-arkit-signing'
    root.mkdir(mode=0o700)
    original = shlex.split(command(['security', 'list-keychains', '-d', 'user']))
    (root / 'search-list.json').write_text(json.dumps(original))
    keychain = root / 'signing.keychain-db'
    password = secrets.token_urlsafe(32)
    command(['security', 'create-keychain', '-p', password, keychain])
    command(['security', 'set-keychain-settings', '-lut', '21600', keychain])
    command(['security', 'unlock-keychain', '-p', password, keychain])
    certificate = root / 'certificate.p12'
    try:
        decode_secret('MACOS_SIGNING_P12', certificate)
        command(['security', 'import', certificate, '-k', keychain, '-P',
                 os.environ.get('MACOS_SIGNING_P12_PASSWORD', ''), '-T', '/usr/bin/codesign'])
        command(['security', 'set-key-partition-list', '-S', 'apple-tool:,apple:',
                 '-k', password, keychain])
        command(['security', 'list-keychains', '-d', 'user', '-s', keychain, *original])
    finally:
        certificate.unlink(missing_ok=True)


def cleanup_keychain():
    root = Path(os.environ['RUNNER_TEMP']) / 'bake-arkit-signing'
    if not root.exists():
        return
    state = root / 'search-list.json'
    keychain = root / 'signing.keychain-db'
    try:
        if state.is_file():
            command(['security', 'list-keychains', '-d', 'user', '-s', *json.loads(state.read_text())])
    finally:
        try:
            if keychain.exists():
                command(['security', 'delete-keychain', keychain])
        finally:
            shutil.rmtree(root)


def sign_and_notarize(bundle, executable, identity, notarize=False):
    if notarize and not identity:
        raise ValueError('Notarization requires a Developer ID signing identity')
    args = ['codesign', '--force', '--sign', identity or '-']
    if identity:
        args += ['--timestamp', '--options', 'runtime']
    command([*args, executable])
    command(['codesign', '--verify', '--strict', executable])
    # A valid on-disk signature must also survive execution under hardened runtime.
    command([str(executable), '--self-test'])
    if not notarize:
        return None
    for name in ('NOTARY_API_KEY', 'NOTARY_KEY_ID', 'NOTARY_ISSUER_ID'):
        if not os.environ.get(name):
            raise ValueError('Missing notarization secret: ' + name)
    # On CI, the always-run keychain cleanup also removes a notary key left
    # behind if this process is cancelled before its context manager unwinds.
    parent = Path(os.environ['RUNNER_TEMP']) / 'bake-arkit-signing' if os.environ.get('RUNNER_TEMP') else None
    if parent is not None and not parent.is_dir():
        parent = None
    with tempfile.TemporaryDirectory(prefix='bake-arkit-notary-', dir=parent) as directory:
        root = Path(directory)
        key = root / 'notary.p8'
        decode_secret('NOTARY_API_KEY', key)
        archive = root / 'submission.zip'
        command(['ditto', '-c', '-k', '--keepParent', bundle, archive])
        auth = ['--key', str(key), '--key-id', os.environ['NOTARY_KEY_ID'],
                '--issuer', os.environ['NOTARY_ISSUER_ID']]
        # Invalid submissions may still produce JSON with exit status zero;
        # check Apple's status explicitly, and always retrieve its diagnostic log.
        result = subprocess.run(['xcrun', 'notarytool', 'submit', str(archive), *auth,
                                 '--wait', '--timeout', '20m', '--output-format', 'json'],
                                capture_output=True, text=True)
        try:
            record = json.loads(result.stdout)
        except json.JSONDecodeError:
            raise ValueError('Apple notarization returned no valid JSON response') from None
        identifier = record.get('id')
        if identifier:
            log = command(['xcrun', 'notarytool', 'log', identifier, *auth])
            print('Apple notarization log: ' + log, flush=True)
        if result.returncode or record.get('status') != 'Accepted' or not identifier:
            raise ValueError('Apple notarization was not accepted: ' + str(record.get('status')))
        return {'id': identifier, 'status': record['status']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('prepare', 'cleanup'))
    arguments = parser.parse_args()
    try:
        {'prepare': prepare_keychain, 'cleanup': cleanup_keychain}[arguments.action]()
    except (OSError, ValueError) as error:
        parser.exit(1, 'ERROR: ' + str(error) + '\n')
