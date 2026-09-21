// Copyright 2026 Holotype Pte Ltd. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MHARKitExportBPLibrary.generated.h"

static_assert(ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2),
	"MHARKitExport requires Unreal Engine 5.2 or later");

UCLASS()
class MHARKITEXPORT_API UMHARKitExportBPLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Dump PoseAsset data to a JSON file.
	 * For each pose, writes the pose name and all curve values (ctrl_expressions).
	 * This gives us the exact mapping: ARKit pose → ctrl_expressions values.
	 *
	 * @param PoseAssetPath   Asset path (e.g. "/Game/MetaHumans/Common/Face/ARKit/PA_MetaHuman_ARKit_Mapping")
	 * @param OutputPath      File path for JSON output
	 * @return                true on success
	 */
	UFUNCTION(BlueprintCallable, Category = "MHARKitExport")
	static bool DumpPoseAssetData(
		const FString& PoseAssetPath,
		const FString& OutputPath);

	// --- Existing functions (kept for future use) ---

	UFUNCTION(BlueprintCallable, Category = "MHARKitExport")
	static bool InitializeAnimBP(
		USkeletalMeshComponent* SkelComp,
		const FString& AnimBPPath = TEXT("/Game/MetaHumans/Common/Face/ABP_Face_PostProcess"));

	UFUNCTION(BlueprintCallable, Category = "MHARKitExport")
	static void ForceAnimationEvaluation(USkeletalMeshComponent* SkelComp);

	UFUNCTION(BlueprintCallable, Category = "MHARKitExport")
	static void SetAnimCurveValue(
		USkeletalMeshComponent* SkelComp,
		FName CurveName,
		float Value);

	UFUNCTION(BlueprintCallable, Category = "MHARKitExport")
	static TArray<FVector> GetDeformedVertexPositions(
		USkeletalMeshComponent* SkelComp,
		int32 LODIndex = 0);

	UFUNCTION(BlueprintCallable, Category = "MHARKitExport")
	static UStaticMesh* CapturePoseAsStaticMesh(
		USkeletalMeshComponent* SkelComp,
		const FString& AssetPath);

	/**
	 * Get CPU-skinned vertex positions WITHOUT forcing animation evaluation.
	 * Use after manually setting morph targets via SetMorphTarget —
	 * calling GetDeformedVertexPositions would re-evaluate and overwrite them.
	 */
	UFUNCTION(BlueprintCallable, Category = "MHARKitExport")
	static TArray<FVector> GetSkinnedVerticesNoEval(
		USkeletalMeshComponent* SkelComp,
		int32 LODIndex = 0);

	/**
	 * Export the current deformed mesh as an OBJ file.
	 * Uses GetCPUSkinnedVertices for positions, reads UVs and face indices
	 * directly from the SkeletalMesh render data. Produces correct vertex
	 * count matching the skeletal mesh vertex buffer (no GeometryScript bloat).
	 */
	UFUNCTION(BlueprintCallable, Category = "MHARKitExport")
	static bool ExportDeformedMeshOBJ(
		USkeletalMeshComponent* SkelComp,
		const FString& OutputPath,
		int32 LODIndex = 0);

	/**
	 * Dump per-vertex skin-weight influences for the face SkeletalMesh to JSON.
	 * For each LOD vertex, writes the (bone, weight) influences exactly as
	 * MetaHuman authored them — what shows up as vertex groups when a normal
	 * FBX export of this SkeletalMesh is imported into Blender, but without
	 * needing the FBX round-trip.
	 *
	 * Output JSON schema:
	 *   {
	 *     "n_verts":         int,
	 *     "max_influences":  int,
	 *     "bones":           [bone_name, ...]   // global skeleton bones referenced
	 *     "indices":         [[b0..bK], ...]    // (N, max_influences) int16 into bones
	 *     "weights":         [[w0..wK], ...]    // (N, max_influences) float32 [0, 1]
	 *   }
	 * Padding slots (vertex with fewer than max_influences) get bone -1 and weight 0.
	 * Weights are uint8 normalised by 255 (matches Blender's vertex-group weights
	 * after FBX import).
	 *
	 * Pair with ExportDeformedMeshOBJ on the SAME SkelComp + LODIndex so vertex
	 * row indices in the OBJ and the JSON match 1:1.
	 */
	UFUNCTION(BlueprintCallable, Category = "MHARKitExport")
	static bool ExportFaceMeshWeightsJSON(
		USkeletalMeshComponent* SkelComp,
		const FString& OutputPath,
		int32 LODIndex = 0);

	/**
	 * Bake all 52 ARKit blend shapes from a MetaHuman face mesh into a GLB file
	 * with morph targets. Spawns a temporary actor, plays AS_MetaHuman_ARKit_Mapping,
	 * evaluates each expression through the full animation pipeline (ControlRig + RigLogic),
	 * captures deformed vertices, and writes a single GLB.
	 *
	 * For MouthClose: if AS_MetaHuman_ARKit_Mapping has jawOpen+lipsTogether at frame 70,
	 * it will be captured. Otherwise falls back to an approximation.
	 *
	 * @param CharacterName     MetaHuman character name (e.g. "Ada")
	 * @param OutputPath        Full path for the output .glb file
	 * @param bExcludeEyelashes If true, drop triangles from render sections whose material
	 *                          slot name contains "lashes" or "eyelash". Vertices are kept
	 *                          (orphaned) so morph-target indexing stays aligned. Default
	 *                          is false — full mesh is exported.
	 * @return                  true on success
	 */
	UFUNCTION(BlueprintCallable, Category = "MHARKitExport")
	static bool BakeARKitShapeKeys(
		const FString& CharacterName,
		const FString& OutputPath,
		bool bExcludeEyelashes = false);

	/**
	 * Bake ARKit shapes from a specific face SkeletalMesh. This is equivalent
	 * to BakeARKitShapeKeys, but accepts editor-owned/transient preview meshes
	 * produced while a MetaHuman Character is open for editing. It avoids a
	 * full MetaHuman package/material build when only native face geometry is
	 * needed.
	 *
	 * The caller must keep FaceMesh alive for the duration of the call.
	 */
	UFUNCTION(BlueprintCallable, Category = "MHARKitExport")
	static bool BakeARKitShapeKeysFromMesh(
		USkeletalMesh* FaceMesh,
		const FString& OutputPath,
		bool bExcludeEyelashes = false);
};
