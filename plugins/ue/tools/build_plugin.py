#!/usr/bin/env python3
"""Build an Editor plugin ZIP with the selected Unreal Engine SDK."""

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import zipfile

from package_plugin import ROOT, package_plugin


def build_plugin(engine_root):
    if not engine_root:
        raise ValueError('Set UE_ROOT to an Unreal Engine directory (or pass --engine)')
    engine = Path(engine_root).expanduser().resolve()
    if (engine / 'Engine/Build/Build.version').is_file():
        engine /= 'Engine'
    version_file = engine / 'Build/Build.version'
    uat = engine / 'Build/BatchFiles' / ('RunUAT.bat' if os.name == 'nt' else 'RunUAT.sh')
    if not version_file.is_file() or not uat.is_file():
        raise ValueError('Unreal Engine Build.version or RunUAT is missing: ' + str(engine))
    host = {'Windows': 'Win64', 'Darwin': 'Mac', 'Linux': 'Linux'}.get(platform.system())
    if host is None:
        raise ValueError('Unsupported Unreal build host: ' + platform.system())
    engine_info = json.loads(version_file.read_text())
    engine_version = '.'.join(str(int(engine_info[name])) for name in ('MajorVersion', 'MinorVersion', 'PatchVersion'))
    descriptor = json.loads((ROOT / 'MHARKitExport.uplugin').read_text())
    version = descriptor['VersionName']
    repository = ROOT.parents[1]
    work_root = repository / 'build/ue'
    work_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='plugin-', dir=work_root) as temporary:
        work = Path(temporary)
        # Stage the same source allowlist as the source ZIP. Local outputs,
        # caches and other products never enter AutomationTool's host project.
        source_zip = package_plugin(root=ROOT, output=work / 'source.zip')
        source = work / 'source/MHARKitExport'
        with zipfile.ZipFile(source_zip) as archive:
            archive.extractall(source)
        package = work / 'package'
        subprocess.run([str(uat), 'BuildPlugin',
                        '-Plugin=' + str(source / 'MHARKitExport.uplugin'),
                        '-Package=' + str(package), '-NoTargetPlatforms', '-StrictIncludes'],
                       check=True, cwd=repository, shell=os.name == 'nt')
        built = json.loads((package / 'MHARKitExport.uplugin').read_text())
        expected_engine = f"{engine_info['MajorVersion']}.{engine_info['MinorVersion']}.0"
        if built.get('VersionName') != version or built.get('EngineVersion') != expected_engine:
            raise ValueError('Compiled plugin descriptor does not match the selected plugin/engine version')
        manifests = list((package / 'Binaries' / host).glob('*.modules'))
        binaries = [path.parent / data['Modules']['MHARKitExport']
                    for path in manifests if 'MHARKitExport' in (data := json.loads(path.read_text())).get('Modules', {})]
        if not binaries or not all(path.is_file() for path in binaries):
            raise ValueError('Compiled MHARKitExport module manifest or binary is missing')
        output = ROOT / 'dist' / f'MHARKitExport-{version}-UE{engine_version}-{host}.zip'
        output.parent.mkdir(parents=True, exist_ok=True)
        archive_path = work / output.name
        with zipfile.ZipFile(archive_path, 'w', zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(package.rglob('*')):
                if path.is_file():
                    archive.write(path, path.relative_to(package).as_posix())
        # Publish only after compilation and descriptor/module checks pass.
        shutil.copy2(archive_path, output.with_suffix('.zip.tmp'))
        os.replace(output.with_suffix('.zip.tmp'), output)
        return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', default=os.environ.get('UE_ROOT'), help='Engine directory or its installation root (default: UE_ROOT)')
    args = parser.parse_args()
    try:
        print('Created ' + str(build_plugin(args.engine)))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, 'ERROR: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
