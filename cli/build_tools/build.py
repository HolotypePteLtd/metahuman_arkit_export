#!/usr/bin/env python3
"""Build and verify a standalone ARKit CLI on its matching native platform."""
import argparse
import gzip
import hashlib
import importlib.metadata
import inspect
import json
import os
from pathlib import Path
import platform
import re
import runpy
import shutil
import subprocess
import sys
import sysconfig
import tarfile
import tempfile
import zipfile

from release_utils import (ROOT, PRODUCT, PLATFORMS, binary_target, file_inventory,
                           host_platform, sha256, verify_execution, version)


def run(command, **kwargs):
    print('+ ' + ' '.join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def read_json(path):
    return json.loads(Path(path).read_text())


def write_json(path, value):
    Path(path).write_text(json.dumps(value, sort_keys=True, indent=2) + '\n')


def source_tree(args, pin):
    source = args.source or args.build_dir / 'sources' / pin['commit']
    if not source.exists():
        if args.offline:
            raise ValueError('Pinned OpenRigLogic source is not cached; prepare online first')
        source.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=source.parent, prefix='source-') as temporary:
            checkout = Path(temporary) / 'OpenRigLogic'
            run(['git', 'init', checkout])
            run(['git', '-C', checkout, 'fetch', '--depth', '1', pin['repository'], pin['commit']])
            run(['git', '-C', checkout, 'checkout', '--detach', 'FETCH_HEAD'])
            checkout.rename(source)
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True).strip()
    if actual != pin['commit'] or dirty:
        raise ValueError('OpenRigLogic checkout differs from the clean pinned revision: ' + str(source))
    return source.resolve()


def clean_environment():
    env = os.environ.copy()
    for key in ('CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'CFLAGS', 'CXXFLAGS',
                'CPPFLAGS', 'LDFLAGS', 'CMAKE_PREFIX_PATH', 'PYTHONPATH', 'PYTHONHOME'):
        env.pop(key, None)
    return env


def compiler_identity():
    if platform.system() == 'Windows':
        # GitHub's windows-11-arm image exposes vswhere on PATH (Chocolatey
        # shim) but not under the 32-bit installer directory, so query the
        # PATH copy first and keep the classic locations as fallbacks.
        candidates = []
        found = shutil.which('vswhere')
        if found:
            candidates.append(Path(found))
        for base in (os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                     os.environ.get('ProgramFiles', r'C:\Program Files')):
            candidates.append(Path(base) / 'Microsoft Visual Studio/Installer/vswhere.exe')
        for vswhere in candidates:
            if vswhere.is_file():
                result = subprocess.check_output([str(vswhere), '-version', '[17.0,18.0)',
                                                   '-latest', '-products', '*', '-format', 'json'], text=True)
                if json.loads(result):
                    return result
        raise ValueError('Visual Studio 2022 C++ Build Tools are required')
    compiler = shutil.which(os.environ.get('CXX', 'c++'))
    if compiler:
        return subprocess.check_output([compiler, '--version'], text=True).strip()
    raise ValueError('No native C++ compiler found')


def native_inventory(directory):
    # Importing a --native-only runtime with ordinary Python creates bytecode.
    # Verify installed inputs while allowing this disposable interpreter cache.
    return {name: record for name, record in file_inventory(directory).items()
            if '__pycache__' not in Path(name).parts}


def native_runtime(args, source, pin, tool_identity):
    env = clean_environment()
    target_info = PLATFORMS[args.platform]
    if args.platform.startswith('macos-'):
        env['MACOSX_DEPLOYMENT_TARGET'] = target_info['deployment_target']
    identity = dict(openriglogic=pin, platform=args.platform, python=platform.python_version(),
                    tools=tool_identity, compiler=compiler_identity(),
                    native_recipe_sha256=hashlib.sha256(''.join(inspect.getsource(function) for function in
                        (native_runtime, native_inventory, compiler_identity, clean_environment)).encode()).hexdigest(),
                    python_base=sys.base_prefix, deployment_target=env.get('MACOSX_DEPLOYMENT_TARGET'))
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    cache = args.build_dir / 'native' / key
    marker = cache / 'native.json'
    # The same lock implementation as the tool bootstrap serializes shared caches.
    tools = runpy.run_path(str(ROOT / 'build_tools/run.py'))
    with tools['cache_lock'](args.build_dir / 'native-locks' / key):
        if marker.is_file():
            record = read_json(marker)
            if record.get('identity') == identity and record.get('files') == native_inventory(cache / 'install'):
                print('Reusing verified native runtime: ' + key[:16], flush=True)
                return cache / 'install/lib', identity
            raise ValueError('Native cache is modified; remove this generated cache and rebuild: ' + str(cache))
        cache.mkdir(parents=True, exist_ok=True)
        options = [
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_STANDARD=17',
            '-DCMAKE_INSTALL_PREFIX=' + str(cache / 'install'), '-DCMAKE_INSTALL_LIBDIR=lib',
            '-DCMAKE_EXPORT_PACKAGE_REGISTRY=OFF', '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
            '-DREAD_ONLY_SOURCE_TREE=ON', '-DBUILD_SHARED_LIBS=OFF', '-DRL_LIBRARY_TYPE=STATIC',
            '-DRL_BUILD_PIC=ON', '-DRL_BUILD_TESTS=OFF', '-DRL_BUILD_BENCHMARKS=OFF',
            '-DRL_BUILD_EXAMPLES=OFF', '-DRL_BUILD_WITH_AVX=OFF', '-DRL_BUILD_WITH_SSE=OFF',
            '-DRL_BUILD_WITH_NEON=OFF', '-DRL_BUILD_WITH_HALF_FLOATS=OFF',
            '-DRL_BUILD_PYTHON_WRAPPER=' + platform.python_version(),
            '-DPython3_EXECUTABLE=' + sys.executable, '-DPython3_ROOT_DIR=' + sys.base_prefix,
            '-DSWIG_EXECUTABLE=' + str(Path(sys.executable).parent / ('swig.exe' if os.name == 'nt' else 'swig')),
        ]
        if os.name == 'nt':
            options += ['-G', 'Visual Studio 17 2022', '-A', 'ARM64' if args.platform.endswith('arm64') else 'x64',
                        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded']
        elif args.platform.startswith('macos-'):
            options += ['-DCMAKE_OSX_ARCHITECTURES=' + ('arm64' if args.platform.endswith('arm64') else 'x86_64'),
                        '-DCMAKE_OSX_DEPLOYMENT_TARGET=' + target_info['deployment_target'],
                        '-DCMAKE_INSTALL_RPATH=@loader_path']
        run(['cmake', '-S', source, '-B', cache / 'objects', *options], env=env)
        run(['cmake', '--build', cache / 'objects', '--config', 'Release', '--parallel', args.jobs], env=env)
        run(['cmake', '--install', cache / 'objects', '--config', 'Release'], env=env)
        if args.platform.startswith('macos-'):
            # The pinned upstream SWIG targets use TYPE SHARED and install
            # .dylib files. Python imports extensions only with a .so suffix.
            # Keep the dylibs too: RigLogic links against the versioned DNA one.
            for module in pin['binding_modules']:
                shutil.copy2(cache / 'install/lib' / (module + '.dylib'),
                             cache / 'install/lib' / (module + '.so'))
        env['PYTHONPATH'] = str(cache / 'install/lib')
        probe = subprocess.check_output([
            sys.executable, '-c', 'import dna,riglogic; print(riglogic.VersionInfo.getVersionString())'],
            env=env, text=True).strip()
        if probe != pin['version']:
            raise ValueError('Compiled runtime version differs from its pin')
        write_json(marker, {'identity': identity, 'files': native_inventory(cache / 'install')})
    return cache / 'install/lib', identity


def licenses(bundle, source):
    destination = bundle / 'LICENSES'
    destination.mkdir()
    shutil.copy2(source / 'LICENSE', destination / 'OpenRigLogic-MIT.txt')
    python_license = Path(sysconfig.get_path('stdlib')) / 'LICENSE.txt'
    if not python_license.is_file():
        candidates = [Path(sys.base_prefix) / 'LICENSE.txt', Path(sys.base_prefix) / 'LICENSE']
        python_license = next((p for p in candidates if p.is_file()), python_license)
    if not python_license.is_file():
        raise ValueError('Cannot locate the bundled Python license')
    shutil.copy2(python_license, destination / 'Python-LICENSE.txt')
    for name in ('numpy', 'pyinstaller', 'packaging'):
        distribution = importlib.metadata.distribution(name)
        files = [p for p in distribution.files
                 if '.dist-info/' in str(p) and ('license' in str(p).lower() or 'copying' in str(p).lower())]
        if not files:
            raise ValueError('Missing dependency license: ' + name)
        for file in files:
            relative = str(file).split('.dist-info/', 1)[1]
            target = destination / name / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(distribution.locate_file(file), target)


def required_glibc(bundle):
    maximum = (0, 0)
    for path in bundle.rglob('*'):
        if not path.is_file():
            continue
        with path.open('rb') as stream:
            if stream.read(4) != b'\x7fELF':
                continue
        output = subprocess.check_output(['readelf', '--version-info', str(path)], text=True)
        for match in re.findall(r'Name: GLIBC_(\d+\.\d+(?:\.\d+)?)\b', output):
            maximum = max(maximum, tuple(map(int, match.split('.'))))
    return '.'.join(map(str, maximum))


def verify_native_files(bundle, target):
    for path in bundle.rglob('*'):
        if not path.is_file():
            continue
        with path.open('rb') as stream:
            header = stream.read(4)
        if header[:2] == b'MZ' or header in (b'\x7fELF', b'\xcf\xfa\xed\xfe'):
            if binary_target(path) != target:
                raise ValueError('Foreign architecture in bundle: ' + str(path))


def required_macos(bundle, maximum):
    required = (0, 0)
    for path in bundle.rglob('*'):
        if not path.is_file():
            continue
        with path.open('rb') as stream:
            if stream.read(4) != b'\xcf\xfa\xed\xfe':
                continue
        output = subprocess.check_output(['otool', '-l', str(path)], text=True)
        for block in output.split('Load command '):
            if 'cmd LC_BUILD_VERSION' in block:
                match = re.search(r'\bminos (\d+\.\d+(?:\.\d+)?)', block)
            elif 'cmd LC_VERSION_MIN_MACOSX' in block:
                match = re.search(r'\bversion (\d+\.\d+(?:\.\d+)?)', block)
            else:
                continue
            if match:
                value = tuple(map(int, match[1].split('.')))
                required = max(required, value)
        links = subprocess.check_output(['otool', '-L', str(path)], text=True)
        for line in links.splitlines()[1:]:
            library = line.strip().split(' (', 1)[0]
            if library.startswith('/') and not library.startswith(('/usr/lib/', '/System/Library/')):
                raise ValueError('External non-system library in macOS bundle: ' + library)
    padded_required = (required + (0, 0, 0))[:3]
    padded_maximum = (tuple(map(int, maximum.split('.'))) + (0, 0, 0))[:3]
    if padded_required > padded_maximum:
        raise ValueError('Bundled Mach-O requires a newer macOS than the declared deployment target')
    return '.'.join(map(str, required))


def make_archive(bundle, destination, target):
    """Normalize archive metadata and preserve executable bits without symlinks."""
    if target.startswith('windows-'):
        with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(bundle.rglob('*')):
                if path.is_file():
                    info = zipfile.ZipInfo((Path(bundle.name) / path.relative_to(bundle)).as_posix(), (2020, 1, 1, 0, 0, 0))
                    info.external_attr = (0o100755 if os.access(path, os.X_OK) else 0o100644) << 16
                    info.compress_type = zipfile.ZIP_DEFLATED
                    archive.writestr(info, path.read_bytes())
    else:
        with destination.open('wb') as raw, gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode='w') as archive:
                for path in sorted(bundle.rglob('*')):
                    if path.is_file():
                        info = archive.gettarinfo(str(path), (Path(bundle.name) / path.relative_to(bundle)).as_posix())
                        info.uid = info.gid = info.mtime = 0
                        info.uname = info.gname = ''
                        info.mode = 0o755 if os.access(path, os.X_OK) else 0o644
                        # Dereference PyInstaller's internal shared-library links;
                        # every platform archive can be extracted without links.
                        info.type, info.linkname, info.size = tarfile.REGTYPE, '', path.stat().st_size
                        with path.open('rb') as stream:
                            archive.addfile(info, stream)


def build(args):
    if args.platform != host_platform():
        raise ValueError('Standalone bundles must be built on their matching host/interpreter')
    if args.jobs < 1:
        raise ValueError('--jobs must be positive')
    tools = runpy.run_path(str(ROOT / 'build_tools/run.py'))
    tool_identity = tools['fingerprint']()
    pin = read_json(ROOT / 'runtime.json')
    source = source_tree(args, pin)
    native, native_identity = native_runtime(args, source, pin, tool_identity)
    if args.native_only:
        print(native)
        return
    test_env = clean_environment()
    test_env['PYTHONPATH'] = str(native)
    for suite in ('tests',):
        run([sys.executable, '-m', 'unittest', 'discover', '-s', ROOT / suite, '-v'], cwd=ROOT, env=test_env)
    args.dist_dir.mkdir(parents=True, exist_ok=True)
    code_paths = ['src/bake_arkit_dna.py', 'src/baked_dna.py', 'src/arkit_controls.py',
                  'src/cli.py', 'src/smoke_fixture.py', 'build_tools/cli.spec']
    revision = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = bool(subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'], text=True).strip())
    info = {'schema': 1, 'application': PRODUCT, 'version': args.version, 'platform': args.platform,
            'source_revision': revision, 'source_dirty': dirty,
            'sources': {p: sha256(ROOT / p) for p in code_paths},
            'openriglogic': pin, 'python': platform.python_version(), 'toolchain_sha256': tool_identity,
            'packages': {name: importlib.metadata.version(name) for name in ('numpy', 'pyinstaller', 'cmake', 'swig')}}
    with tempfile.TemporaryDirectory(prefix='package-', dir=args.build_dir) as temporary:
        work = Path(temporary)
        write_json(work / 'BUILD_INFO.json', info)
        env = clean_environment()
        env.update(MHARKIT_NATIVE_LIB=str(native), MHARKIT_BUILD_INFO=str(work / 'BUILD_INFO.json'),
                   PYINSTALLER_CONFIG_DIR=str(work / 'pyinstaller-cache'))
        if args.platform.startswith('linux-'):
            # Let dependency analysis resolve the versioned DNA library that
            # the RigLogic extension links against, as well as its Python import.
            env['LD_LIBRARY_PATH'] = str(native)
        if args.platform.startswith('macos-'):
            env.update(MHARKIT_MACOS_ARCH='arm64' if args.platform.endswith('arm64') else 'x86_64',
                       MACOSX_DEPLOYMENT_TARGET=PLATFORMS[args.platform]['deployment_target'])
            if args.codesign_identity:
                env['MHARKIT_CODESIGN_IDENTITY'] = args.codesign_identity
        run([sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean',
             '--distpath', work / 'frozen', '--workpath', work / 'freeze-work',
             ROOT / 'build_tools/cli.spec'], env=env)
        # Copy with dereferencing to make the distributable independent of
        # symlink support (Windows, external drives, and zip extractors).
        bundle = work / f'{PRODUCT}-{args.version}-{args.platform}'
        shutil.copytree(work / 'frozen' / PRODUCT, bundle, symlinks=False)
        shutil.copy2(ROOT / 'USAGE.txt', bundle / 'README.txt')
        shutil.copy2(ROOT / 'THIRD_PARTY_NOTICES.txt', bundle / 'THIRD_PARTY_NOTICES.txt')
        licenses(bundle, source)
        verify_native_files(bundle, args.platform)
        compatibility = dict(PLATFORMS[args.platform])
        release_eligible = True
        if args.platform.startswith('linux-'):
            compatibility['required_glibc'] = required_glibc(bundle)
            required = tuple(map(int, compatibility['required_glibc'].split('.')))
            if required > (2, 28):
                release_eligible = False
                compatibility['minimum_system'] = 'glibc ' + compatibility['required_glibc']
                if not args.allow_newer_linux:
                    raise ValueError('Bundle needs glibc ' + compatibility['required_glibc']
                                     + '; build releases in the pinned manylinux container')
        elif args.platform.startswith('macos-'):
            compatibility['required_macos'] = required_macos(bundle, compatibility['deployment_target'])
        if args.windows_certificate:
            if not args.platform.startswith('windows-') or not re.fullmatch(r'[A-Fa-f0-9]{40}', args.windows_certificate):
                raise ValueError('--windows-certificate requires Windows and a certificate SHA1 thumbprint')
            for path in sorted(bundle.rglob('*')):
                if path.is_file() and path.suffix.lower() in ('.exe', '.dll', '.pyd'):
                    run(['signtool', 'sign', '/sha1', args.windows_certificate, '/fd', 'SHA256',
                         '/tr', 'http://timestamp.digicert.com', '/td', 'SHA256', path])
                    run(['signtool', 'verify', '/pa', path])
        if args.codesign_identity:
            if not args.platform.startswith('macos-'):
                raise ValueError('--codesign-identity requires macOS')
            run(['codesign', '--verify', '--deep', '--strict', bundle / PRODUCT])
        manifest = {'schema': 1, 'application': PRODUCT, 'version': args.version, 'platform': args.platform,
                    'openriglogic': pin, 'build_info': info, 'native_build': native_identity,
                    'compatibility': compatibility, 'release_eligible': release_eligible,
                    'signing': {'macos_identity': args.codesign_identity,
                                'windows_certificate': args.windows_certificate}}
        write_json(bundle / 'MANIFEST.json', manifest)
        test = verify_execution(bundle, args.version)
        manifest.update(self_test=test, files=file_inventory(bundle))
        write_json(bundle / 'MANIFEST.json', manifest)
        extension = '.zip' if args.platform.startswith('windows-') else '.tar.gz'
        archive = args.dist_dir / (bundle.name + extension)
        temporary_archive = work / archive.name
        make_archive(bundle, temporary_archive, args.platform)
        # Validate the actual archive after relocation before exposing it as a
        # release candidate. verify.py also checks its complete file inventory.
        from verify import verify_archive
        verify_archive(temporary_archive, expected_platform=args.platform, expected_version=args.version)
        shutil.copy2(temporary_archive, archive)
        artifact = {'schema': 1, 'application': PRODUCT, 'version': args.version,
                    'platform': args.platform, 'filename': archive.name, 'sha256': sha256(archive),
                    'size': archive.stat().st_size, 'release_eligible': release_eligible,
                    'manifest': manifest}
        write_json(args.dist_dir / (archive.name + '.json'), artifact)
        (args.dist_dir / (archive.name + '.sha256')).write_text(artifact['sha256'] + '  ' + archive.name + '\n')
        print('VERIFIED ARCHIVE: ' + str(archive), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', type=version, default=(ROOT / 'VERSION').read_text().strip())
    parser.add_argument('--platform', choices=PLATFORMS, default=host_platform())
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/arkit')
    parser.add_argument('--dist-dir', type=Path, default=ROOT / 'dist/arkit')
    parser.add_argument('--source', type=Path, help='Existing clean checkout of the pinned OpenRigLogic commit')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--native-only', action='store_true')
    parser.add_argument('--allow-newer-linux', action='store_true', help='Permit a local test artifact that release assembly will reject')
    parser.add_argument('--codesign-identity', help='macOS Developer ID signing identity')
    parser.add_argument('--windows-certificate', help='Windows certificate-store SHA1 thumbprint')
    args = parser.parse_args()
    args.build_dir, args.dist_dir = args.build_dir.resolve(), args.dist_dir.resolve()
    args.source = args.source.resolve() if args.source else None
    try:
        build(args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, 'ERROR: ' + str(error) + '\n')


if __name__ == '__main__':
    runpy.run_path(str(ROOT / 'build_tools/run.py'))['enter']()
    main()
