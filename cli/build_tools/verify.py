#!/usr/bin/env python3
"""Verify an extracted, relocated release archive and run its synthetic bake."""
import argparse
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import tempfile
import zipfile

from release_utils import PRODUCT, binary_target, host_platform, verify_execution, verify_inventory, version


def safe_name(name):
    path = PurePosixPath(name)
    if not name or '\\' in name or ':' in name or path.is_absolute() or '..' in path.parts:
        raise ValueError('Unsafe archive member: ' + name)
    return path


def extract_archive(path, destination):
    seen = set()
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            for member in archive.infolist():
                relative = safe_name(member.filename)
                if relative.as_posix() in seen:
                    raise ValueError('Duplicate archive member')
                seen.add(relative.as_posix())
                target = destination / relative
                if member.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                mode = member.external_attr >> 16
                if mode & 0o170000 == 0o120000:
                    raise ValueError('Archive must not contain symlinks')
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(member) as source, target.open('wb') as output:
                    shutil.copyfileobj(source, output)
                target.chmod(0o755 if mode & 0o111 else 0o644)
    else:
        with tarfile.open(path, 'r:gz') as archive:
            for member in archive:
                relative = safe_name(member.name)
                if relative.as_posix() in seen:
                    raise ValueError('Duplicate archive member')
                seen.add(relative.as_posix())
                target = destination / relative
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                if not member.isfile():
                    raise ValueError('Archive must contain regular files only')
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, target.open('wb') as output:
                    shutil.copyfileobj(source, output)
                target.chmod(0o755 if member.mode & 0o111 else 0o644)


def verify_archive(path, *, expected_platform=None, expected_version=None, execute=True):
    with tempfile.TemporaryDirectory(prefix='arkit relocated bundle ') as temporary:
        root = Path(temporary)
        extract_archive(Path(path), root)
        entries = list(root.iterdir())
        if len(entries) != 1 or not entries[0].is_dir():
            raise ValueError('Expected one top-level application directory')
        bundle = entries[0]
        record = json.loads((bundle / 'MANIFEST.json').read_text())
        if record.get('schema') != 1 or record.get('application') != PRODUCT:
            raise ValueError('Unsupported application manifest')
        version(record['version'])
        if expected_platform and record['platform'] != expected_platform:
            raise ValueError('Archive platform differs from expected platform')
        if expected_version and record['version'] != expected_version:
            raise ValueError('Archive version differs from expected version')
        verify_inventory(bundle, record['files'])
        executable = bundle / (PRODUCT + ('.exe' if record['platform'].startswith('windows-') else ''))
        if binary_target(executable) != record['platform']:
            raise ValueError('Executable architecture differs from the archive manifest')
        if execute:
            if host_platform() != record['platform']:
                raise ValueError('Run execution verification on the matching native platform')
            verify_execution(bundle, record['version'])
        return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--platform')
    parser.add_argument('--version')
    parser.add_argument('--integrity-only', action='store_true', help='Check a foreign archive without claiming runtime validation')
    args = parser.parse_args()
    info = verify_archive(args.archive, expected_platform=args.platform,
                          expected_version=args.version, execute=not args.integrity_only)
    print(json.dumps({'ok': True, 'platform': info['platform'], 'version': info['version'],
                      'executed': not args.integrity_only}, sort_keys=True))
