# Building and testing

Run commands from the repository root unless noted otherwise.

Run `make` or `make help` from the repository root to list the targets. GNU Make
is required for these shortcuts; the Python entry points also work directly.

| Command | Result |
| --- | --- |
| `make plugin` | Unreal plugin source ZIP |
| `make plugin-build UE_ROOT=/path/to/Engine` | Compiled Editor plugin ZIP for that engine and host |
| `make cli` | Standalone CLI for the current host |
| `make cli-docker PLATFORM=linux-x64` | Linux CLI publication artifact |
| `make test` | Both products' tests |
| `make plugin-test` / `make cli-test` | One product's tests |

## Unreal plugin

Package plugin sources for distribution:

```bash
make plugin
```

The ZIP is written to `plugins/ue/dist/MHARKitExport-0.2.1.zip`. Recipients
compile it with their Unreal toolchain. The plugin source workflow tests the
Unreal Python helper and uploads this source ZIP independently of the CLI.

Compile and package the plugin with a selected Unreal Engine installation:

```bash
make plugin-build UE_ROOT=/path/to/Engine
```

`UE_ROOT` accepts the `Engine` directory or its parent installation directory.
The builder uses AutomationTool with precompiled headers and unity builds
disabled, then writes a ZIP such as
`plugins/ue/dist/MHARKitExport-0.2.1-UE5.7.4-Linux.zip`. Each engine version and
host platform needs its own native build. The matching Unreal C++ compiler and
SDK must be installed. The Python equivalent is
`python3 plugins/ue/tools/build_plugin.py --engine /path/to/Engine`.

## Standalone CLI

The product is a C++17 executable with statically linked OpenRigLogic and DNA
libraries. It does not embed or launch Python. The release driver uses Python
to build the native product, compare it against the Python reference baker,
and create verified archives.

### Build directly with CMake

For a native executable without the Python reference test environment, install
CMake 3.24+ (4.2+ for Visual Studio 2026), Git, and the platform C++ toolchain.
Check out the pinned OpenRigLogic revision from `cli/runtime.json`:

```bash
git clone https://github.com/EpicGames/OpenRigLogic.git /path/to/OpenRigLogic
git -C /path/to/OpenRigLogic checkout d3d329bbd6992ac38adc1c1b24203ceb998f75c0
cmake -S cli -B /tmp/mh-arkit-cpp -DOPENRIGLOGIC_SOURCE=/path/to/OpenRigLogic -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/mh-arkit-cpp --config Release --target bake-arkit-dna --parallel 4
ctest --test-dir /tmp/mh-arkit-cpp -C Release --output-on-failure
```

The executable is `/tmp/mh-arkit-cpp/bake-arkit-dna`; Visual Studio puts
`bake-arkit-dna.exe` under the build directory's `Release` subdirectory.
On Windows, use a writable Windows path for the build directory and specify
`-A x64` or `-A ARM64` when configuring Visual Studio. On macOS, add
`-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0` to retain the published compatibility
baseline. Linux publication builds must use the compatibility container below.
The executable can be copied and run independently of its build directory.

### Verified release builds

Build a native CLI from source, using its isolated toolchain:

```bash
make cli JOBS=4
```

Linux publication artifacts use the pinned compatibility container:

```bash
make cli-docker PLATFORM=linux-x64 JOBS=4
```

CLI artifacts are written to `cli/dist/arkit`. Its release workflow builds
Linux, Windows, and macOS on x64 and ARM64 on every push to `main` and pull
request touching CLI inputs, verifies the complete matrix,
and stages a draft release for `arkit-v*` tags. The CLI version lives in
`cli/VERSION`; the plugin version lives in `plugins/ue/MHARKitExport.uplugin`.
`JOBS`, `PLATFORM`, and `VERSION` are forwarded to the CLI build. `BUILD_ARGS`
passes additional native builder arguments; for a development-only Linux
package on a newer workstation, use `make cli BUILD_ARGS=--allow-newer-linux`.
The Linux publication build uses `make cli-docker` to enforce glibc 2.28.

### Local builds

The build configuration lives under `cli/` and uses a pinned, isolated
release environment in a shared user cache. See [pinned release tools](cli/build_tools/README.md)
for cache locations and dependency pins. Run these commands from the repository root. The bootstrap needs
Python 3.9+ with venv/pip, then selects its own exact Python and package versions.

Windows needs Visual Studio 2022 or 2026 Build Tools with the matching x64 or ARM64
C++ toolset and Windows SDK. macOS needs Xcode command-line tools. Git is
required to fetch and verify the pinned OpenRigLogic source.

```bash
python3 cli/build_tools/run.py --prepare
python3 cli/build_tools/run.py python cli/build_tools/build.py --version 0.1.1
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
python3 cli/build_tools/docker_build.py --platform linux-x64 --version 0.1.1
python3 cli/build_tools/docker_build.py --platform linux-arm64 --version 0.1.1
```

The container contains the same locked release tools as local builds. Its
Docker context includes only `cli/build_tools`, so character files and other
workspace assets cannot be baked into the builder image. The source checkout
is mounted read-only when compiling. Cache and artifact directories are
separate writable mounts. Building the foreign Linux architecture locally
requires Docker's binfmt/QEMU support; CI uses native runners for both.

Artifacts appear under `cli/dist/arkit/`, named, for example:

```text
bake-arkit-dna-0.1.1-linux-x64.tar.gz
bake-arkit-dna-0.1.1-linux-x64.tar.gz.json
bake-arkit-dna-0.1.1-linux-x64.tar.gz.sha256
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

### Verification

Every package build compiles the C++ executable, runs its native self-test,
and compares its output with the Python reference baker. Tests cover geometry,
UVs, all 52 targets, custom GUI/raw mappings, baked DNA, and independent
execution without a runtime directory. The runtime self-test creates
a tiny synthetic DNA with a bone and corrective shape, bakes all 52 targets,
adds a neutral body, and checks the numerical GLB and baked DNA output. No Scott files or
other character assets are included in the test or release artifacts.

The builder verifies CPU architecture for all collected native files, records
a file-by-file SHA256 inventory, archives the bundle, extracts it into a new
directory containing spaces, and runs version, provenance, and bake probes.
Those probes run with empty `PATH` and no inherited Python or library paths.

Verify a finished archive independently:

```bash
python3 cli/build_tools/verify.py cli/dist/arkit/bake-arkit-dna-0.1.1-linux-x64.tar.gz
```

Run this on its matching platform. `--integrity-only` allows inspection of a
foreign archive while explicitly skipping execution; it still verifies
architecture, manifest, and every recorded file. It does not claim runtime
validation.

### Publishing

The `Standalone ARKit CLI` GitHub Actions workflow builds all six targets on
pushes to `main` that touch CLI inputs, on pull requests, manual dispatch, and
`arkit-v*` tags. Actions and Linux images are pinned by immutable
revisions/digests. A pinned actionlint validates the workflow before starting
the matrix. A failed platform prevents release assembly.

1. Update `cli/VERSION` and commit the application, tests, build tools, lock, and
   workflow. Release assembly requires a clean checkout.
2. Push to `main` to build and test all six platform archives continuously;
   inspect the workflow artifacts for checksums, manifests, and verification logs.
3. Push a matching tag, for example `arkit-v0.1.1`. The workflow builds and
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
python3 cli/build_tools/release.py --artifacts cli/dist/arkit --output cli/dist/release-0.1.1 --version 0.1.1
```

### Signing

Default CI artifacts are unsigned on Windows and ad-hoc signed on macOS.
For trusted public downloads, use your publisher certificates and notarization
process; credentials stay outside the repository and artifacts.

- macOS: `build.py --codesign-identity "Developer ID Application: ..."`
  signs the native executable using `codesign`. The builder verifies the resulting signature. Submit the
  signed distribution to Apple's notarization service before public release.
- Windows: `build.py --windows-certificate CERTIFICATE_SHA1` signs and verifies
  the EXE/DLL/PYD files using `signtool` and a certificate already installed
  in the builder's certificate store. The Windows SDK signing tools must be
  on the builder's `PATH`.

Signing happens before inventory hashing, archive verification, and checksums.
Post-build changes require regenerating and re-verifying those records.
Default GitHub-hosted builds do not have signing credentials.

## Tests

Run each product's checks independently:

```bash
make test
make plugin-test
make cli-test
```
