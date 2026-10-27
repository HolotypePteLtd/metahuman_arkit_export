# bake-arkit-dna releases

The C++ DNA baker builds into a single native command-line executable.
Each download contains `bake-arkit-dna` (or `.exe`), usage instructions,
dependency licenses, and a manifest. The executable runs on its own, without
an `_internal` directory or temporary extraction of runtime libraries. Users
extract the complete archive and run the executable; Python, Unreal, Blender,
and compilers are not required on their machines.

```text
bake-arkit-dna head.dna --body-dna body.dna -o character.glb
bake-arkit-dna head.dna -o character_arkit.dna
bake-arkit-dna --self-test
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
versions and library references are checked before packaging. Windows uses
Visual Studio 2022 or 2026 with the matching target architecture and a static
MSVC runtime. Linux builds
inspect the actual GLIBC requirements of every bundled ELF file and reject
artifacts exceeding 2.28. A successful build on a newer workstation alone
does not establish the Linux compatibility target.

The default builder uses portable scalar OpenRigLogic code, without AVX,
SSE, NEON, half-float, or host-specific CPU flags. MSVC builds statically
link the C++ runtime. OpenRigLogic and DNA libraries are linked statically.
Only platform system libraries remain as runtime dependencies.
Compilers, SDKs, and build tools are not included in the downloads.

## Building and publishing

See [BUILD.md](../BUILD.md#standalone-cli) for prerequisites, local and Docker
builds, verification, publishing, and signing. See [Tests](../BUILD.md#tests)
for the test commands.
