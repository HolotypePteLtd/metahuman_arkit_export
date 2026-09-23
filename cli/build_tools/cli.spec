# PyInstaller inputs are supplied by build.py; no local DNA or broad directory
# collection is permitted here.
import json
import os
from pathlib import Path

root = Path(SPECPATH).parent
native = Path(os.environ['MHARKIT_NATIVE_LIB'])
info = Path(os.environ['MHARKIT_BUILD_INFO'])
modules = json.loads((root / 'runtime.json').read_text())['binding_modules']
# Keep versioned dependency aliases as well as the unversioned import modules.
dependencies = [(str(p), '.') for p in sorted(native.iterdir())
                if p.is_file() and ('.so.' in p.name or p.suffix.lower() in ('.dll', '.dylib'))]
a = Analysis(
    [str(root / 'src/cli.py')],
    pathex=[str(root / 'src'), str(native)],
    binaries=dependencies, datas=[(str(info), '.')],
    hiddenimports=['dna', 'riglogic', *modules],
    hookspath=[], runtime_hooks=[], excludes=['bpy', 'unreal'],
    noarchive=False,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz, a.scripts, [], exclude_binaries=True,
    name='mh-arkit-export', debug=False, bootloader_ignore_signals=False,
    strip=False, upx=False, console=True,
    target_arch=os.environ.get('MHARKIT_MACOS_ARCH'),
    codesign_identity=os.environ.get('MHARKIT_CODESIGN_IDENTITY'),
)
coll = COLLECT(exe, a.binaries, a.datas, strip=False, upx=False, name='mh-arkit-export')
