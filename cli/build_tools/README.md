# Pinned standalone release tools

A standard-library launcher prepares a separate uv environment, pins the
exact Python/tool versions, uses a checked-in dependency lock, and verifies
installed versions before reusing a cache. All build configuration is in this
repository; no other project checkout or environment is required.

```bash
python3 cli/build_tools/run.py --prepare
python3 cli/build_tools/run.py --check
python3 cli/build_tools/run.py python cli/build_tools/build.py --help
```

The bootstrap host needs Python 3.9+ with venv/pip. It downloads the exact
Python 3.13.9 runtime selected in `toolchain.json`; recipients of the final
CLI do not need Python. `pyproject.toml` and `uv.lock` pin CMake 3.31.6,
SWIG 4.4.1, NumPy 2.3.4, PyInstaller 6.22.3, uv 0.11.4, and transitive
dependencies, including platform wheel hashes.

Environment caches are keyed by OS, CPU architecture, and the toolchain/lock
fingerprint. Default locations are `~/.cache/mh-arkit-export/release-tools/`
on Linux, `~/Library/Caches/mh-arkit-export/release-tools/` on macOS, and
`%LOCALAPPDATA%/mh-arkit-export/release-tools/` on Windows.
`MHARKIT_TOOLS_CACHE` overrides the cache root; `MHARKIT_TOOLS_ENV` selects
an explicitly managed environment. Unmanaged environments are preserved.
macOS compilation caches should live on APFS rather than exFAT drives.

`--offline` or `MHARKIT_TOOLS_OFFLINE=1` requires a matching prepared
environment. The launcher never installs or repairs dependencies in this mode.
It removes inherited Python, pip, uv, and active-environment overrides before
invoking managed tools. Git, compilers, SDKs, Docker, and signing tools remain
host dependencies.

To update dependencies, edit the pins and regenerate the lock using the
pinned uv version. Ordinary builds run `uv sync --locked` and cannot update it.
If changing Python, update both declarations in `toolchain.json` and
`pyproject.toml`, then review the native CI matrix and Docker bootstrap path.

See [standalone release instructions](../README.md).
