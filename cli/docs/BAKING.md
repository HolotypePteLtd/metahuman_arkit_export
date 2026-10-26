# Offline ARKit baking from head DNA

`cli/src/bake_arkit_dna.py` evaluates a complete MetaHuman head DNA using Epic's
standalone OpenRigLogic Python bindings. Unreal Engine and Blender are not
required to run the baker.

For standalone executable downloads and cross-platform publication, see the
[release build system](../README.md). Those bundles include Python,
NumPy, and the compiled bindings, so recipients need none of these installed.

The baker loads LOD0, evaluates joints and corrective blendshape weights,
applies the corrective deltas before linear blend skinning, and writes the
neutral geometry plus 52 ARKit morph targets to a GLB. It exports all LOD0
face parts as separate mesh nodes, each with the same target names and order.
UV seams are preserved by expanding DNA vertex layouts. Coordinate conversion
and winding conversion use the native DNA reader; the GLB uses metres and
right/up/back axes.

The output extension selects GLB or baked DNA. DNA output keeps the neutral
head skeleton, skin weights, original polygon topology, normals, and UV
layouts at LOD0. It replaces the source expression solvers with 52 independent
blendshape channels named exactly as `TARGET_NAMES`, from `EyeBlinkLeft` to
`MouthClose`. GUI controls use those same names; raw controls use
`CTRL_expressions.eyeBlinkLeft`, etc. Each control passes directly to its
matching blendshape channel. There are no animated joint groups: source
joint motion is already included in the baked vertex deltas.

The writer reloads every baked DNA through RigLogic and checks the neutral
pose, all 52 controls individually, and three deterministic mixed poses
against the evaluated bake, with a maximum allowed vertex error of 0.002 mm.
It installs the output file only after verification passes. The report's
`baked_dna.verification` records the measured error. This validates the baked
linear morph model, including skinning; it does not establish parity with
Unreal's mapping assets or nonlinear combinations of the original live rig.

The GLB contains geometry and UVs. Materials, textures, and wrinkle maps are
not included. Use `--body-dna` to include the complete LOD0 body geometry in
its evaluated neutral pose. The body is a static mesh; skeletal animation
and skin weights are not exported. Apply the same expression weights to every
exported face mesh node. Zero targets on individual face parts are expected, such as a smile on
an eyeball; the report identifies targets that are zero across all selected
parts.

## Build the runtime

The repository's pinned builder prepares Python, NumPy, CMake, and SWIG and
prints the installed bindings directory:

```bash
python3 cli/build_tools/run.py python cli/build_tools/build.py \
  --native-only --build-dir /tmp/mh-arkit-native --jobs 8
```

For a manual upstream build, use the commands below. On macOS, the pinned
builder also handles the upstream SWIG library suffix and runtime search path
needed for Python imports.

Install a C++ compiler, CMake, SWIG, NumPy, and Python development headers.
The Python major/minor version used to build the bindings must match the
interpreter running the baker. The example below uses Python 3.13.

```bash
git clone --branch 5.8 https://github.com/EpicGames/OpenRigLogic.git /tmp/OpenRigLogic
cmake -S /tmp/OpenRigLogic -B /tmp/OpenRigLogic-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DRL_BUILD_PYTHON_WRAPPER=3.13 \
  -DRL_BUILD_TESTS=OFF \
  -DRL_BUILD_BENCHMARKS=OFF \
  -DRL_BUILD_EXAMPLES=OFF \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_INSTALL_PREFIX=/tmp/OpenRigLogic-runtime
cmake --build /tmp/OpenRigLogic-build --parallel 4
cmake --install /tmp/OpenRigLogic-build
```

This also works without Blender on macOS. Compile the bindings for the Mac's
architecture and Python version; Linux `.so` files cannot be used on macOS.
The Scott validation used OpenRigLogic commit
`d3d329bbd6992ac38adc1c1b24203ceb998f75c0` (runtime version 13.2.9), built for
Linux x86_64 / Python 3.13. Set the checkout to that commit to reproduce the
same runtime source.

## Bake

From the repository root:

```bash
python3 cli/src/bake_arkit_dna.py /path/to/Scott/head.dna \
  --bindings /tmp/OpenRigLogic-runtime/lib \
  --output /tmp/Scott_arkit.glb
```

`--bindings` points at the directory containing `dna.py`, `riglogic.py`, and
their native libraries. Existing compatible bindings on `PYTHONPATH` work too.
`--mesh head_lod0_mesh` selects just the face skin; repeat `--mesh` to select
multiple parts. The default includes the teeth, eyeballs, eyelashes, and other
LOD0 head parts so jaw, gaze, and tongue motion are captured.

Write a baked DNA instead:

```bash
python3 cli/src/bake_arkit_dna.py /path/to/Scott/head.dna \
  --bindings /tmp/OpenRigLogic-runtime/lib \
  --output /tmp/Scott_arkit.dna
```

`--mesh` works for both output formats. Baked DNA is head-only and has one
LOD; `--body-dna` requires GLB. Source DNA files cannot be used as the output
path. Choose different stems when keeping both formats and their reports,
since each report uses the output stem followed by `.report.json`.

The pinned macOS builder supplies importable `.so` copies of upstream's SWIG
`.dylib` modules and sets their runtime search path to `@loader_path`. Use
`cli/build_tools/build.py --native-only` through the pinned tool launcher to
obtain a directly importable runtime on macOS.

With that runtime's matching Python and bindings on `PYTHONPATH`, reproduce
the writer fidelity check and compare the two output formats:

```bash
python cli/build_tools/roundtrip_dna.py /path/to/Scott/head.dna
python cli/build_tools/compare_dna_glb.py /tmp/Scott_arkit.dna /tmp/Scott_reference.glb
```

The round-trip uses the unmodified reader configuration and the native
`BinaryStreamWriter.setFrom(reader)` overload, preserving all LODs, behavior,
and source coordinate conventions. Scott's format 2.5 input round-trips byte
for byte (SHA-256 `378fbcc29daf795bd38283675d299fca6fc178be6f79df4d25e81f3ce10b4276`).
The bake uses the transformed LOD0 reader and copies geometry/definition
layers before replacing controls and behavior. DNA's neutral joint rotation
getter exposes Euler angles; RigLogic's quaternion output is a different API.

Include the body from the same character export:

```bash
python3 cli/src/bake_arkit_dna.py /path/to/Scott/head.dna \
  --body-dna /path/to/Scott/body.dna \
  --bindings /tmp/OpenRigLogic-runtime/lib \
  --output /tmp/Scott_arkit_full_body.glb
```

Body parts are separate mesh nodes in the same scene. They retain their
original alignment with the head after the shared coordinate conversion,
without adding facial targets to the body. `--mesh` filters only head parts.

The adjacent `.report.json` records the source hash, original DNA format,
adapted reader format, runtime version, mesh counts, control recipes, and
maximum displacement for every target. Native loading can upgrade a DNA's
format in memory; this does not modify the input file.

## Expression mapping and Unreal parity

The default recipes use the dedicated DNA GUI control mapping in `cli/src/arkit_controls.py`,
with two local corrections:

- `MouthClose` is the evaluated jaw-open-with-lips-together pose minus the
  evaluated jaw-open pose, matching the plugin's additive construction.
- `NoseSneerLeft/Right` use the primary `CTRL_L/R_nose.ty` controls. The old
  upper-wrinkle-only recipes produce no deformation on Scott's DNA.

DNA GUI control names are used directly for left and right. The plugin's
frame-to-PoseAsset naming swap is specific to its Unreal animation assets and
is not applied to these GUI recipes. The DNA reader converts coordinate
conventions independently of expression naming.

This reproduces the plugin's *baking method*. The built-in GUI recipes have
not been compared against `AS_MetaHuman_ARKit_Mapping`, so numerical parity
with an Unreal bake is not established. The report explicitly records this.
For exact recipe matching, provide a mapping exported from the corresponding
Unreal assets using `--mapping /path/to/mapping.json`. It must include exactly
the 52 capitalized names in `TARGET_NAMES` and has this structure:

```json
{
  "control_space": "raw",
  "targets": {
    "JawOpen": {
      "controls": {"CTRL_expressions.jawOpen": 1.0}
    },
    "MouthClose": {
      "controls": {
        "CTRL_expressions.jawOpen": 1.0,
        "CTRL_expressions.mouthLipsTogetherUL": 1.0,
        "CTRL_expressions.mouthLipsTogetherUR": 1.0,
        "CTRL_expressions.mouthLipsTogetherDL": 1.0,
        "CTRL_expressions.mouthLipsTogetherDR": 1.0
      },
      "subtract": {"CTRL_expressions.jawOpen": 1.0}
    }
  }
}
```

The snippet shows only two targets; supply all 52 in a real mapping. Native
DNA raw control names generally use `CTRL_expressions.name`, whereas Unreal
curve names use `CTRL_expressions_name`. Use the exact names present in the
DNA. `control_space` can also be `gui`. Missing controls fail validation
instead of silently dropping part of a recipe. An omitted `subtract` samples
against the evaluated neutral pose.

As with the Unreal exporter, adding independently baked targets does not
preserve every nonlinear corrective interaction of the full live rig.

## Scott validation

The supplied archive's manifest identifies Unreal Engine 5.7.4, and its head
DNA uses format 2.5. It loaded successfully in OpenRigLogic and was adapted to
the reader's 2.8 representation.

- 870 joints, 174 GUI controls, 263 raw controls, 782 blendshape channels.
- Nine LOD0 meshes, 33,845 source positions, 64,094 triangles.
- 858 mesh corrective targets read and available for deformation.
- 52 nonzero ARKit targets across the full head; a roughly 22 MiB GLB.
- Blender import verified 52 named shape keys plus Basis on every mesh,
  along with the expected vertex counts and UV layers.
- macOS ARM64 baked DNA: nine LOD0 meshes, 870 neutral joints, 52 GUI/raw
  pass-through controls, and 52 blendshape channels; approximately 11.5 MiB.
- All 56 baked-DNA verification poses passed, with maximum vertex error
  `2.48e-9` metres against the evaluated bake and `5.93e-8` metres against the
  serialized GLB (which stores metre positions as float32).

With Scott's body included, the output has ten meshes and adds 30,455 body
positions and 60,816 triangles. The head and body each have 92 vertices at
their shared boundary; the maximum distance between matching boundary
vertices is approximately 0.000154 mm. The body is exported in its neutral
pose, and the facial targets remain on the nine head parts.

The host tests cover joint hierarchy traversal, parent-space translation,
correctives applied before skinning, the mouth-close difference, mapping
validation, UV-seam expansion with all 52 targets retained, and inclusion of
the body as geometry without facial morph targets:

```bash
python3 -m unittest discover -s cli/tests -p test_bake_arkit_dna.py -v
```

The full `cli/tests` suite additionally exercises native round-trip fidelity,
nonzero neutral joint rotations, multiple source LODs, mesh selection and
index remapping, UV/topology/skin preservation, zero targets, pass-through
controls, GLB comparison, and preservation of existing output when validation
fails. Native tests require the built bindings on `PYTHONPATH`; standalone
builds supply them automatically.
