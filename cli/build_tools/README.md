# Native build and release tools

These Python scripts use only the standard library (Python 3.10+). They build,
sign, package and verify the native C++ CLI. No Python environment, pip
installation, NumPy, SWIG, or runtime bindings are needed.

```bash
python3 cli/build_tools/toolchain.py
python3 cli/build_tools/build.py --jobs 4
```

`toolchain.json` pins CMake 4.2.3 and the distribution SHA-256 hashes for all
six targets. `toolchain.py` uses a matching CMake installation on PATH, or
downloads and verifies the native binaries into `cli/build/tools`. The wheel
files are used as ZIP containers for CMake; no Python packages are installed.
`CMAKE=/path/to/cmake` selects an existing matching installation.

The build driver caches its CMake download under its `--build-dir`. Use
`--offline` to require existing CMake and OpenRigLogic source; missing or
modified caches fail without downloading. `--source` accepts a clean checkout
of the pinned OpenRigLogic revision. On macOS, prefer an APFS build directory.

`build.py --native-only` compiles the CLI and native tests and runs CTest,
without creating a distribution archive. Full builds additionally run the
standard-library packaging/signing tests and verify the relocated archive.
All baking regression coverage is in `cli/native/tests`, including saved
synthetic expectations from the retired reference implementation.

Linux publication containers prepare only CMake and use the image's Python
for standard-library release scripts. Their generated image context contains
only tooling and platform metadata; application sources are mounted read-only.

See [BUILD.md](../../BUILD.md) for builds, offline use, signing and releases.
