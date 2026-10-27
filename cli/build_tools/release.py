#!/usr/bin/env python3
"""Assemble verified platform archives into a reviewable release directory."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

from release_utils import ROOT, PRODUCT, PLATFORMS, sha256, version
from verify import verify_archive


def check_artifact(record, directory, expected_version, expected_platform):
    if (record.get('schema') != 1 or record.get('application') != PRODUCT
            or record.get('version') != expected_version or record.get('platform') != expected_platform):
        raise ValueError('Artifact identity does not match the release')
    filename = record['filename']
    if Path(filename).name != filename or '/' in filename or '\\' in filename:
        raise ValueError('Artifact filename must be a basename')
    suffix = '.zip' if expected_platform.startswith('windows-') else '.tar.gz'
    if filename != f'{PRODUCT}-{expected_version}-{expected_platform}{suffix}':
        raise ValueError('Artifact filename does not match the release identity')
    path = directory / filename
    if path.stat().st_size != record['size'] or sha256(path) != record['sha256']:
        raise ValueError('Artifact bytes differ from their recorded checksum: ' + filename)
    manifest = verify_archive(path, expected_platform=expected_platform,
                              expected_version=expected_version, execute=False)
    if manifest != record['manifest']:
        raise ValueError('Archive and sidecar manifests differ')
    if record.get('release_eligible') is not True or manifest.get('release_eligible') is not True:
        raise ValueError('Development-only artifact cannot be published')
    if manifest['build_info']['source_dirty']:
        raise ValueError('Release artifacts must come from a clean source checkout')
    test = manifest.get('self_test', {})
    expected = dict(ok=True, targets=52, body=True, version=expected_version,
                    openriglogic_version=manifest['openriglogic']['version'])
    if any(test.get(key) != value for key, value in expected.items()):
        raise ValueError('Artifact does not record a successful native bake test')
    if expected_platform.startswith('linux-'):
        required = tuple(map(int, manifest['compatibility']['required_glibc'].split('.')))
        if required > (2, 28):
            raise ValueError('Linux release exceeds the glibc 2.28 baseline')
    return manifest


def assemble(directory, destination, release_version, targets):
    if destination.exists():
        raise ValueError('Choose a new release output directory; refusing to overwrite ' + str(destination))
    records = {}
    for path in sorted(directory.rglob(f'{PRODUCT}-{release_version}-*.json')):
        if path.name.endswith(('.tar.gz.json', '.zip.json')):
            record = json.loads(path.read_text())
            target = record.get('platform')
            if target not in targets:
                continue
            if target in records:
                raise ValueError('Duplicate platform artifact: ' + target)
            manifest = check_artifact(record, path.parent, release_version, target)
            records[target] = (record, path, manifest)
    missing = set(targets) - records.keys()
    if missing:
        raise ValueError('Missing required platform archives: ' + ', '.join(sorted(missing)))
    source_revision = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    runtime_pin = json.loads((ROOT / 'runtime.json').read_text())
    first = next(iter(records.values()))[2]['build_info']
    for _record, _path, manifest in records.values():
        info = manifest['build_info']
        if info['source_revision'] != source_revision or manifest['openriglogic'] != runtime_pin:
            raise ValueError('Release inputs differ from the checked-out source and runtime pin')
        for key in ('sources', 'toolchain_sha256', 'tools'):
            if info[key] != first[key]:
                raise ValueError('Platform artifacts use different ' + key)
        if any(sha256(ROOT / path) != digest for path, digest in info['sources'].items()):
            raise ValueError('Application sources changed since artifacts were built')
    destination.mkdir(parents=True)
    artifacts = []
    for target in sorted(records):
        record, sidecar, _manifest = records[target]
        shutil.copy2(sidecar.parent / record['filename'], destination / record['filename'])
        shutil.copy2(sidecar, destination / sidecar.name)
        artifacts.append({key: record[key] for key in ('platform', 'filename', 'size', 'sha256')})
    release = dict(schema=1, application=PRODUCT, version=release_version,
                   source_revision=source_revision, openriglogic=runtime_pin, artifacts=artifacts)
    (destination / 'RELEASE_MANIFEST.json').write_text(json.dumps(release, sort_keys=True, indent=2) + '\n')
    (destination / 'SHA256SUMS').write_text(''.join(row['sha256'] + '  ' + row['filename'] + '\n' for row in artifacts))
    (destination / 'RELEASE_NOTES.md').write_text(
        f'{PRODUCT} {release_version}\n\n'
        'Native C++ MetaHuman DNA baker. Choose the download matching your OS and CPU.\n'
        'Extract the archive and run bake-arkit-dna (bake-arkit-dna.exe on Windows).\n'
        'The executable runs on its own; no Python runtime, _internal directory, Unreal, Blender, or compiler is required.\n\n'
        'This release renames the CLI and archives from mh-arkit-export to bake-arkit-dna.\n\n'
        'Bake a complete character:\n\n'
        '```text\nbake-arkit-dna head.dna --body-dna body.dna -o character.glb\n```\n\n'
        'Includes 52 facial morph targets and optional neutral body geometry. '
        'Textures, materials, and skeletal animation are not exported. '
        'Exact Unreal expression-mapping parity is not yet verified.\n\n'
        'Every archive passed its native synthetic-DNA bake after extraction and relocation. '
        'See README.txt, MANIFEST.json, and LICENSES/ in each download.\n')
    return release


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', type=version, required=True)
    parser.add_argument('--platform', action='append', choices=PLATFORMS,
                        help='Require only these platforms; default requires all six')
    args = parser.parse_args()
    try:
        result = assemble(args.artifacts, args.output, args.version, args.platform or list(PLATFORMS))
        print(json.dumps(result, sort_keys=True))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, 'ERROR: ' + str(error) + '\n')
