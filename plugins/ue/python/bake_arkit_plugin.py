"""Invoke MHARKitExport from Unreal Editor's Python interpreter.

Add this plugin's ``python`` directory to sys.path, then call
``bake('Scott', '/path/to/exports')``. With Execute Python Script, set
EXPORT_BS_CHAR and EXPORT_BS_DIR before running this file instead.
Requires the built MHARKitExport plugin and Unreal's Python scripting plugin.
"""

import os
from pathlib import Path


def bake(character, output_dir, *, exclude_eyelashes=True):
    """Bake a project character and return the resulting GLB path."""
    if not character or any(char in character for char in '/\\') or character in ('.', '..'):
        raise ValueError('character must be a MetaHuman character name')
    import unreal

    directory = Path(output_dir).expanduser().resolve()
    directory.mkdir(parents=True, exist_ok=True)
    output = directory / f'{character}_arkit.glb'
    result = unreal.MHARKitExportBPLibrary.bake_ar_kit_shape_keys(
        character, str(output), exclude_eyelashes=exclude_eyelashes)
    if not result or not output.is_file():
        raise RuntimeError(f'ARKit export failed for {character}; see the Unreal Output Log')
    print(f'Exported {output} ({output.stat().st_size / 1024 / 1024:.1f} MB)')
    return output


def main():
    character = os.environ.get('EXPORT_BS_CHAR', '')
    output_dir = os.environ.get('EXPORT_BS_DIR', '')
    if not character or not output_dir:
        raise RuntimeError('Set EXPORT_BS_CHAR and EXPORT_BS_DIR before executing this script in Unreal')
    bake(character, output_dir)


if __name__ == '__main__':
    main()
