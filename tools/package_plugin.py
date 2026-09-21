"""Zip the MHARKitExport plugin for FAB marketplace submission."""

import json
import os
import zipfile

BASE_DIR = os.path.dirname(__file__)
PLUGIN_DIR = os.path.join(BASE_DIR, "MHARKitExport")
UPLUGIN_PATH = os.path.join(PLUGIN_DIR, "MHARKitExport.uplugin")

SKIP_EXTENSIONS = {".pyc"}
SKIP_NAMES = {"__pycache__", ".git", "Binaries", "Intermediate"}


def should_include(name):
    return name not in SKIP_NAMES and os.path.splitext(name)[1] not in SKIP_EXTENSIONS


def read_version():
    """Read the VersionName (falling back to Version) from the .uplugin descriptor."""
    with open(UPLUGIN_PATH, encoding="utf-8") as f:
        descriptor = json.load(f)
    return descriptor.get("VersionName") or str(descriptor.get("Version", ""))


version = read_version()
output_zip = os.path.join(BASE_DIR, f"MHARKitExport-{version}.zip")

with zipfile.ZipFile(output_zip, "w", zipfile.ZIP_DEFLATED) as zf:
    for root, dirs, files in os.walk(PLUGIN_DIR):
        dirs[:] = [d for d in dirs if should_include(d)]
        for f in files:
            if not should_include(f):
                continue
            full_path = os.path.join(root, f)
            arcname = os.path.relpath(full_path, PLUGIN_DIR)
            zf.write(full_path, arcname)
            print(f"  {arcname}")

print(f"\nCreated {output_zip}")
