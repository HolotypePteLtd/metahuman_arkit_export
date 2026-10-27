#!/usr/bin/env python3
"""Locate or download only the pinned native CMake distribution (stdlib only)."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import time
import urllib.request
import zipfile

from release_utils import ROOT, file_inventory, host_platform, sha256

SPEC = ROOT / 'build_tools/toolchain.json'


def configuration():
    value = json.loads(SPEC.read_text())
    if value['schema'] != 2:
        raise ValueError('Unsupported toolchain configuration')
    return value


def fingerprint():
    return hashlib.sha256(SPEC.read_bytes()).hexdigest()


@contextmanager
def cache_lock(directory):
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / 'lock').open('a+b') as stream:
        if os.name == 'nt':
            import msvcrt
            if not stream.tell():
                stream.write(b'0')
                stream.flush()
            deadline = time.monotonic() + 600
            while True:
                try:
                    stream.seek(0)
                    msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise ValueError('Timed out waiting for build cache lock')
                    time.sleep(0.1)
            try:
                yield
            finally:
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(stream, fcntl.LOCK_UN)


def version_of(executable):
    result = subprocess.check_output([str(executable), '--version'], text=True)
    return result.splitlines()[0].split()[-1]


def ensure(directory, offline=False, check=False):
    spec = configuration()
    explicit = os.environ.get('CMAKE')
    if explicit:
        path = Path(explicit).expanduser().resolve()
        if version_of(path) != spec['cmake']:
            raise ValueError('CMAKE must point to CMake ' + spec['cmake'])
        return path
    installed = shutil.which('cmake')
    if installed and version_of(installed) == spec['cmake']:
        return Path(installed).resolve()
    distribution = spec['downloads'][host_platform()]
    cache = Path(directory).resolve() / ('cmake-' + distribution['sha256'][:16])
    executable = cache / ('cmake/data/bin/cmake.exe' if os.name == 'nt' else 'cmake/data/bin/cmake')
    marker = cache / 'MANIFEST.json'
    if marker.is_file():
        record = json.loads(marker.read_text())
        if (record.get('distribution') != distribution or record.get('files') != file_inventory(cache)
                or version_of(executable) != spec['cmake']):
            raise ValueError('CMake cache is modified; choose a new tool cache directory')
        return executable
    if offline or check:
        raise ValueError('Pinned CMake is not installed or cached; prepare the tools online first')
    if cache.exists():
        raise ValueError('Refusing to modify an unmanaged CMake cache: ' + str(cache))
    cache.parent.mkdir(parents=True, exist_ok=True)
    with cache_lock(cache.parent / ('lock-' + distribution['sha256'][:16])):
        if marker.is_file():
            return ensure(directory, offline=True)
        with tempfile.TemporaryDirectory(prefix='cmake-', dir=cache.parent) as temporary:
            root = Path(temporary)
            wheel = root / 'distribution.whl'
            urllib.request.urlretrieve(distribution['url'], wheel)
            if sha256(wheel) != distribution['sha256']:
                raise ValueError('CMake download checksum mismatch')
            contents = root / 'contents'
            contents.mkdir()
            with zipfile.ZipFile(wheel) as archive:
                for item in archive.infolist():
                    path = contents / item.filename
                    if (not path.resolve().is_relative_to(contents.resolve())
                            or stat.S_ISLNK(item.external_attr >> 16)):
                        raise ValueError('Unsafe CMake archive member')
                    archive.extract(item, contents)
                    if path.is_file() and item.external_attr >> 16 & 0o111:
                        path.chmod(0o755)
            record = {'distribution': distribution, 'files': file_inventory(contents)}
            (contents / 'MANIFEST.json').write_text(json.dumps(record, sort_keys=True) + '\n')
            contents.rename(cache)
    if version_of(executable) != spec['cmake']:
        raise ValueError('Downloaded CMake has an unexpected version')
    return executable


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=ROOT / 'build/tools')
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    try:
        print(ensure(args.directory, args.offline, args.check))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, 'ERROR: ' + str(error) + '\n')
