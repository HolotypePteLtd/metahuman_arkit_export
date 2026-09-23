#!/usr/bin/env python3
"""Build Linux release archives in pinned manylinux containers."""
import argparse
import hashlib
from pathlib import Path
import os
import platform
import subprocess

from release_utils import ROOT, PLATFORMS, version


def command(args):
    print('+ ' + ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True)


def build(args):
    if platform.system() != 'Linux':
        raise ValueError('Run Linux release containers on a Linux Docker host')
    targets = ('linux-x64', 'linux-arm64') if args.platform == 'all' else (args.platform,)
    if args.jobs < 1:
        raise ValueError('--jobs must be positive')
    args.cache.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    for target in targets:
        image = PLATFORMS[target]['container']
        digest = hashlib.sha256(image.encode())
        for filename in ('run.py', 'toolchain.json', 'pyproject.toml', 'uv.lock'):
            digest.update((ROOT / 'build_tools' / filename).read_bytes())
        digest.update((ROOT / 'build_tools/Dockerfile').read_bytes())
        tag = f'mh-arkit-builder:{target}-{digest.hexdigest()[:20]}'
        docker_platform = 'linux/amd64' if target == 'linux-x64' else 'linux/arm64'
        if args.offline:
            command(['docker', 'image', 'inspect', tag])
        else:
            command(['docker', 'build', '--platform', docker_platform, '--progress=plain',
                     '--build-arg', 'MANYLINUX_IMAGE=' + image, '--tag', tag,
                     '--file', ROOT / 'build_tools/Dockerfile', ROOT / 'build_tools'])
        command(['docker', 'run', '--rm', '--platform', docker_platform,
                 '--user', f'{os.getuid()}:{os.getgid()}', '--env', 'HOME=/tmp',
                 *( ['--network=none'] if args.offline else [] ),
                 '--mount', f'type=bind,src={ROOT.parent},dst=/work,readonly',
                 '--mount', f'type=bind,src={args.cache},dst=/cache',
                 '--mount', f'type=bind,src={args.output},dst=/out',
                 tag, 'python', '/work/cli/build_tools/build.py', '--version', args.version,
                 '--platform', target, '--build-dir', '/cache/' + target,
                 '--dist-dir', '/out', '--jobs', args.jobs,
                 *( ['--offline'] if args.offline else [] )])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=('linux-x64', 'linux-arm64', 'all'), default='all')
    parser.add_argument('--version', type=version, default=(ROOT / 'VERSION').read_text().strip())
    parser.add_argument('--cache', type=Path, default=ROOT / 'build/arkit-containers')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist/arkit')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--offline', action='store_true')
    args = parser.parse_args()
    args.cache, args.output = args.cache.resolve(), args.output.resolve()
    try:
        build(args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, 'ERROR: ' + str(error) + '\n')
