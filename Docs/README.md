# MH ARKit Export - Documentation

Export MetaHuman face meshes with all 52 ARKit blend shape morph targets as a single glTF Binary (GLB) file.

## Overview

MH ARKit Export evaluates each of the 52 ARKit facial expressions through the full MetaHuman animation pipeline (ControlRig + RigLogic), captures the deformed vertex positions, and writes a GLB file containing the base mesh and all 52 morph targets. The output is compatible with any application that supports glTF 2.0 morph targets (Blender, Unity, three.js, ARKit-based apps, etc.).

## Requirements

- Unreal Engine 5.2 or later (5.7 recommended)
- A project with at least one MetaHuman character imported via the MetaHuman plugin
- The standard MetaHuman common assets must be present:
  - `AS_MetaHuman_ARKit_Mapping` (AnimSequence)
  - `PA_MetaHuman_ARKit_Mapping` (PoseAsset)

## Installation

1. Copy the `MHARKitExport` folder into your project's `Plugins/` directory.
2. Restart the Unreal Editor.
3. If prompted, confirm that you want to enable the plugin.
4. Verify the plugin is active under **Edit > Plugins**, searching for "MH ARKit Export".

## Usage - Editor UI

1. Open the exporter window from the menu: **Tools > Export ARKit Blend Shapes**.
2. Select a MetaHuman character from the dropdown. The plugin auto-discovers characters under `/Game/MetaHumans/`.
3. Set the output path for the `.glb` file (or use the **Browse...** button).
4. Click **Export ARKit Blend Shapes**.
5. Monitor progress in the log panel at the bottom of the window. On success, the file path and size are displayed.

## Usage - Blueprint / C++ API

The plugin exposes all functionality through `UMHARKitExportBPLibrary`, a `BlueprintFunctionLibrary` callable from Blueprints or C++.

### BakeARKitShapeKeys

The primary function. Bakes all 52 ARKit blend shapes into a single GLB file.

```
bool BakeARKitShapeKeys(
    FString CharacterName,
    FString OutputPath,
    bool bExcludeEyelashes = false)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `CharacterName` | `FString` | MetaHuman character name, e.g. `"Ada"` |
| `OutputPath` | `FString` | Full file path for the output `.glb` file |
| `bExcludeEyelashes` | `bool` | Exclude eyelash triangles while preserving vertex indexing |
| **Returns** | `bool` | `true` on success |

**Example (Blueprint):** Call `BakeARKitShapeKeys` with `CharacterName = "Ada"` and `OutputPath = "C:/Export/Ada_arkit.glb"`.

### BakeARKitShapeKeysFromMesh

Bakes a specific `USkeletalMesh`, including editor-owned transient face meshes
created while a MetaHuman Character is open for editing. This avoids requiring
a packaged character asset when only fitted LOD0 geometry and native ARKit
deformations are needed. If the transient mesh has no post-process animation
blueprint, the exporter installs the stock MetaHuman Face post-process
blueprint on its temporary component so RigLogic still evaluates.

```
bool BakeARKitShapeKeysFromMesh(
    USkeletalMesh* FaceMesh,
    FString OutputPath,
    bool bExcludeEyelashes = false)
```

The caller must keep `FaceMesh` alive for the duration of the synchronous call.
The exporter fails instead of writing a GLB when animation evaluation produces
fewer than three non-zero targets; this prevents an unrigged preview mesh from
silently becoming an all-neutral dataset sample.

### DumpPoseAssetData

Exports the PoseAsset curve mapping to a JSON file. Useful for debugging or understanding the ARKit-to-curve mapping.

```
bool DumpPoseAssetData(FString PoseAssetPath, FString OutputPath)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `PoseAssetPath` | `FString` | Asset path, e.g. `"/Game/MetaHumans/Common/Face/ARKit/PA_MetaHuman_ARKit_Mapping"` |
| `OutputPath` | `FString` | File path for JSON output |
| **Returns** | `bool` | `true` on success |

### InitializeAnimBP

Sets up an Animation Blueprint on a skeletal mesh component.

```
bool InitializeAnimBP(USkeletalMeshComponent* SkelComp, FString AnimBPPath)
```

| Parameter | Type | Default |
|-----------|------|---------|
| `SkelComp` | `USkeletalMeshComponent*` | Required |
| `AnimBPPath` | `FString` | `"/Game/MetaHumans/Common/Face/ABP_Face_PostProcess"` |

### ForceAnimationEvaluation

Ticks animation and refreshes bone transforms on a skeletal mesh component.

```
void ForceAnimationEvaluation(USkeletalMeshComponent* SkelComp)
```

### SetAnimCurveValue

Sets an animation curve value and corresponding morph target on a skeletal mesh component.

```
void SetAnimCurveValue(USkeletalMeshComponent* SkelComp, FName CurveName, float Value)
```

### GetDeformedVertexPositions

Returns CPU-skinned vertex positions after forcing animation evaluation.

```
TArray<FVector> GetDeformedVertexPositions(USkeletalMeshComponent* SkelComp, int32 LODIndex = 0)
```

### GetSkinnedVerticesNoEval

Returns CPU-skinned vertex positions without re-evaluating animation. Use after manually setting morph targets.

```
TArray<FVector> GetSkinnedVerticesNoEval(USkeletalMeshComponent* SkelComp, int32 LODIndex = 0)
```

### ExportDeformedMeshOBJ

Exports the current deformed mesh as an OBJ file.

```
bool ExportDeformedMeshOBJ(USkeletalMeshComponent* SkelComp, FString OutputPath, int32 LODIndex = 0)
```

### CapturePoseAsStaticMesh

Converts the current deformed skeletal mesh pose into a `UStaticMesh` asset.

```
UStaticMesh* CapturePoseAsStaticMesh(USkeletalMeshComponent* SkelComp, FString AssetPath)
```

## Output Format

The exported `.glb` file is a glTF 2.0 binary container containing:

- **Base mesh:** The neutral face geometry with positions, UVs, and triangle indices
- **52 morph targets:** One per ARKit blend shape, stored as vertex position deltas
- **Morph target names:** Stored in the mesh `extras` field for applications that support named morph targets

Coordinate system conversion is applied automatically:
- Unreal (cm, left-handed) to glTF (meters, right-handed)
- Conversion: `X_glTF = X_UE/100`, `Y_glTF = Z_UE/100`, `Z_glTF = -Y_UE/100`

### ARKit Blend Shape List

The 52 morph targets correspond to the standard Apple ARKit blend shapes:

EyeBlinkLeft, EyeLookDownLeft, EyeLookInLeft, EyeLookOutLeft, EyeLookUpLeft, EyeSquintLeft, EyeWideLeft, EyeBlinkRight, EyeLookDownRight, EyeLookInRight, EyeLookOutRight, EyeLookUpRight, EyeSquintRight, EyeWideRight, JawForward, JawLeft, JawRight, JawOpen, MouthFunnel, MouthPucker, MouthLeft, MouthRight, MouthSmileLeft, MouthSmileRight, MouthFrownLeft, MouthFrownRight, MouthDimpleLeft, MouthDimpleRight, MouthStretchLeft, MouthStretchRight, MouthRollLower, MouthRollUpper, MouthShrugLower, MouthShrugUpper, MouthPressLeft, MouthPressRight, MouthLowerDownLeft, MouthLowerDownRight, MouthUpperUpLeft, MouthUpperUpRight, MouthClose, BrowDownLeft, BrowDownRight, BrowInnerUp, BrowOuterUpLeft, BrowOuterUpRight, CheekPuff, CheekSquintLeft, CheekSquintRight, NoseSneerLeft, NoseSneerRight, TongueOut

## Troubleshooting

**No characters appear in the dropdown**
- Ensure MetaHuman characters are imported under `/Game/MetaHumans/<CharacterName>/`.
- Each character must have a `Face/` subfolder containing a SkeletalMesh asset.

**Export fails immediately**
- Check the Output Log (Window > Developer Tools > Output Log) for detailed error messages.
- Verify that `AS_MetaHuman_ARKit_Mapping` and `PA_MetaHuman_ARKit_Mapping` exist under `/Game/MetaHumans/Common/Face/ARKit/`.

**GLB file is empty or has no morph targets**
- This may occur if the MetaHuman animation pipeline assets are missing or incompatible.
- Ensure you are using the standard Epic MetaHuman assets (not custom replacements).

**Large file size**
- The GLB includes full vertex deltas for all 52 expressions. File sizes of 20-50 MB are typical for LOD 0 MetaHuman face meshes.

## Support

For issues and feature requests, contact Holotype Pte Ltd.
