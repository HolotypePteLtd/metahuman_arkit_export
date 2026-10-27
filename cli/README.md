# Standalone MH ARKit Export releases

This builds the Python DNA baker into a standalone command-line application.
Each download contains `mh-arkit-export` (or `.exe`), its `_internal` runtime
directory, usage instructions, dependency licenses, and a manifest. Users
extract the complete archive and run the executable; Python, Unreal, Blender,
NumPy installations, and compilers are not required on their machines.

```text
mh-arkit-export head.dna --body-dna body.dna -o character.glb
mh-arkit-export head.dna -o character_arkit.dna
mh-arkit-export --self-test
```

Both formats include 52 baked facial morph targets. GLB can include optional
unrigged neutral body geometry. DNA retains the neutral head skeleton, skin
weights, UVs, and topology at LOD0, with 52 pass-through controls replacing the
original facial solvers. Joint motion is baked into the targets. Materials,
textures, and wrinkle maps are not exported. Exact parity with Unreal's expression mapping remains
unverified. See [the baker documentation](docs/BAKING.md).

## Release matrix

| Download target | Build environment | Compatibility target |
| --- | --- | --- |
| `linux-x64` | Pinned manylinux 2.28 x86_64 container | glibc 2.28+ |
| `linux-arm64` | Pinned manylinux 2.28 aarch64 container | glibc 2.28+ |
| `windows-x64` | Native Windows / MSVC | Windows 10+ x64 |
| `windows-arm64` | Native Windows ARM64 / MSVC | Windows 11 ARM64 |
| `macos-x64` | Native Intel macOS | macOS 14+ |
| `macos-arm64` | Native Apple Silicon macOS | macOS 14+ |

`platforms.json` records runner labels and immutable Linux image digests.
Every platform runs the extracted executable on its matching architecture.
macOS CI currently runs on macOS 15 while targeting 14; Mach-O deployment
versions and library references are checked before packaging. Windows ARM64 CI
runs on the `windows-11-arm` image's preinstalled interpreter, because
`actions/setup-python` cannot force-install 3.13.9 there; the bootstrap
installs the pinned interpreter regardless and selects uv's native
`windows-aarch64` build, so the frozen executable is ARM64 rather than
emulated x64. Linux builds
inspect the actual GLIBC requirements of every bundled ELF file and reject
artifacts exceeding 2.28. A successful build on a newer workstation alone
does not establish the Linux compatibility target.

The default builder uses portable scalar OpenRigLogic code, without AVX,
SSE, NEON, half-float, or host-specific CPU flags. MSVC builds statically
link the C++ runtime. PyInstaller collects the Python/NumPy runtime dependencies.
Compilers, SDKs, and build tools are not included in the downloads.

## Local builds

The build tools use a pinned, isolated release environment contained entirely
under `cli/`. Run these commands from the repository root. The bootstrap needs
Python 3.9+ with venv/pip, then selects its own exact Python and package versions.

Windows needs Visual Studio 2022 Build Tools with the matching x64 or ARM64
C++ toolset and Windows SDK. macOS needs Xcode command-line tools. Git is
required to fetch and verify the pinned OpenRigLogic source.

```bash
python3 cli/build_tools/run.py --prepare
python3 cli/build_tools/run.py python cli/build_tools/build.py --version 0.1.0
```

On Windows, use `python` if `python3` is not available. The platform is detected
from the actual Python architecture; selecting a foreign target fails before
building. The CLI requires no end-user Python ABI selection because Python
is bundled.

Make aliases are available:
`make -C cli tools`, `test`, `build`, `native`, `docker`, and
`assemble`. They accept `VERSION`, `PLATFORM`, and `JOBS` as appropriate.

Build Linux publication artifacts through Docker:

```bash
python3 cli/build_tools/docker_build.py --platform linux-x64 --version 0.1.0
python3 cli/build_tools/docker_build.py --platform linux-arm64 --version 0.1.0
```

The container contains the same locked release tools as local builds. Its
Docker context includes only `cli/build_tools`, so character files and other
workspace assets cannot be baked into the builder image. The source checkout
is mounted read-only when compiling. Cache and artifact directories are
separate writable mounts. Building the foreign Linux architecture locally
requires Docker's binfmt/QEMU support; CI uses native runners for both.

Artifacts appear under `cli/dist/arkit/`, named, for example:

```text
mh-arkit-export-0.1.0-linux-x64.tar.gz
mh-arkit-export-0.1.0-linux-x64.tar.gz.json
mh-arkit-export-0.1.0-linux-x64.tar.gz.sha256
```

`--build-dir` selects a native compilation cache; `--dist-dir` selects the
artifact directory. Keep these distinct from your DNA inputs. Native cache
identity includes the runtime revision, native build recipe, compiler,
platform, Python installation, deployment target, and locked tools. The
installed-file inventory is checked before a cached runtime is reused.
`--source /path/to/OpenRigLogic` accepts an existing clean checkout only if
its revision exactly matches `runtime.json`.

After online preparation, require cached inputs:

```bash
python3 cli/build_tools/run.py --offline python cli/build_tools/build.py --offline
python3 cli/build_tools/docker_build.py --platform linux-x64 --offline
```

The Docker offline command requires an already prepared builder image and
cached source/runtime; it disables the container network. Local compilation
on a newer Linux system can be tested using `--allow-newer-linux`. Such an
archive records its actual GLIBC requirement and is marked ineligible for
release assembly. Do not publish it as the glibc 2.28 download.

## Verification

Every package build runs the Python tests and build-system tests, then freezes
the tested baker with the pinned native runtime. The runtime self-test creates
a tiny synthetic DNA with a bone and corrective shape, bakes all 52 targets,
adds a neutral body, and checks the numerical GLB and baked DNA output. No Scott files or
other character assets are included in the test or release artifacts.

The builder verifies CPU architecture for all collected native files, records
a file-by-file SHA256 inventory, archives the bundle, extracts it into a new
directory containing spaces, and runs version, provenance, and bake probes.
Those probes run with empty `PATH` and no inherited Python or library paths.

Verify a finished archive independently:

```bash
python3 cli/build_tools/verify.py cli/dist/arkit/mh-arkit-export-0.1.0-linux-x64.tar.gz
```

Run this on its matching platform. `--integrity-only` allows inspection of a
foreign archive while explicitly skipping execution; it still verifies
architecture, manifest, and every recorded file. It does not claim runtime
validation.

## Publishing

The `Standalone ARKit CLI` GitHub Actions workflow builds all six targets on
pushes to `main` that touch CLI inputs, on pull requests, manual dispatch, and
`arkit-v*` tags. Actions and Linux images are pinned by immutable
revisions/digests. A pinned actionlint validates the workflow before starting
the matrix. A failed platform prevents release assembly.

1. Update `cli/VERSION` and commit the application, tests, build tools, lock, and
   workflow. Release assembly requires a clean checkout.
2. Push to `main` to build and test all six platform archives continuously;
   inspect the workflow artifacts for checksums, manifests, and verification logs.
3. Push a matching tag, for example `arkit-v0.1.0`. The workflow builds and
   verifies the complete matrix, then creates a **draft** GitHub release.
4. Review the draft downloads and publish it when ready.

Manual workflow dispatch can also stage a draft when its matching tag already
exists. Published releases are not overwritten by the draft job. No release
is created by running local build or assembly commands.

Release assembly requires each requested platform exactly once, matching
version/source/runtime/toolchain pins, successful native smoke tests, the
declared compatibility baseline, and intact archive and file checksums.
It produces `SHA256SUMS`, `RELEASE_MANIFEST.json`, and release notes:

```bash
python3 cli/build_tools/release.py --artifacts cli/dist/arkit --output cli/dist/release-0.1.0 --version 0.1.0
```

## Signing

Default CI artifacts are unsigned on Windows and ad-hoc signed on macOS.
For trusted public downloads, use your publisher certificates and notarization
process; credentials stay outside the repository and artifacts.

- macOS: `build.py --codesign-identity "Developer ID Application: ..."`
  passes the identity to PyInstaller to sign the executable and native
  dependencies. The builder verifies the resulting signature. Submit the
  signed distribution to Apple's notarization service before public release.
- Windows: `build.py --windows-certificate CERTIFICATE_SHA1` signs and verifies
  the EXE/DLL/PYD files using `signtool` and a certificate already installed
  in the builder's certificate store. The Windows SDK signing tools must be
  on the builder's `PATH`.

Signing happens before inventory hashing, archive verification, and checksums.
Post-build changes require regenerating and re-verifying those records.
Default GitHub-hosted builds do not have signing credentials.
