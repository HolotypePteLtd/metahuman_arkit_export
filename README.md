# Export Unreal Engine MetaHuman with ARKit Blendshapes

A MetaHuman instance is described by DNA file(s) (`head.dna`, `body.dna`, etc), which is an Epic file format that specifies values for:

- **Geometry** — face and body meshes per LOD (nine LOD0 head parts on a
  typical character: face skin, teeth, eyeballs, eyelashes, ...)
- **Skeleton** — a joint hierarchy (870 joints on the validated head)
- **Controls and rig logic** — 174 GUI controls and 263 raw controls mapped by
  the rig-logic graph onto joints and 782 blendshape channels, including
  nonlinear correctives
- **Corrective shapes** — pose-space fixes that are applied before linear blend
  skinning

At runtime (either inside or outside Unreal Engine), Epic's open source Rig Logic library can be used to evaluate the graph efficiently for facial animation.

However, Unreal Engine (and other related plugins like Polyhammer) do not automatically export MetaHumans with ARKit-compatible blendshapes in their DNA descriptions.

This repository exists to fill this gap with two tools, both to "bake" a set of ARKit blendshapes onto an exported MetaHuman. The first is a UE plugin to export a glTF file from within UE itself, the second is to bake the blendshapes directly from/to the exported .dna files.  

## CLI

Download and extract the binary for your OS and CPU architecture from
[Releases](https://github.com/HolotypePteLtd/metahuman_arkit_export/releases).
Python, Unreal, Blender, and a compiler are not required.

### Bake to DNA

Choose an output filename ending in `.dna` to create a new DNA file containing
all 52 baked ARKit blendshapes. For example, on macOS or Linux:

```bash
./bake-arkit-dna head.dna --output head_arkit.dna
```

On Windows PowerShell:

```powershell
.\bake-arkit-dna.exe head.dna --output head_arkit.dna
```

The output preserves the LOD0 head meshes, neutral skeleton, skin weights,
UVs, and topology. Facial joint motion and corrective shapes are baked into
the blendshapes, and the original facial solvers are replaced with 52
pass-through controls. With RigLogic, drive a GUI control such as
`EyeBlinkLeft` or its raw control `CTRL_expressions.eyeBlinkLeft` to apply
that shape; the skeleton stays in its neutral pose.

The source `head.dna` is kept intact. The baker reloads and verifies the new
DNA before installing the output, and writes `head_arkit.report.json` beside
it with target statistics and verification results. Omit `--body-dna` for DNA
output; that option is supported only for GLB output.

### Bake to GLB

Use a `.glb` output filename to export geometry with ARKit morph targets,
optionally including the matching body in its neutral pose:

```bash
bake-arkit-dna head.dna --body-dna body.dna --output character.glb
```

The optional body is neutral geometry without a skeleton. Materials and
textures are not exported. The CLI's GUI expression recipes have not been
numerically matched against Unreal's animation mapping assets.

Run `bake-arkit-dna --self-test` to check the executable without supplying any
character files.

### Baking a Poly Hammer export

[Poly Hammer](https://www.polyhammer.com) publishes the free
[Poly Hammer Interchange](https://docs.polyhammer.com/poly-hammer-interchange/getting-started/)
Unreal plugin, which exports a MetaHuman as a single folder containing its
DNA, grooms, clothing, props, textures, and an export manifest. The DNA files
in that folder are standard MetaHuman DNA, so the CLI can bake them directly.
Unreal, Blender, and the rest of the bundle are not required. Verified against
a real Interchange export: the folder held `head.dna`, `body.dna`, textures,
and `ExportManifest.json`. To bake its head DNA to a new DNA file:

```bash
bake-arkit-dna /path/to/bundle/head.dna -o head_arkit.dna
```

To export GLB with all 52 ARKit morph targets plus the neutral body:

```bash
bake-arkit-dna /path/to/bundle/head.dna --body-dna /path/to/bundle/body.dna -o character.glb
```

Use the actual DNA file names found in the bundle (typically `head.dna` and
`body.dna`); the manifest, grooms, clothing, and textures are not CLI inputs
and are not exported. If the character was customized in Blender with Poly
Hammer's Character DNA addon, bake the DNA file exported from that addon
instead.



| --- | --- | --- | --- |
| [Unreal plugin](plugins/ue) | MetaHuman project assets | Unreal Editor 5.7 | [Plugin guide](plugins/ue/Docs/README.md) |
| [Standalone CLI](cli) | Complete head DNA, optional body DNA | Bundled executable | [CLI guide](cli/README.md), [baking guide](cli/docs/BAKING.md) |

```text
plugins/ue/                 Unreal plugin and its own tooling
  MHARKitExport.uplugin
  Source/                  Editor module
  Config/  Resources/  Docs/
  python/                  Python helper for Unreal Editor
  tools/  tests/  marketplace/
cli/                       Standalone DNA baker
  native/                  C++ baker, exporters, expression recipes, CLI
  src/                     Python reference implementation for parity tests
  tests/                   Baker and release-system tests
  build_tools/             Pinned tools, native builds, packaging, verification
  docs/
  VERSION  runtime.json  platforms.json  Makefile
.github/workflows/         Separate plugin and CLI workflows
Makefile                  Root build and test commands
BUILD.md                  Build, test, and release instructions
```

See [BUILD.md](BUILD.md) for build prerequisites, packaging, tests, and release instructions.


## Unreal plugin

Copy `plugins/ue` into `<Project>/Plugins/MHARKitExport`, build the project's
Editor target, enable **MH ARKit Export**, then open
**Tools > Export ARKit Blend Shapes**. The descriptor belongs inside the
plugin directory.

See [BUILD.md](BUILD.md#unreal-plugin) to package plugin sources or compile a plugin ZIP for your Unreal Engine installation.

See [BUILD.md](BUILD.md#standalone-cli) to build the CLI from source or prepare release artifacts.

## License

This repository's code — the exporters in `cli/` and `plugins/ue/` and their
build tools — is released under the MIT License: see [LICENSE](LICENSE).

That license covers only this repository's own code. It does not and cannot
relicense the third-party parts:

| Component | License | Notes |
| --- | --- | --- |
| OpenRigLogic + DNA libraries | [MIT](https://github.com/EpicGames/OpenRigLogic) (Epic Games) | Rig evaluation engine and DNA format tooling; pinned source revision in [cli/runtime.json](cli/runtime.json) |
| MetaHuman character assets and DNA files derived from them | [Epic's MetaHuman content terms](https://www.unrealengine.com/eula/metahuman) — not open source | Characters, textures, grooms, clothing, and exported DNA are governed by Epic's MetaHuman EULA, not MIT |
| nlohmann/json | [MIT](cli/native/vendor/json.LICENSE.MIT) | JSON parsing and serialization in the native CLI |

Third-party attribution lives in [cli/THIRD_PARTY_NOTICES.txt](cli/THIRD_PARTY_NOTICES.txt),
and every standalone CLI download ships the complete license text of each
bundled component in its `LICENSES/` directory.
