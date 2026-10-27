#!/usr/bin/env python3
"""Run release commands in the shared, locked uv environment (stdlib bootstrap)."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
SPEC_FILES = ('toolchain.json', 'pyproject.toml', 'uv.lock')
PROBE = '''import importlib.metadata as m,json,platform,re
print(json.dumps(dict(python=platform.python_version(),packages={re.sub(r"[-_.]+","-",d.metadata["Name"]).lower():d.version for d in m.distributions()})))'''


def fingerprint():
    digest = hashlib.sha256()
    for name in SPEC_FILES:
        digest.update(name.encode() + b'\0' + (ROOT / name).read_bytes())
    return digest.hexdigest()


def configuration():
    value = json.loads((ROOT / 'toolchain.json').read_text())
    if value.get('schema') != 1:
        raise ValueError('Unsupported release toolchain configuration')
    return value


def cache_directory():
    if os.environ.get('MHARKIT_TOOLS_CACHE'):
        return Path(os.environ['MHARKIT_TOOLS_CACHE']).expanduser().resolve()
    if sys.platform == 'darwin':
        base = Path.home() / 'Library/Caches'
    elif os.name == 'nt':
        base = Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData/Local'))
    else:
        base = Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache'))
    return base / 'mh-arkit-export/release-tools'


def python_path(environment):
    return environment / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')


def tool_path(environment, name):
    return python_path(environment).parent / (name + '.exe' if os.name == 'nt' else name)


def clean_environment():
    env = os.environ.copy()
    for name in list(env):
        if name.startswith(('UV_', 'PIP_')) or name in ('PYTHONPATH', 'PYTHONHOME', 'VIRTUAL_ENV', 'CONDA_PREFIX', 'CONDA_DEFAULT_ENV'):
            env.pop(name, None)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    # Leave authentication and system compiler settings in their existing locations.
    return env


def probe(environment):
    result = subprocess.run([str(python_path(environment)), '-I', '-c', PROBE],
                            env=clean_environment(), check=True, capture_output=True, text=True)
    return json.loads(result.stdout)


def marker_path(environment):
    return environment.with_name(environment.name + '.mharkit-tools.json')


def ready(environment, identity):
    marker = marker_path(environment)
    if not marker.is_file() or not python_path(environment).is_file():
        return False
    record = json.loads(marker.read_text())
    if record.get('fingerprint') != identity or not record.get('installed'):
        return False
    return probe(environment) == record['installed']


@contextmanager
def cache_lock(cache):
    cache.mkdir(parents=True, exist_ok=True)
    with (cache / 'prepare.lock').open('a+b') as stream:
        if os.name == 'nt':
            import msvcrt
            if stream.tell() == 0:
                stream.write(b'\0'); stream.flush()
            stream.seek(0)
            deadline = time.monotonic() + 600
            while True:
                try:
                    msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise TimeoutError('Another process is preparing release tools')
                    time.sleep(.1)
        else:
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            if os.name == 'nt':
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def uv_executable(cache, env, version):
    found = shutil.which('uv')
    if found:
        result = subprocess.run([found, '--version'], capture_output=True, text=True, env=env)
        if result.returncode == 0 and result.stdout.split()[1:2] == [version]:
            return Path(found)
    bootstrap = cache / ('uv-' + version)
    executable = tool_path(bootstrap, 'uv')
    if not executable.is_file():
        subprocess.run([sys.executable, '-m', 'venv', '--copies', str(bootstrap)], check=True, env=env)
        subprocess.run([str(python_path(bootstrap)), '-m', 'pip', '--isolated', 'install', '--disable-pip-version-check',
                        '--no-input', '--no-deps', '--index-url', 'https://pypi.org/simple',
                        'uv==' + version], check=True, env=env)
    result = subprocess.run([str(executable), '--version'], capture_output=True, text=True, check=True, env=env)
    if result.stdout.split()[1:2] != [version]:
        raise ValueError('Cached uv differs from the release toolchain pin')
    return executable


def ensure(*, check=False, offline=False):
    identity, pins = fingerprint(), configuration()
    cache = cache_directory()
    environment = Path(os.environ.get('MHARKIT_TOOLS_ENV') or cache / 'environments' / (platform.system().lower() + '-' + platform.machine().lower() + '-' + identity[:20])).resolve()
    if ready(environment, identity):
        return environment
    if check or offline or os.environ.get('MHARKIT_TOOLS_OFFLINE') == '1':
        raise ValueError('Locked release environment is missing or changed. Run build_tools/run.py --prepare '
                         'online, or rebuild the Docker image with the current release-tools context.')
    with cache_lock(cache):
        if ready(environment, identity):
            return environment
        marker = marker_path(environment)
        if environment.exists() and not marker.is_file():
            raise ValueError('Refusing to modify an unmanaged environment: ' + str(environment))
        environment.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text(json.dumps(dict(fingerprint=identity, installed=None)) + '\n')
        env = clean_environment()
        temporary = cache / 'tmp'
        temporary.mkdir(exist_ok=True)
        env.update(UV_CACHE_DIR=str(cache / 'uv-cache'), UV_PYTHON_INSTALL_DIR=str(cache / 'python'),
                   UV_PROJECT_ENVIRONMENT=str(environment), UV_SYSTEM_CERTS='true',
                   TMPDIR=str(temporary), TEMP=str(temporary), TMP=str(temporary))
        uv = uv_executable(cache, env, pins['uv'])
        print('Preparing pinned MHARKIT release tools (Python ' + pins['python'] + ')', file=sys.stderr, flush=True)
        request = pins.get('python_windows_aarch64') if (os.name == 'nt'
                                                         and platform.machine().lower() in ('arm64', 'aarch64')) else pins['python']
        subprocess.run([str(uv), 'sync', '--project', str(ROOT), '--no-config', '--locked',
                        '--no-dev', '--no-install-project', '--managed-python', '--python', request,
                        '--link-mode', 'copy'], check=True, env=env)
        installed = probe(environment)
        if installed['python'] != pins['python'] or installed['packages'].get('uv') != pins['uv']:
            raise ValueError('Prepared release tools differ from the toolchain pins')
        marker.write_text(json.dumps(dict(fingerprint=identity, installed=installed), sort_keys=True) + '\n')
        return environment


def command_environment(environment):
    env = clean_environment()
    env.update(PATH=str(python_path(environment).parent) + os.pathsep + env.get('PATH', ''),
               VIRTUAL_ENV=str(environment), PYTHON=str(python_path(environment)),
               MHARKIT_TOOLS_ENV=str(environment), MHARKIT_TOOLS_ACTIVE=fingerprint())
    return env


def enter():
    """Re-enter a directly executed release driver before it imports build libraries."""
    if (os.environ.get('MHARKIT_TOOLS_ACTIVE') == fingerprint()
            and Path(sys.prefix) == Path(os.environ.get('MHARKIT_TOOLS_ENV', ''))):
        return
    environment = ensure(offline='--offline' in sys.argv[1:])
    raise SystemExit(subprocess.run([str(python_path(environment)), *sys.argv],
                                   env=command_environment(environment)).returncode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare', action='store_true', help='Prepare the environment without running a command')
    parser.add_argument('--check', action='store_true', help='Verify the prepared environment without changing it')
    parser.add_argument('--offline', action='store_true', help='Require the verified prepared environment')
    parser.add_argument('command', nargs=argparse.REMAINDER, help='python SCRIPT ... or another build command')
    args = parser.parse_args()
    try:
        environment = ensure(check=args.check, offline=args.offline)
        if args.prepare or args.check:
            print(environment)
            return 0
        command = args.command
        if command[:1] == ['--']:
            command = command[1:]
        if not command:
            parser.error('Supply a command or --prepare/--check')
        if command[0] in ('python', 'python3', 'cmake', 'swig', 'pyinstaller', 'uv'):
            command[0] = str(python_path(environment) if command[0].startswith('python') else tool_path(environment, command[0]))
        return subprocess.run(command, env=command_environment(environment)).returncode
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print('ERROR: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
