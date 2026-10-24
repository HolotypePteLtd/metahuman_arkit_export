# MH ARKit Export

Two exporters for MetaHuman facial geometry with 52 ARKit morph targets:

| Product | Input | Runtime | Documentation |
| --- | --- | --- | --- |
| [Unreal plugin](plugins/ue) | MetaHuman project assets | Unreal Editor 5.7 | [Plugin guide](plugins/ue/Docs/README.md) |
| [Standalone CLI](cli) | Complete head DNA, optional body DNA | Bundled executable | [Builds and releases](cli/README.md), [baking guide](cli/docs/BAKING.md) |

```text
plugins/ue/                 Unreal plugin and its own tooling
  MHARKitExport.uplugin
  Source/                  Editor module
  Config/  Resources/  Docs/
  python/                  Python helper for Unreal Editor
  tools/  tests/  marketplace/
cli/                       Standalone DNA baker
  src/                     Baker, expression controls, CLI entry point
  tests/                   Baker and release-system tests
  build_tools/             Pinned tools, native builds, packaging, verification
  docs/
  VERSION  runtime.json  platforms.json  Makefile
.github/workflows/         Separate plugin and CLI workflows
Makefile                  Root build and test commands
```

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

Copy `plugins/ue` into `<Project>/Plugins/MHARKitExport`, build the project's
Editor target, enable **MH ARKit Export**, then open
**Tools > Export ARKit Blend Shapes**. The descriptor belongs inside the
plugin directory.

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

Extract the complete executable download for your operating system and CPU,
keeping the executable beside its `_internal` directory:

```bash
mh-arkit-export head.dna --body-dna body.dna --output character.glb
mh-arkit-export --self-test
```

Recipients do not need Python, Unreal, Blender, or a compiler. The optional
body is neutral geometry without a skeleton; materials and textures are not
exported. The CLI's GUI expression recipes have not been numerically matched
against Unreal's animation mapping assets.

### Baking a Poly Hammer export

[Poly Hammer](https://www.polyhammer.com) publishes the free
[Poly Hammer Interchange](https://docs.polyhammer.com/poly-hammer-interchange/getting-started/)
Unreal plugin, which exports a MetaHuman as a single folder containing its
DNA, grooms, clothing, props, textures, and an export manifest. The DNA files
in that folder are standard MetaHuman DNA, so the CLI can bake them directly.
Unreal, Blender, and the rest of the bundle are not required. Verified against
a real Interchange export: the folder held `head.dna`, `body.dna`, textures,
and `ExportManifest.json`, and the command below baked all 52 ARKit morph
targets plus the neutral body.

```bash
mh-arkit-export /path/to/bundle/head.dna --body-dna /path/to/bundle/body.dna -o character.glb
```

Use the actual DNA file names found in the bundle (typically `head.dna` and
`body.dna`); the manifest, grooms, clothing, and textures are not CLI inputs
and are not exported. If the character was customized in Blender with Poly
Hammer's Character DNA addon, bake the DNA file exported from that addon
instead.

Build a native CLI from source, using its isolated toolchain:

```bash
make cli JOBS=4
```

Linux publication artifacts use the pinned compatibility container:

```bash
make cli-docker PLATFORM=linux-x64 JOBS=4
```

CLI artifacts are written to `cli/dist/arkit`. Its release workflow builds
Linux, Windows, and macOS on x64 and ARM64, verifies the complete matrix,
and stages a draft release for `arkit-v*` tags. The CLI version lives in
`cli/VERSION`; the plugin version lives in `plugins/ue/MHARKitExport.uplugin`.
`JOBS`, `PLATFORM`, and `VERSION` are forwarded to the CLI build. `BUILD_ARGS`
passes additional native builder arguments; for a development-only Linux
package on a newer workstation, use `make cli BUILD_ARGS=--allow-newer-linux`.
The Linux publication build uses `make cli-docker` to enforce glibc 2.28.

## Tests

Run each product's checks independently:

```bash
make test
make plugin-test
make cli-test
```

The relevant Git history was extracted from an internal toolkit repository.
Filtered commit IDs differ because unrelated toolkit files were omitted.
Character DNA, baked models, Unreal build products, and other toolkit modules
are not included.
