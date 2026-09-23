// Copyright 2026 Holotype Pte Ltd. All Rights Reserved.

#include "MHARKitExportBPLibrary.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/PoseAsset.h"
#include "Animation/SkeletalMeshActor.h"
#include "Curves/RichCurve.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "SkeletalRenderPublic.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "MeshUtilities.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

bool UMHARKitExportBPLibrary::DumpPoseAssetData(
	const FString& PoseAssetPath,
	const FString& OutputPath)
{
	UPoseAsset* PoseAsset = LoadObject<UPoseAsset>(nullptr, *PoseAssetPath);
	if (!PoseAsset)
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: Could not load PoseAsset: %s"), *PoseAssetPath);
		return false;
	}

	// Get pose names
	const TArray<FName>& PoseNames = PoseAsset->GetPoseFNames();
	int32 NumPoses = PoseAsset->GetNumPoses();
	UE_LOG(LogTemp, Log, TEXT("MHARKitExport: PoseAsset has %d poses"), NumPoses);

	// Get curve names (ctrl_expressions)
	const TArray<FAnimCurveBase>& CurveData = PoseAsset->GetCurveData();
	UE_LOG(LogTemp, Log, TEXT("MHARKitExport: PoseAsset has %d curves"), CurveData.Num());

	// Build JSON
	TSharedRef<FJsonObject> RootObj = MakeShared<FJsonObject>();
	RootObj->SetStringField("asset_path", PoseAssetPath);
	RootObj->SetNumberField("num_poses", PoseNames.Num());
	RootObj->SetNumberField("num_curves", CurveData.Num());

	// Curve names array
	TArray<TSharedPtr<FJsonValue>> CurveNamesJson;
	for (const FAnimCurveBase& Curve : CurveData)
	{
		CurveNamesJson.Add(MakeShared<FJsonValueString>(Curve.GetName().ToString()));
	}
	RootObj->SetArrayField("curve_names", CurveNamesJson);

	// Access the internal PoseContainer
	// PoseContainer is private but we can access poses through the public API
	// Each pose has CurveData array matching the Curves array
	TArray<TSharedPtr<FJsonValue>> PosesJson;

	for (int32 PoseIdx = 0; PoseIdx < PoseNames.Num(); ++PoseIdx)
	{
		TSharedRef<FJsonObject> PoseObj = MakeShared<FJsonObject>();
		PoseObj->SetStringField("name", PoseNames[PoseIdx].ToString());
		PoseObj->SetNumberField("index", PoseIdx);

		// Get curve values for this pose using GetAnimationCurveOnly
		// This is the proper way to get per-pose curve data
		TArray<FName> InCurveNames;
		TArray<float> InCurveValues;

		// Set input: this pose = 1.0, everything else = 0.0
		for (int32 i = 0; i < PoseNames.Num(); ++i)
		{
			InCurveNames.Add(PoseNames[i]);
			InCurveValues.Add(i == PoseIdx ? 1.0f : 0.0f);
		}

		TArray<FName> OutCurveNames;
		TArray<float> OutCurveValues;
		{
			// GetAnimationCurveOnly uses FBlendedCurve which requires FMemStack context
			FMemMark Mark(FMemStack::Get());
			PoseAsset->GetAnimationCurveOnly(InCurveNames, InCurveValues, OutCurveNames, OutCurveValues);
		}

		// Write non-zero curves
		TSharedRef<FJsonObject> CurvesObj = MakeShared<FJsonObject>();
		int32 NonZeroCount = 0;
		for (int32 i = 0; i < OutCurveNames.Num(); ++i)
		{
			if (FMath::Abs(OutCurveValues[i]) > 1e-6f)
			{
				CurvesObj->SetNumberField(OutCurveNames[i].ToString(), OutCurveValues[i]);
				NonZeroCount++;
			}
		}
		PoseObj->SetObjectField("curves", CurvesObj);
		PoseObj->SetNumberField("nonzero_curves", NonZeroCount);

		PosesJson.Add(MakeShared<FJsonValueObject>(PoseObj));

		UE_LOG(LogTemp, Log, TEXT("  Pose %d '%s': %d non-zero curves"),
			PoseIdx, *PoseNames[PoseIdx].ToString(), NonZeroCount);
	}

	RootObj->SetArrayField("poses", PosesJson);

	// Write JSON
	FString JsonStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
	FJsonSerializer::Serialize(RootObj, Writer);

	if (FFileHelper::SaveStringToFile(JsonStr, *OutputPath))
	{
		UE_LOG(LogTemp, Log, TEXT("MHARKitExport: Wrote PoseAsset data to %s"), *OutputPath);
		return true;
	}

	UE_LOG(LogTemp, Error, TEXT("MHARKitExport: Failed to write %s"), *OutputPath);
	return false;
}

bool UMHARKitExportBPLibrary::InitializeAnimBP(
	USkeletalMeshComponent* SkelComp,
	const FString& AnimBPPath)
{
	if (!SkelComp)
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport::InitializeAnimBP: null SkelComp"));
		return false;
	}

	if (SkelComp->GetAnimInstance())
	{
		UE_LOG(LogTemp, Log, TEXT("MHARKitExport: AnimInstance already exists"));
		return true;
	}

	UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AnimBPPath);
	if (!AnimBP || !AnimBP->GeneratedClass)
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: Could not load AnimBP: %s"), *AnimBPPath);
		return false;
	}

	SkelComp->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	SkelComp->SetAnimInstanceClass(AnimBP->GeneratedClass);
	SkelComp->InitializeAnimScriptInstance(true);

	UAnimInstance* Inst = SkelComp->GetAnimInstance();
	if (Inst)
	{
		UE_LOG(LogTemp, Log, TEXT("MHARKitExport: AnimInstance created: %s"), *Inst->GetClass()->GetName());
		return true;
	}

	UE_LOG(LogTemp, Error, TEXT("MHARKitExport: AnimInstance still null after init"));
	return false;
}

void UMHARKitExportBPLibrary::ForceAnimationEvaluation(USkeletalMeshComponent* SkelComp)
{
	if (!SkelComp)
	{
		return;
	}

	UAnimInstance* AnimInstance = SkelComp->GetAnimInstance();

	// Strategy 1: Tick animation, then re-inject curves, then refresh bones
	SkelComp->TickAnimation(0.0333f, false);

	// After TickAnimation, the AnimBP has evaluated and produced bone transforms.
	// The morph target overrides on the component should have been applied.
	// Now force bone refresh.
	SkelComp->RefreshBoneTransforms();
	SkelComp->MarkRenderDynamicDataDirty();
}

void UMHARKitExportBPLibrary::SetAnimCurveValue(
	USkeletalMeshComponent* SkelComp,
	FName CurveName,
	float Value)
{
	if (!SkelComp)
	{
		return;
	}

	// Strategy: Set BOTH morph target AND anim curve override.
	// Morph targets feed into the component tick pipeline.
	// Curve overrides feed into the AnimInstance evaluation.

	// 1. Set as morph target on the component
	SkelComp->SetMorphTarget(CurveName, Value);

	// 2. Also set on AnimInstance if available
	UAnimInstance* AnimInstance = SkelComp->GetAnimInstance();
	if (AnimInstance)
	{
		// OverrideCurveValue injects into attribute curves
		AnimInstance->OverrideCurveValue(CurveName, Value);

		// Also try setting it as a morph target curve via AnimInstance
		AnimInstance->SetMorphTarget(CurveName, Value);
	}

	UE_LOG(LogTemp, Verbose, TEXT("MHARKitExport: Set %s = %f (morph + curve)"),
		*CurveName.ToString(), Value);
}

TArray<FVector> UMHARKitExportBPLibrary::GetDeformedVertexPositions(
	USkeletalMeshComponent* SkelComp,
	int32 LODIndex)
{
	TArray<FVector> Positions;

	if (!SkelComp || !SkelComp->GetSkeletalMeshAsset())
	{
		return Positions;
	}

	ForceAnimationEvaluation(SkelComp);

	TArray<FFinalSkinVertex> SkinnedVertices;
	SkelComp->GetCPUSkinnedVertices(SkinnedVertices, LODIndex);

	Positions.Reserve(SkinnedVertices.Num());
	for (const FFinalSkinVertex& V : SkinnedVertices)
	{
		Positions.Add(FVector(V.Position));
	}

	UE_LOG(LogTemp, Log, TEXT("MHARKitExport: %d deformed verts at LOD %d"),
		Positions.Num(), LODIndex);

	return Positions;
}

UStaticMesh* UMHARKitExportBPLibrary::CapturePoseAsStaticMesh(
	USkeletalMeshComponent* SkelComp,
	const FString& AssetPath)
{
	if (!SkelComp)
	{
		return nullptr;
	}

	ForceAnimationEvaluation(SkelComp);

	TArray<UMeshComponent*> MeshComponents;
	MeshComponents.Add(SkelComp);

	IMeshUtilities& MeshUtil = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>("MeshUtilities");

	UStaticMesh* Result = MeshUtil.ConvertMeshesToStaticMesh(
		MeshComponents,
		SkelComp->GetComponentTransform(),
		AssetPath);

	if (Result)
	{
		UE_LOG(LogTemp, Log, TEXT("MHARKitExport: StaticMesh at %s (%d verts)"),
			*AssetPath, Result->GetNumVertices(0));
	}

	return Result;
}

TArray<FVector> UMHARKitExportBPLibrary::GetSkinnedVerticesNoEval(
	USkeletalMeshComponent* SkelComp,
	int32 LODIndex)
{
	TArray<FVector> Positions;
	if (!SkelComp || !SkelComp->GetSkeletalMeshAsset())
	{
		return Positions;
	}

	// NO ForceAnimationEvaluation — use current bone transforms + morph targets as-is
	TArray<FFinalSkinVertex> SkinnedVertices;
	SkelComp->GetCPUSkinnedVertices(SkinnedVertices, LODIndex);

	Positions.Reserve(SkinnedVertices.Num());
	for (const FFinalSkinVertex& V : SkinnedVertices)
	{
		Positions.Add(FVector(V.Position));
	}

	return Positions;
}

bool UMHARKitExportBPLibrary::ExportDeformedMeshOBJ(
	USkeletalMeshComponent* SkelComp,
	const FString& OutputPath,
	int32 LODIndex)
{
	if (!SkelComp || !SkelComp->GetSkeletalMeshAsset())
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: ExportDeformedMeshOBJ: invalid component"));
		return false;
	}

	ForceAnimationEvaluation(SkelComp);

	// Get CPU-skinned (deformed) vertices
	TArray<FFinalSkinVertex> SkinnedVertices;
	SkelComp->GetCPUSkinnedVertices(SkinnedVertices, LODIndex);
	int32 NumVerts = SkinnedVertices.Num();

	if (NumVerts == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: No skinned vertices at LOD %d"), LODIndex);
		return false;
	}

	// Get render data for UVs and triangle indices
	USkeletalMesh* Mesh = SkelComp->GetSkeletalMeshAsset();
	const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	if (!RenderData || LODIndex >= RenderData->LODRenderData.Num())
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: No render data for LOD %d"), LODIndex);
		return false;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];

	// Check UV availability
	const FStaticMeshVertexBuffer& UVBuffer = LODData.StaticVertexBuffers.StaticMeshVertexBuffer;
	bool bHasUVs = UVBuffer.GetNumTexCoords() > 0 && (int32)UVBuffer.GetNumVertices() == NumVerts;

	// Get index buffer as array
	TArray<uint32> Indices;
	LODData.MultiSizeIndexContainer.GetIndexBuffer(Indices);

	// Build OBJ
	FString OBJ;
	OBJ.Reserve(NumVerts * 80);
	OBJ += FString::Printf(TEXT("# %d vertices, LOD %d\n"), NumVerts, LODIndex);

	// Deformed positions
	for (const FFinalSkinVertex& V : SkinnedVertices)
	{
		OBJ += FString::Printf(TEXT("v %.6f %.6f %.6f\n"),
			V.Position.X, V.Position.Y, V.Position.Z);
	}

	// UVs from the static vertex buffer
	if (bHasUVs)
	{
		for (int32 i = 0; i < NumVerts; ++i)
		{
			FVector2f UV = UVBuffer.GetVertexUV(i, 0);
			OBJ += FString::Printf(TEXT("vt %.6f %.6f\n"), UV.X, 1.0f - UV.Y);
		}
	}

	// Faces per render section (material group)
	for (int32 SecIdx = 0; SecIdx < LODData.RenderSections.Num(); ++SecIdx)
	{
		const FSkelMeshRenderSection& Section = LODData.RenderSections[SecIdx];
		OBJ += FString::Printf(TEXT("g section_%d\n"), SecIdx);

		uint32 IdxStart = Section.BaseIndex;
		uint32 IdxEnd = IdxStart + Section.NumTriangles * 3;

		for (uint32 i = IdxStart; i < IdxEnd; i += 3)
		{
			uint32 I0 = Indices[i] + 1;      // OBJ is 1-indexed
			uint32 I1 = Indices[i + 1] + 1;
			uint32 I2 = Indices[i + 2] + 1;

			if (bHasUVs)
			{
				OBJ += FString::Printf(TEXT("f %u/%u %u/%u %u/%u\n"),
					I0, I0, I1, I1, I2, I2);
			}
			else
			{
				OBJ += FString::Printf(TEXT("f %u %u %u\n"), I0, I1, I2);
			}
		}
	}

	if (FFileHelper::SaveStringToFile(OBJ, *OutputPath))
	{
		UE_LOG(LogTemp, Log, TEXT("MHARKitExport: Wrote deformed OBJ (%d verts) to %s"),
			NumVerts, *OutputPath);
		return true;
	}

	UE_LOG(LogTemp, Error, TEXT("MHARKitExport: Failed to write %s"), *OutputPath);
	return false;
}

bool UMHARKitExportBPLibrary::ExportFaceMeshWeightsJSON(
	USkeletalMeshComponent* SkelComp,
	const FString& OutputPath,
	int32 LODIndex)
{
	if (!SkelComp || !SkelComp->GetSkeletalMeshAsset())
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: ExportFaceMeshWeightsJSON: invalid component"));
		return false;
	}

	USkeletalMesh* Mesh = SkelComp->GetSkeletalMeshAsset();
	const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	if (!RenderData || LODIndex >= RenderData->LODRenderData.Num())
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: No render data for LOD %d"), LODIndex);
		return false;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
	const FSkinWeightVertexBuffer& WeightBuffer = *LODData.GetSkinWeightVertexBuffer();
	const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
	const int32 NumVerts = LODData.GetNumVertices();
	const int32 MaxInfluences = WeightBuffer.GetMaxBoneInfluences();

	if (NumVerts == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: No vertices at LOD %d"), LODIndex);
		return false;
	}

	// Build a vertex -> render section lookup. SkinWeightVertexBuffer returns
	// SECTION-LOCAL bone indices (per FSkelMeshRenderSection::BoneMap), so we
	// resolve to global skeleton indices per section. Sections cover contiguous
	// vertex ranges [BaseVertexIndex, BaseVertexIndex + NumVertices).
	TArray<int32> VertexToSection;
	VertexToSection.Init(-1, NumVerts);
	for (int32 SecIdx = 0; SecIdx < LODData.RenderSections.Num(); ++SecIdx)
	{
		const FSkelMeshRenderSection& Section = LODData.RenderSections[SecIdx];
		const int32 V0 = (int32)Section.BaseVertexIndex;
		const int32 V1 = FMath::Min(V0 + (int32)Section.NumVertices, NumVerts);
		for (int32 v = V0; v < V1; ++v) { VertexToSection[v] = SecIdx; }
	}

	// Per vertex: resolve each influence to (global_bone_idx, weight).
	// Dedupe global bone indices into a sorted bones[] list for the JSON.
	// Padding slots (less-used vertices) are written as bone=-1, weight=0.
	TMap<int32, int32> GlobalBoneToLocal;     // global skeleton idx -> bones[] idx
	TArray<int32> BonesList;                  // bones[] -> global skeleton idx
	TArray<int32> AllIndices;                 // (NumVerts * MaxInfluences,) bones[]-idx, -1 = pad
	TArray<float> AllWeights;                 // (NumVerts * MaxInfluences,) [0, 1]
	AllIndices.SetNumUninitialized(NumVerts * MaxInfluences);
	AllWeights.SetNumUninitialized(NumVerts * MaxInfluences);

	for (int32 v = 0; v < NumVerts; ++v)
	{
		const int32 SecIdx = VertexToSection[v];
		const TArray<FBoneIndexType>& BoneMap =
			(SecIdx >= 0) ? LODData.RenderSections[SecIdx].BoneMap
			              : LODData.RenderSections[0].BoneMap;

		for (int32 k = 0; k < MaxInfluences; ++k)
		{
			const int32 Slot = v * MaxInfluences + k;
			const uint32 SectionBoneIdx = WeightBuffer.GetBoneIndex(v, k);
			const uint8 WeightU8 = WeightBuffer.GetBoneWeight(v, k);
			const float Weight = (float)WeightU8 / 255.0f;

			// UE pads influence slots with weight 0 + bone index 0; treat both
			// as "no influence" so the JSON sentinel (-1) is consistent.
			if (WeightU8 == 0)
			{
				AllIndices[Slot] = -1;
				AllWeights[Slot] = 0.0f;
				continue;
			}
			if (!BoneMap.IsValidIndex((int32)SectionBoneIdx))
			{
				AllIndices[Slot] = -1;
				AllWeights[Slot] = 0.0f;
				continue;
			}
			const int32 GlobalBoneIdx = (int32)BoneMap[SectionBoneIdx];
			int32* LocalPtr = GlobalBoneToLocal.Find(GlobalBoneIdx);
			int32 LocalIdx;
			if (LocalPtr) { LocalIdx = *LocalPtr; }
			else
			{
				LocalIdx = BonesList.Add(GlobalBoneIdx);
				GlobalBoneToLocal.Add(GlobalBoneIdx, LocalIdx);
			}
			AllIndices[Slot] = LocalIdx;
			AllWeights[Slot] = Weight;
		}
	}

	// Build the JSON header (small) via FJsonWriter; emit the bulk indices /
	// weights arrays as inlined text. The full thing through FJsonWriter would
	// allocate per number and balloon for 32k * 8 entries.
	TArray<TSharedPtr<FJsonValue>> BoneNamesJson;
	BoneNamesJson.Reserve(BonesList.Num());
	for (int32 GlobalIdx : BonesList)
	{
		const FName BoneName = RefSkeleton.GetBoneName(GlobalIdx);
		BoneNamesJson.Add(MakeShared<FJsonValueString>(BoneName.ToString()));
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField("n_verts", NumVerts);
	Root->SetNumberField("max_influences", MaxInfluences);
	Root->SetArrayField("bones", BoneNamesJson);

	FString HeaderStr;
	TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&HeaderStr);
	FJsonSerializer::Serialize(Root, Writer);

	// HeaderStr ends with the closing "}". Splice in the heavy arrays before it.
	int32 CloseIdx = HeaderStr.Find(TEXT("}"), ESearchCase::CaseSensitive,
	                                ESearchDir::FromEnd);
	if (CloseIdx == INDEX_NONE)
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: header JSON malformed"));
		return false;
	}
	FString OutStr = HeaderStr.Left(CloseIdx);

	auto EmitMatrix = [&](const TCHAR* Field, bool bIsFloat)
	{
		OutStr += FString::Printf(TEXT(",\"%s\":["), Field);
		for (int32 v = 0; v < NumVerts; ++v)
		{
			OutStr += TEXT("[");
			for (int32 k = 0; k < MaxInfluences; ++k)
			{
				if (k > 0) { OutStr += TEXT(","); }
				const int32 Slot = v * MaxInfluences + k;
				OutStr += bIsFloat
					? FString::Printf(TEXT("%.6g"), (double)AllWeights[Slot])
					: FString::Printf(TEXT("%d"),   AllIndices[Slot]);
			}
			OutStr += (v < NumVerts - 1) ? TEXT("],") : TEXT("]");
		}
		OutStr += TEXT("]");
	};
	EmitMatrix(TEXT("indices"), false);
	EmitMatrix(TEXT("weights"), true);
	OutStr += TEXT("}");

	if (!FFileHelper::SaveStringToFile(OutStr, *OutputPath))
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: Failed to write %s"), *OutputPath);
		return false;
	}

	UE_LOG(LogTemp, Log,
	       TEXT("MHARKitExport: Wrote weights JSON (%d verts, %d max influences, "
	            "%d unique bones) to %s"),
	       NumVerts, MaxInfluences, BonesList.Num(), *OutputPath);
	return true;
}

// ============================================================
// ARKit Blend Shape Baking
// ============================================================

namespace
{

const TCHAR* GARKitPoseNames[] = {
	TEXT("EyeBlinkLeft"), TEXT("EyeLookDownLeft"), TEXT("EyeLookInLeft"), TEXT("EyeLookOutLeft"),
	TEXT("EyeLookUpLeft"), TEXT("EyeSquintLeft"), TEXT("EyeWideLeft"),
	TEXT("EyeBlinkRight"), TEXT("EyeLookDownRight"), TEXT("EyeLookInRight"), TEXT("EyeLookOutRight"),
	TEXT("EyeLookUpRight"), TEXT("EyeSquintRight"), TEXT("EyeWideRight"),
	TEXT("JawForward"), TEXT("JawLeft"), TEXT("JawRight"), TEXT("JawOpen"),
	TEXT("MouthFunnel"), TEXT("MouthPucker"), TEXT("MouthLeft"), TEXT("MouthRight"),
	TEXT("MouthSmileLeft"), TEXT("MouthSmileRight"), TEXT("MouthFrownLeft"), TEXT("MouthFrownRight"),
	TEXT("MouthDimpleLeft"), TEXT("MouthDimpleRight"), TEXT("MouthStretchLeft"), TEXT("MouthStretchRight"),
	TEXT("MouthRollLower"), TEXT("MouthRollUpper"), TEXT("MouthShrugLower"), TEXT("MouthShrugUpper"),
	TEXT("MouthPressLeft"), TEXT("MouthPressRight"), TEXT("MouthLowerDownLeft"), TEXT("MouthLowerDownRight"),
	TEXT("MouthUpperUpLeft"), TEXT("MouthUpperUpRight"),
	TEXT("BrowDownLeft"), TEXT("BrowDownRight"), TEXT("BrowInnerUp"), TEXT("BrowOuterUpLeft"), TEXT("BrowOuterUpRight"),
	TEXT("CheekPuff"), TEXT("CheekSquintLeft"), TEXT("CheekSquintRight"),
	TEXT("NoseSneerLeft"), TEXT("NoseSneerRight"),
	TEXT("TongueOut"),
};
constexpr int32 GNumARKitPoses = UE_ARRAY_COUNT(GARKitPoseNames);

void MinMax3(const TArray<float>& Buf, float OutMin[3], float OutMax[3])
{
	OutMin[0] = OutMin[1] = OutMin[2] = FLT_MAX;
	OutMax[0] = OutMax[1] = OutMax[2] = -FLT_MAX;
	for (int32 i = 0; i < Buf.Num(); ++i)
	{
		int32 c = i % 3;
		if (Buf[i] < OutMin[c]) OutMin[c] = Buf[i];
		if (Buf[i] > OutMax[c]) OutMax[c] = Buf[i];
	}
}

USkeletalMesh* FindFaceMesh(const FString& CharacterName)
{
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FString SearchPath = FString::Printf(TEXT("/Game/MetaHumans/%s/Face"), *CharacterName);
	TArray<FAssetData> Assets;
	AR.GetAssetsByPath(FName(*SearchPath), Assets, true);

	for (const FAssetData& AD : Assets)
	{
		UObject* Asset = AD.GetAsset();
		if (USkeletalMesh* SM = Cast<USkeletalMesh>(Asset))
		{
			UE_LOG(LogTemp, Log, TEXT("MHARKitExport: Found face mesh: %s"), *SM->GetPathName());
			return SM;
		}
	}

	UE_LOG(LogTemp, Error, TEXT("MHARKitExport: No face SkeletalMesh under %s"), *SearchPath);
	return nullptr;
}

bool ReadTopology(USkeletalMesh* Mesh, int32 LODIndex,
	TArray<float>& OutUVs, TArray<uint32>& OutIndices, int32& OutNumVerts,
	bool bExcludeEyelashes = false)
{
	const FSkeletalMeshRenderData* RD = Mesh->GetResourceForRendering();
	if (!RD || LODIndex >= RD->LODRenderData.Num())
	{
		return false;
	}

	const FSkeletalMeshLODRenderData& LOD = RD->LODRenderData[LODIndex];
	const FStaticMeshVertexBuffer& UVBuf = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
	OutNumVerts = static_cast<int32>(UVBuf.GetNumVertices());

	// UVs — UE and glTF both use top-left origin, no flip needed
	OutUVs.Reset();
	OutUVs.Reserve(OutNumVerts * 2);
	for (uint32 i = 0; i < static_cast<uint32>(OutNumVerts); ++i)
	{
		FVector2f UV = UVBuf.GetVertexUV(i, 0);
		OutUVs.Add(UV.X);
		OutUVs.Add(UV.Y);
	}

	// Triangle indices from all render sections. When bExcludeEyelashes is
	// set (opt-in; pipeline use case), skip sections whose material slot name
	// contains "lashes" or "eyelash" — same filter MHExpressionEval applies
	// in its OBJ exporter. Eyelash vertices remain in position/UV/morph
	// arrays as orphans (valid glTF, preserves morph-target indexing).
	static const TCHAR* const SkipSubstrings[] = { TEXT("lashes"), TEXT("eyelash") };
	const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();

	TArray<uint32> AllIndices;
	LOD.MultiSizeIndexContainer.GetIndexBuffer(AllIndices);

	OutIndices.Reset();
	for (int32 SecIdx = 0; SecIdx < LOD.RenderSections.Num(); ++SecIdx)
	{
		const FSkelMeshRenderSection& Section = LOD.RenderSections[SecIdx];

		bool bSkip = false;
		if (bExcludeEyelashes)
		{
			FString SlotName;
			if (Materials.IsValidIndex(Section.MaterialIndex))
			{
				SlotName = Materials[Section.MaterialIndex].MaterialSlotName.ToString();
			}
			for (const TCHAR* Needle : SkipSubstrings)
			{
				if (SlotName.Contains(Needle, ESearchCase::IgnoreCase))
				{
					bSkip = true;
					break;
				}
			}
			if (bSkip)
			{
				UE_LOG(LogTemp, Log,
					TEXT("MHARKitExport: GLB skipping section %d (%s, %d tris) — eyelash filter"),
					SecIdx, *SlotName, Section.NumTriangles);
				continue;
			}
		}

		uint32 Start = Section.BaseIndex;
		uint32 End = Start + Section.NumTriangles * 3;
		for (uint32 i = Start; i < End; ++i)
		{
			OutIndices.Add(AllIndices[i]);
		}
	}

	return true;
}

bool WriteGLB(const FString& Path,
	const TArray<float>& Positions, const TArray<float>& UVs,
	const TArray<uint32>& Indices, int32 NumVerts,
	const TArray<FString>& MorphNames, const TArray<TArray<float>>& MorphDeltas)
{
	TArray<uint8> Blob;

	// Append data to blob (4-byte aligned), return offset
	auto Append = [&Blob](const void* Data, int32 Bytes) -> int32
	{
		int32 Off = Blob.Num();
		Blob.Append(static_cast<const uint8*>(Data), Bytes);
		while (Blob.Num() % 4) Blob.Add(0);
		return Off;
	};

	// Build JSON arrays incrementally
	FString BVJson, AccJson;
	int32 BVCount = 0, AccCount = 0;

	auto AddBV = [&](int32 Off, int32 Len, int32 Target) -> int32
	{
		int32 Idx = BVCount++;
		if (Idx > 0) BVJson += TEXT(",");
		BVJson += FString::Printf(TEXT("{\"buffer\":0,\"byteOffset\":%d,\"byteLength\":%d"), Off, Len);
		if (Target > 0) BVJson += FString::Printf(TEXT(",\"target\":%d"), Target);
		BVJson += TEXT("}");
		return Idx;
	};

	auto AddAcc = [&](int32 BV, int32 CT, int32 Count, const TCHAR* Type,
		const float* Min3 = nullptr, const float* Max3 = nullptr) -> int32
	{
		int32 Idx = AccCount++;
		if (Idx > 0) AccJson += TEXT(",");
		AccJson += FString::Printf(
			TEXT("{\"bufferView\":%d,\"componentType\":%d,\"count\":%d,\"type\":\"%s\""),
			BV, CT, Count, Type);
		if (Min3 && Max3)
		{
			AccJson += FString::Printf(
				TEXT(",\"min\":[%.8g,%.8g,%.8g],\"max\":[%.8g,%.8g,%.8g]"),
				Min3[0], Min3[1], Min3[2], Max3[0], Max3[1], Max3[2]);
		}
		AccJson += TEXT("}");
		return Idx;
	};

	// --- Base mesh ---
	float PosMin[3], PosMax[3];
	MinMax3(Positions, PosMin, PosMax);
	int32 PosOff = Append(Positions.GetData(), Positions.Num() * sizeof(float));
	int32 PosBV = AddBV(PosOff, Positions.Num() * sizeof(float), 34962);
	int32 PosAcc = AddAcc(PosBV, 5126, NumVerts, TEXT("VEC3"), PosMin, PosMax);

	int32 UVOff = Append(UVs.GetData(), UVs.Num() * sizeof(float));
	int32 UVBV = AddBV(UVOff, UVs.Num() * sizeof(float), 34962);
	int32 UVAcc = AddAcc(UVBV, 5126, NumVerts, TEXT("VEC2"));

	int32 IdxOff = Append(Indices.GetData(), Indices.Num() * sizeof(uint32));
	int32 IdxBV = AddBV(IdxOff, Indices.Num() * sizeof(uint32), 34963);
	int32 IdxAcc = AddAcc(IdxBV, 5125, Indices.Num(), TEXT("SCALAR"));

	// --- Morph targets ---
	FString TargetsJson, NamesJson;
	int32 TargetCount = 0;

	for (int32 i = 0; i < MorphNames.Num(); ++i)
	{
		float DMax = 0.0f;
		for (float V : MorphDeltas[i]) DMax = FMath::Max(DMax, FMath::Abs(V));
		if (DMax < 1e-6f) continue;

		float DMin[3], DMx[3];
		MinMax3(MorphDeltas[i], DMin, DMx);

		int32 DOff = Append(MorphDeltas[i].GetData(), MorphDeltas[i].Num() * sizeof(float));
		int32 DBV = AddBV(DOff, MorphDeltas[i].Num() * sizeof(float), 0);
		int32 DAcc = AddAcc(DBV, 5126, NumVerts, TEXT("VEC3"), DMin, DMx);

		if (TargetCount > 0) { TargetsJson += TEXT(","); NamesJson += TEXT(","); }
		TargetsJson += FString::Printf(TEXT("{\"POSITION\":%d}"), DAcc);
		NamesJson += FString::Printf(TEXT("\"%s\""), *MorphNames[i]);
		TargetCount++;
	}

	// --- Assemble JSON ---
	FString TF = TargetCount > 0
		? FString::Printf(TEXT(",\"targets\":[%s]"), *TargetsJson) : TEXT("");
	FString EF = TargetCount > 0
		? FString::Printf(TEXT(",\"extras\":{\"targetNames\":[%s]}"), *NamesJson) : TEXT("");

	FString Json = FString::Printf(TEXT(
		"{\"asset\":{\"version\":\"2.0\",\"generator\":\"MHARKitExport\"},"
		"\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
		"\"meshes\":[{\"primitives\":[{"
		"\"attributes\":{\"POSITION\":%d,\"TEXCOORD_0\":%d},\"indices\":%d%s"
		"}]%s}],"
		"\"buffers\":[{\"byteLength\":%d}],"
		"\"bufferViews\":[%s],"
		"\"accessors\":[%s]}"),
		PosAcc, UVAcc, IdxAcc, *TF, *EF, Blob.Num(), *BVJson, *AccJson);

	// UTF-8 conversion + 4-byte pad with spaces
	FTCHARToUTF8 Utf8(*Json);
	TArray<uint8> JsonBuf;
	JsonBuf.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	while (JsonBuf.Num() % 4) JsonBuf.Add(0x20);

	// --- Write GLB container ---
	int32 Total = 12 + 8 + JsonBuf.Num() + 8 + Blob.Num();
	TArray<uint8> GLB;
	GLB.Reserve(Total);

	auto W32 = [&GLB](uint32 V)
	{
		GLB.Append(reinterpret_cast<const uint8*>(&V), 4);
	};

	W32(0x46546C67); W32(2); W32(static_cast<uint32>(Total));  // GLB header
	W32(static_cast<uint32>(JsonBuf.Num())); W32(0x4E4F534A);  // JSON chunk
	GLB.Append(JsonBuf);
	W32(static_cast<uint32>(Blob.Num())); W32(0x004E4942);     // BIN chunk
	GLB.Append(Blob);

	return FFileHelper::SaveArrayToFile(GLB, *Path);
}

} // anonymous namespace


static bool BakeARKitShapeKeysForMesh(
	USkeletalMesh* FaceMesh,
	const FString& OutputPath,
	bool bExcludeEyelashes)
{
	// Load original AnimSequence (never modified)
	UAnimSequence* OrigSeq = LoadObject<UAnimSequence>(nullptr,
		TEXT("/Game/MetaHumans/Common/Face/ARKit/AS_MetaHuman_ARKit_Mapping"));
	if (!OrigSeq)
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: Could not load AS_MetaHuman_ARKit_Mapping"));
		return false;
	}

	// Duplicate into transient package — we inject MouthClose here without
	// modifying the user's original asset. GC cleans up when we're done.
	UAnimSequence* AnimSeq = DuplicateObject<UAnimSequence>(OrigSeq, GetTransientPackage());

	// Inject MouthClose curves (jawOpen=1 + lipsTogether=1) at frame 70
	{
		constexpr float MCFrameTime = 70.0f / 24.0f;
		constexpr float ExtendedLength = 71.0f / 24.0f;

		IAnimationDataController& Ctrl = AnimSeq->GetController();
		Ctrl.OpenBracket(FText::FromString(TEXT("InjectMouthClose")), false);
		Ctrl.SetNumberOfFrames(FFrameNumber(71), false);

		TSet<FName> MCKeepCurves;
		MCKeepCurves.Add(FName(TEXT("CTRL_expressions_jawOpen")));
		MCKeepCurves.Add(FName(TEXT("CTRL_expressions_mouthLipsTogetherUL")));
		MCKeepCurves.Add(FName(TEXT("CTRL_expressions_mouthLipsTogetherUR")));
		MCKeepCurves.Add(FName(TEXT("CTRL_expressions_mouthLipsTogetherDL")));
		MCKeepCurves.Add(FName(TEXT("CTRL_expressions_mouthLipsTogetherDR")));

		const IAnimationDataModel* Model = AnimSeq->GetDataModel();
		const TArray<FFloatCurve>& FloatCurves = Model->GetFloatCurves();

		int32 NumInjected = 0;
		for (const FFloatCurve& Curve : FloatCurves)
		{
			FAnimationCurveIdentifier CurveId(Curve.GetName(), ERawCurveTrackTypes::RCT_Float);
			float Value = MCKeepCurves.Contains(Curve.GetName()) ? 1.0f : 0.0f;
			FRichCurveKey Key(MCFrameTime, Value);
			Key.InterpMode = RCIM_Constant;
			Ctrl.SetCurveKey(CurveId, Key, false);
			NumInjected++;
		}

		Ctrl.CloseBracket(false);

		UE_LOG(LogTemp, Log, TEXT("MHARKitExport: Injected MouthClose at frame 70 (%d curves)"),
			NumInjected);
	}

	// Load PoseAsset to get authoritative pose names (frame order)
	UPoseAsset* PoseAsset = LoadObject<UPoseAsset>(nullptr,
		TEXT("/Game/MetaHumans/Common/Face/ARKit/PA_MetaHuman_ARKit_Mapping"));
	TArray<FString> PoseNames;
	if (PoseAsset)
	{
		const TArray<FName>& RawNames = PoseAsset->GetPoseFNames();
		for (int32 i = 0; i < RawNames.Num(); ++i)
		{
			FString Name = RawNames[i].ToString();
			if (Name != TEXT("Default"))
			{
				// The current Epic PoseAsset also contains Pose_4..Pose_17
				// samples after the 51 ARKit channels.  Those are frames from
				// the tongue animation tail, not additional ARKit blendshapes.
				// Exporting them used to turn a documented 52-target GLB into
				// a misleading 66-target file.
				if (PoseNames.Num() >= GNumARKitPoses)
				{
					break;
				}
				PoseNames.Add(Name);
			}
		}
		if (RawNames.Num() > GNumARKitPoses + 1)
		{
			UE_LOG(LogTemp, Log,
				TEXT("MHARKitExport: Ignoring %d non-ARKit tail poses"),
				RawNames.Num() - GNumARKitPoses - 1);
		}
		UE_LOG(LogTemp, Log, TEXT("MHARKitExport: %d pose names from PoseAsset"), PoseNames.Num());
	}
	else
	{
		// Fallback to hardcoded names
		UE_LOG(LogTemp, Warning, TEXT("MHARKitExport: PoseAsset not found, using hardcoded names"));
		for (int32 i = 0; i < GNumARKitPoses; ++i)
		{
			PoseNames.Add(GARKitPoseNames[i]);
		}
	}

	// Read mesh topology (UVs + face indices) directly from render data
	TArray<float> UVs;
	TArray<uint32> Indices;
	int32 TopoVerts = 0;
	if (!ReadTopology(FaceMesh, 0, UVs, Indices, TopoVerts, bExcludeEyelashes))
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: Failed to read mesh topology"));
		return false;
	}
	UE_LOG(LogTemp, Log, TEXT("MHARKitExport: Topology: %d verts, %d tris"),
		TopoVerts, Indices.Num() / 3);

	// Spawn temporary actor in the editor world
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: No editor world"));
		return false;
	}

	ASkeletalMeshActor* TempActor = World->SpawnActor<ASkeletalMeshActor>();
	if (!TempActor)
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: Failed to spawn actor"));
		return false;
	}

	auto Cleanup = [&]()
	{
		if (TempActor) { World->DestroyActor(TempActor); TempActor = nullptr; }
	};

	USkeletalMeshComponent* Comp = TempActor->GetSkeletalMeshComponent();
	Comp->SetSkeletalMeshAsset(FaceMesh);
	Comp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

	// MetaHuman Character editor preview meshes are transient and do not carry
	// the post-process AnimBP assigned to packaged Face meshes.  RigLogic lives
	// in that post-process instance, so evaluating the ARKit curve sequence
	// without it silently produces 52 zero deltas.  Install the stock Face
	// post-process blueprint as a component-local override when the mesh does
	// not provide one; the fitted DNA attached to the mesh remains authoritative
	// for the character identity.
	if (!FaceMesh->GetPostProcessAnimBlueprint())
	{
		UAnimBlueprint* FacePostProcessBP = LoadObject<UAnimBlueprint>(nullptr,
			TEXT("/Game/MetaHumans/Common/Face/ABP_Face_PostProcess"));
		if (!FacePostProcessBP || !FacePostProcessBP->GeneratedClass)
		{
			UE_LOG(LogTemp, Error,
				TEXT("MHARKitExport: Face mesh has no post-process AnimBP and the stock fallback could not be loaded"));
			Cleanup();
			return false;
		}
		TSubclassOf<UAnimInstance> FacePostProcessClass(
			FacePostProcessBP->GeneratedClass.Get());
		Comp->SetOverridePostProcessAnimBP(FacePostProcessClass, true);
		UE_LOG(LogTemp, Log, TEXT("MHARKitExport: Installed stock Face post-process AnimBP override"));
	}

	// Play AnimSequence → creates AnimSingleNodeInstance
	Comp->PlayAnimation(AnimSeq, false);
	UAnimSingleNodeInstance* Single = Cast<UAnimSingleNodeInstance>(Comp->GetAnimInstance());
	if (!Single)
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: No AnimSingleNodeInstance"));
		Cleanup();
		return false;
	}
	Single->SetPlaying(false);
	Single->SetPlayRate(0.0f);

	// Frame timing: each pose = one frame at 24fps.
	// Use fixed 1/24s per frame — NOT SeqLength/65, because the sequence may have
	// been extended (e.g. MouthClose at frame 70) which would stretch all positions.
	float SeqLength = AnimSeq->GetPlayLength();
	constexpr float OriginalFPS = 24.0f;
	constexpr float FrameDur = 1.0f / OriginalFPS;
	UE_LOG(LogTemp, Log, TEXT("MHARKitExport: Seq %.4fs, frameDur=%.6fs (fixed %gfps)"),
		SeqLength, FrameDur, OriginalFPS);

	UAnimInstance* PP = Comp->GetPostProcessInstance();
	UE_LOG(LogTemp, Log, TEXT("MHARKitExport: PostProcess ABP: %s"),
		PP ? *PP->GetClass()->GetName() : TEXT("NONE"));

	// Helper: capture deformed vertex positions in glTF coords (Y-up, meters)
	auto CapturePositions = [Comp](TArray<float>& Out) -> int32
	{
		Comp->TickAnimation(0.0333f, false);
		Comp->RefreshBoneTransforms();

		TArray<FFinalSkinVertex> Skinned;
		Comp->GetCPUSkinnedVertices(Skinned, 0);

		Out.Reset();
		Out.Reserve(Skinned.Num() * 3);
		for (const FFinalSkinVertex& V : Skinned)
		{
			Out.Add(V.Position.X / 100.0f);    // X → X
			Out.Add(V.Position.Z / 100.0f);    // Z → Y (up)
			Out.Add(-V.Position.Y / 100.0f);   // -Y → Z (back)
		}
		return Skinned.Num();
	};

	// Capture neutral (frame 0 = Default pose)
	Single->SetPosition(0.0f, false);
	TArray<float> Neutral;
	int32 NumVerts = CapturePositions(Neutral);
	if (NumVerts != TopoVerts)
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: Vert mismatch: %d skinned vs %d topology"),
			NumVerts, TopoVerts);
		Cleanup();
		return false;
	}
	UE_LOG(LogTemp, Log, TEXT("MHARKitExport: %d vertices"), NumVerts);

	// --- Capture ARKit expressions ---
	// Frame-seeking for geometry (proven working path), with a Left/Right name
	// swap to fix the systematic L/R flip between the PoseAsset pose order and
	// the AnimSequence frame order.
	int32 NumPoses = PoseNames.Num();
	TArray<FString> MorphNames;
	TArray<TArray<float>> MorphDeltas;

	for (int32 i = 0; i < NumPoses; ++i)
	{
		int32 FrameIdx = i + 1;  // Frame 0 = Default (neutral), Frame 1+ = expressions
		Single->SetPosition(FrameIdx * FrameDur, false);

		TArray<float> Posed;
		CapturePositions(Posed);

		TArray<float> Delta;
		Delta.SetNum(NumVerts * 3);
		float MaxD = 0.0f;
		for (int32 j = 0; j < NumVerts * 3; ++j)
		{
			Delta[j] = Posed[j] - Neutral[j];
			MaxD = FMath::Max(MaxD, FMath::Abs(Delta[j]));
		}

		// Fix systematic L/R flip: the AnimSequence has left-eye geometry at
		// the frame that maps to the PoseAsset's right pose, and vice versa.
		// Swap Left<->Right in the exported name.
		FString PoseName = PoseNames[i];
		if (PoseName.Contains(TEXT("Left")))
		{
			PoseName = PoseName.Replace(TEXT("Left"), TEXT("Right"));
		}
		else if (PoseName.Contains(TEXT("Right")))
		{
			PoseName = PoseName.Replace(TEXT("Right"), TEXT("Left"));
		}

		MorphNames.Add(PoseName);
		MorphDeltas.Add(MoveTemp(Delta));
		UE_LOG(LogTemp, Log, TEXT("  [%d/%d] %s: %.4f m"), i + 1, NumPoses,
			*PoseName, MaxD);
	}

	// MouthClose: frame 70 if extended, otherwise approximation
	{
		const float MCTime = 70.0f / 24.0f;
		bool bMouthCloseOk = false;

		if (SeqLength >= MCTime - 0.01f)
		{
			Single->SetPosition(MCTime, false);
			TArray<float> MCPosed;
			CapturePositions(MCPosed);

			// Subtract JawOpen (frame 18) to isolate lip closure
			Single->SetPosition(18 * FrameDur, false);
			TArray<float> JOPosed;
			CapturePositions(JOPosed);

			TArray<float> MCDelta;
			MCDelta.SetNum(NumVerts * 3);
			float MaxD = 0.0f;
			for (int32 j = 0; j < NumVerts * 3; ++j)
			{
				MCDelta[j] = MCPosed[j] - JOPosed[j];
				MaxD = FMath::Max(MaxD, FMath::Abs(MCDelta[j]));
			}

			if (MaxD > 1e-4f)
			{
				MorphNames.Add(TEXT("MouthClose"));
				MorphDeltas.Add(MoveTemp(MCDelta));
				bMouthCloseOk = true;
				UE_LOG(LogTemp, Log, TEXT("  [52/52] MouthClose: %.4f m"), MaxD);
			}
		}

		if (!bMouthCloseOk)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("MHARKitExport: MouthClose not at frame 70 — using approximation"));
			UE_LOG(LogTemp, Warning,
				TEXT("  To fix: set jawOpen=1 + lipsTogether=1 at frame 70 of AS_MetaHuman_ARKit_Mapping"));

			int32 PuckerIdx = MorphNames.IndexOfByKey(FString(TEXT("MouthPucker")));
			int32 FunnelIdx = MorphNames.IndexOfByKey(FString(TEXT("MouthFunnel")));
			if (PuckerIdx >= 0 && FunnelIdx >= 0)
			{
				TArray<float> MCDelta;
				MCDelta.SetNum(NumVerts * 3);
				float MaxD = 0.0f;
				for (int32 j = 0; j < NumVerts * 3; ++j)
				{
					MCDelta[j] = MorphDeltas[PuckerIdx][j] - 0.75f * MorphDeltas[FunnelIdx][j];
					MaxD = FMath::Max(MaxD, FMath::Abs(MCDelta[j]));
				}
				MorphNames.Add(TEXT("MouthClose"));
				MorphDeltas.Add(MoveTemp(MCDelta));
				UE_LOG(LogTemp, Log, TEXT("  [52/52] MouthClose (approx): %.4f m"), MaxD);
			}
		}
	}

	Cleanup();

	// A missing or non-functional RigLogic post-process used to produce a
	// syntactically valid GLB containing only zero morph targets.  Treat that as
	// a hard failure so callers cannot accidentally admit it into a dataset.
	int32 NonZero = 0;
	for (const TArray<float>& D : MorphDeltas)
	{
		float MaxV = 0.0f;
		for (float V : D) MaxV = FMath::Max(MaxV, FMath::Abs(V));
		if (MaxV > 1e-6f) NonZero++;
	}
	if (NonZero < 3)
	{
		UE_LOG(LogTemp, Error,
			TEXT("MHARKitExport: Only %d non-zero morph targets; refusing to write an invalid bake"),
			NonZero);
		return false;
	}

	// Ensure output directory exists
	FString Dir = FPaths::GetPath(OutputPath);
	if (!Dir.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*Dir, true);
	}

	// Write GLB
	UE_LOG(LogTemp, Log, TEXT("MHARKitExport: Writing %s..."), *OutputPath);
	if (!WriteGLB(OutputPath, Neutral, UVs, Indices, NumVerts, MorphNames, MorphDeltas))
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: Failed to write GLB"));
		return false;
	}

	int64 FileSize = IFileManager::Get().FileSize(*OutputPath);
	UE_LOG(LogTemp, Log, TEXT("MHARKitExport: Done! %.1f MB, %d verts, %d morph targets"),
		FileSize / (1024.0 * 1024.0), NumVerts, NonZero);
	return true;
}

bool UMHARKitExportBPLibrary::BakeARKitShapeKeys(
	const FString& CharacterName,
	const FString& OutputPath,
	bool bExcludeEyelashes)
{
	UE_LOG(LogTemp, Log, TEXT("MHARKitExport: BakeARKitShapeKeys('%s', '%s', excludeEyelashes=%s)"),
		*CharacterName, *OutputPath, bExcludeEyelashes ? TEXT("true") : TEXT("false"));

	USkeletalMesh* FaceMesh = FindFaceMesh(CharacterName);
	if (!FaceMesh)
	{
		return false;
	}
	return BakeARKitShapeKeysForMesh(FaceMesh, OutputPath, bExcludeEyelashes);
}

bool UMHARKitExportBPLibrary::BakeARKitShapeKeysFromMesh(
	USkeletalMesh* FaceMesh,
	const FString& OutputPath,
	bool bExcludeEyelashes)
{
	UE_LOG(LogTemp, Log,
		TEXT("MHARKitExport: BakeARKitShapeKeysFromMesh('%s', '%s', excludeEyelashes=%s)"),
		FaceMesh ? *FaceMesh->GetPathName() : TEXT("None"), *OutputPath,
		bExcludeEyelashes ? TEXT("true") : TEXT("false"));
	if (!FaceMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("MHARKitExport: FaceMesh is null"));
		return false;
	}
	return BakeARKitShapeKeysForMesh(FaceMesh, OutputPath, bExcludeEyelashes);
}
