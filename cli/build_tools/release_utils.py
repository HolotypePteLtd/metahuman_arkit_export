"""Shared artifact and platform contracts for standalone releases."""
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PRODUCT = 'bake-arkit-dna'
PLATFORMS = json.loads((ROOT / 'platforms.json').read_text())


def version(value):
    if not re.fullmatch(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)', value):
        raise ValueError('Version must contain three non-negative integers')
    return value


def host_platform():
    system = {'Linux': 'linux', 'Darwin': 'macos', 'Windows': 'windows'}.get(platform.system())
    machine = platform.machine().lower()
    # Windows on ARM can also run an x64 Python. Inspect the interpreter build,
    # rather than labeling an emulated x64 binary as a native ARM64 release.
    if system == 'windows':
        import sysconfig
        machine = sysconfig.get_platform().removeprefix('win-')
    arch = {'x86_64': 'x64', 'amd64': 'x64', 'aarch64': 'arm64', 'arm64': 'arm64'}.get(machine)
    target = f'{system}-{arch}'
    if target not in PLATFORMS:
        raise ValueError('Unsupported host architecture: ' + target)
    return target


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def file_inventory(directory):
    root = Path(directory)
    return {p.relative_to(root).as_posix(): {'sha256': sha256(p), 'size': p.stat().st_size}
            for p in sorted(root.rglob('*')) if p.is_file() and p != root / 'MANIFEST.json'}


def verify_inventory(directory, record):
    actual = file_inventory(directory)
    if actual != record:
        raise ValueError('Bundle contents differ from the recorded inventory')


def binary_target(path):
    """Read executable headers without executing or relying on file(1)."""
    with Path(path).open('rb') as stream:
        header = stream.read(64)
        if header[:4] == b'\x7fELF':
            if header[4:6] != b'\x02\x01':
                raise ValueError('Expected a little-endian 64-bit ELF')
            arch = {62: 'x64', 183: 'arm64'}.get(struct.unpack_from('<H', header, 18)[0])
            return 'linux-' + str(arch)
        if header[:2] == b'MZ':
            stream.seek(struct.unpack_from('<I', header, 60)[0])
            signature, machine = struct.unpack('<4sH', stream.read(6))
            if signature != b'PE\0\0':
                raise ValueError('Invalid PE signature')
            return 'windows-' + str({0x8664: 'x64', 0xAA64: 'arm64'}.get(machine))
        if header[:4] == b'\xcf\xfa\xed\xfe':
            cpu = struct.unpack_from('<I', header, 4)[0]
            return 'macos-' + str({0x01000007: 'x64', 0x0100000C: 'arm64'}.get(cpu))
    raise ValueError('Unsupported executable format: ' + str(path))


def isolated_environment():
    env = os.environ.copy()
    for name in list(env):
        if name.startswith(('PYTHON', 'UV_', 'PIP_', 'MHARKIT_')) or name in (
                'VIRTUAL_ENV', 'CONDA_PREFIX', 'LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH'):
            env.pop(name, None)
    env['PATH'] = ''
    return env


def verify_execution(directory, expected_version):
    root = Path(directory).resolve()
    manifest = json.loads((root / 'MANIFEST.json').read_text())
    executable = root / (PRODUCT + ('.exe' if manifest['platform'].startswith('windows-') else ''))
    if binary_target(executable) != manifest['platform']:
        raise ValueError('Executable architecture does not match its manifest')
    env = isolated_environment()
    for args, expected in [(['--version'], f'{PRODUCT} {expected_version}')]:
        result = subprocess.run([str(executable), *args], cwd=root, env=env,
                                capture_output=True, text=True, timeout=60, check=True)
        if result.stdout.strip() != expected:
            raise ValueError('Standalone version probe returned an unexpected result')
    result = subprocess.run([str(executable), '--self-test'], cwd=root, env=env,
                            capture_output=True, text=True, timeout=180, check=True)
    test = json.loads(result.stdout)
    expected = {'ok': True, 'targets': 52, 'body': True, 'version': expected_version,
                'openriglogic_version': manifest['openriglogic']['version']}
    if any(test.get(key) != value for key, value in expected.items()):
        raise ValueError('Standalone synthetic bake did not pass: ' + result.stdout)
    result = subprocess.run([str(executable), '--build-info'], cwd=root, env=env,
                            capture_output=True, text=True, timeout=60, check=True)
    if json.loads(result.stdout) != manifest['build_info']:
        raise ValueError('Executable build provenance differs from its manifest')
    return test
