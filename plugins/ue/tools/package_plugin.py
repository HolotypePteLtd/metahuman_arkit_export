#!/usr/bin/env python3
"""Package Unreal plugin sources independently of the standalone CLI."""

import argparse
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PLUGIN_FILES = ('MHARKitExport.uplugin', 'Source', 'Config', 'Resources', 'Docs', 'python')
SKIP_NAMES = {'__pycache__', '.git', 'Binaries', 'Intermediate'}


def package_plugin(root=ROOT, output=None):
    root = Path(root).resolve()
    descriptor = json.loads((root / 'MHARKitExport.uplugin').read_text(encoding='utf-8'))
    version = descriptor['VersionName']
    if not version or any(c not in '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-_' for c in version):
        raise ValueError('Invalid plugin VersionName')
    output = Path(output) if output else root / 'dist' / f'MHARKitExport-{version}.zip'
    output = output.resolve()
    files = []
    for entry in PLUGIN_FILES:
        source = root / entry
        if not source.exists():
            raise ValueError('Missing plugin source: ' + entry)
        for path in sorted(source.rglob('*')) if source.is_dir() else [source]:
            relative = path.relative_to(root)
            if any(part in SKIP_NAMES for part in relative.parts) or path.suffix == '.pyc':
                continue
            if path.is_symlink():
                raise ValueError('Plugin sources must not contain symlinks: ' + str(relative))
            if path.is_file():
                files.append((path, relative.as_posix()))
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path, name in files:
            archive.write(path, name)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='Destination ZIP (default: dist/MHARKitExport-VERSION.zip)')
    args = parser.parse_args()
    print('Created ' + str(package_plugin(output=args.output)))


if __name__ == '__main__':
    main()
