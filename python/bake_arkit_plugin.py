"""Bake ARKit shape keys via the MHARKitExport C++ plugin.

Single-command GLB export: spawns a temp actor, evaluates each ARKit expression
through the full MetaHuman animation pipeline, captures deformed verts, writes GLB.

Usage:
  python3 run_remote.py bake_arkit_plugin.py <character_name> [output_dir]

Requires: MHARKitExport plugin built and loaded in UE editor.
For MouthClose: AS_MetaHuman_ARKit_Mapping must have jawOpen+lipsTogether at frame 70.
"""
import unreal
import os

CHARACTER = "__EXPORT_BS_CHAR_PLACEHOLDER__"
_out_raw = "__EXPORT_BS_DIR_PLACEHOLDER__"
if _out_raw.startswith("__"):
    raise RuntimeError("OUTPUT_DIR not set — run via run_remote.py with output dir argument")
OUTPUT_DIR = _out_raw

os.makedirs(OUTPUT_DIR, exist_ok=True)
output_path = os.path.join(OUTPUT_DIR, f"{CHARACTER}_arkit.glb")

print(f"Baking ARKit shape keys for '{CHARACTER}'...")
print(f"Output: {output_path}")

result = unreal.MHARKitExportBPLibrary.bake_ar_kit_shape_keys(
    CHARACTER, output_path, exclude_eyelashes=True
)

if result:
    size_mb = os.path.getsize(output_path) / 1024 / 1024
    print(f"\nSuccess! {output_path} ({size_mb:.1f} MB)")
else:
    print("\nFailed — check Output Log for details")
