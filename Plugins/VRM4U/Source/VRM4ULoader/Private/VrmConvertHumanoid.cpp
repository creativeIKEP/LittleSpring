// VRM4U Copyright (c) 2019 Haruyoshi Yamamoto. This software is released under the MIT License.

#include "VrmConvertHumanoid.h"
#include "VrmConvert.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/pbrmaterial.h>
#include <assimp/vrm/vrmmeta.h>

#include "VrmAssetListObject.h"
#include "VrmMetaObject.h"

#include "Engine/SkeletalMesh.h"
#include "RenderingThread.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Animation/MorphTarget.h"
#include "Animation/Skeleton.h"
#include "Animation/NodeMappingContainer.h"


static void renameToHumanoidBone(USkeletalMesh *targetSK, const UVrmMetaObject *meta, const USkeletalMesh *srcSK) {

	USkeleton *targetSkeleton = targetSK->Skeleton;
	UNodeMappingContainer * rig = targetSK->NodeMappingData[0];
	UNodeMappingContainer *src_rig = srcSK->NodeMappingData[0];

	if (meta == nullptr) {
		return;
	}
	//k->RemoveBonesFromSkeleton()
	auto &allbone = const_cast<TArray<FMeshBoneInfo> &>(targetSkeleton->GetReferenceSkeleton().GetRawRefBoneInfo());

	TMap<FName, FName> changeTable;

	for (auto &a : allbone) {
		auto p = meta->humanoidBoneTable.FindKey(a.Name.ToString());
		if (p == nullptr) {
			continue;
		}
		if (p->IsEmpty()) {
			continue;
		}
		for (auto &b : allbone) {
			if (a == b) continue;
			if (b.Name == **p) {
				b.Name = *(b.Name.ToString() + TEXT("_renamed_vrm4u"));
			}
		}
		changeTable.Add(a.Name, **p);

		a.Name = **p;
	}

	const_cast<FReferenceSkeleton&>(targetSkeleton->GetReferenceSkeleton()).RebuildRefSkeleton(targetSkeleton, true);

	targetSK->RefSkeleton = targetSkeleton->GetReferenceSkeleton();

#if WITH_EDITOR
#if	UE_VERSION_OLDER_THAN(4,20,0)
#else
	if (src_rig) {
		rig->SetSourceAsset(src_rig->GetSourceAsset());
		rig->SetTargetAsset(targetSK);

		targetSkeleton->RefreshRigConfig();

		{
			auto &table = src_rig->GetNodeMappingTable();

			for (auto &t : table) {
				FName n = t.Value;
				{
					auto *ret = changeTable.Find(t.Value);
					if (ret) {
						n = *ret;
					}
				}

				rig->AddMapping(t.Key, n);
				targetSkeleton->SetRigBoneMapping(t.Key, n);
			}
		}
		rig->PostEditChange();
	}
#endif
#endif
}

static void renameToUE4Bone(USkeletalMesh *targetSK, const USkeletalMesh *srcSK) {

	USkeleton *targetSkeleton = targetSK->Skeleton;
	UNodeMappingContainer * rig = targetSK->NodeMappingData[0];
	UNodeMappingContainer *src_rig = srcSK->NodeMappingData[0];

	//k->RemoveBonesFromSkeleton()
	auto &allbone = const_cast<TArray<FMeshBoneInfo> &>(targetSkeleton->GetReferenceSkeleton().GetRawRefBoneInfo());

	TMap<FName, FName> changeTable;

	for (auto &a : allbone) {
		VRMConverter::VRMBoneTable t;
		for (auto &tmp : VRMConverter::table_ue4_vrm) {
			if (a.Name != *(tmp.BoneVRM.ToLower())) continue;

			t = tmp;
			break;
		}
		if (t.BoneUE4.IsEmpty()) {
			continue;
		}
		FString newName = t.BoneUE4;

		for (auto &b : allbone) {
			if (a == b) continue;
			if (b.Name == *(newName.ToLower())) {
				b.Name = *(b.Name.ToString() + TEXT("_renamed_vrm4u"));
			}
		}
		changeTable.Add(a.Name, *newName);

		a.Name = *newName;
	}

	const_cast<FReferenceSkeleton&>(targetSkeleton->GetReferenceSkeleton()).RebuildRefSkeleton(targetSkeleton, true);

	targetSK->RefSkeleton = targetSkeleton->GetReferenceSkeleton();

#if WITH_EDITOR
#if	UE_VERSION_OLDER_THAN(4,20,0)
#else
	if (src_rig) {
		rig->SetSourceAsset(src_rig->GetSourceAsset());
		rig->SetTargetAsset(targetSK);

		targetSkeleton->RefreshRigConfig();

		{
			auto &table = src_rig->GetNodeMappingTable();

			for (auto &t : table) {
				FName n = t.Value;
				{
					auto *ret = changeTable.Find(t.Value);
					if (ret) {
						n = *ret;
					}
				}

				rig->AddMapping(t.Key, n);
				targetSkeleton->SetRigBoneMapping(t.Key, n);
			}
		}
		rig->PostEditChange();
	}
#endif
#endif
}



bool VRMConverter::ConvertHumanoid(UVrmAssetListObject *vrmAssetList, const aiScene *mScenePtr) {
	if (Options::Get().IsCreateHumanoidRenamedMesh() == false) {
		return true;
	}
	const USkeletalMesh *src_sk = vrmAssetList->SkeletalMesh;
	const USkeleton *src_k = src_sk->Skeleton;
	const UNodeMappingContainer *src_rig = nullptr;
	if (src_sk->NodeMappingData.Num()) {
		src_rig = src_sk->NodeMappingData[0];
	}

	USkeletalMesh *new_sk[2] = {};
	USkeleton *new_k[2] = {};
	UNodeMappingContainer *new_rig[2] = {};

	// 0: humanoid
	// 1: ue4mannequin
	for (int i = 0; i < 2; ++i) {
		FString name_skeleton;
		FString name_mesh;
		FString name_rig;

		if (i == 0) {
			name_skeleton = (TEXT("SKEL_") + vrmAssetList->BaseFileName + TEXT("_humanoid"));
			name_mesh = (FString(TEXT("SK_")) + vrmAssetList->BaseFileName + TEXT("_humanoid"));
			name_rig = (FString(TEXT("RIG_")) + vrmAssetList->BaseFileName + TEXT("_humanoid"));
		} else {
			name_skeleton = (TEXT("SKEL_") + vrmAssetList->BaseFileName + TEXT("_ue4mannequin"));
			name_mesh = (FString(TEXT("SK_")) + vrmAssetList->BaseFileName + TEXT("_ue4mannequin"));
			name_rig = (FString(TEXT("RIG_")) + vrmAssetList->BaseFileName + TEXT("_ue4mannequin"));
		}

		USkeleton *base = nullptr;
		USkeletalMesh *ss = nullptr;
		UNodeMappingContainer *rr = nullptr;

		if (i == 0) {
			ss = DuplicateObject<USkeletalMesh>(src_sk, vrmAssetList->Package, *name_mesh);
			base = DuplicateObject<USkeleton>(src_k, vrmAssetList->Package, *name_skeleton);
			rr = NewObject<UNodeMappingContainer>(vrmAssetList->Package, *name_rig, RF_Public | RF_Standalone);
		} else {
			ss = DuplicateObject<USkeletalMesh>(new_sk[0], vrmAssetList->Package, *name_mesh);
			base = DuplicateObject<USkeleton>(new_k[0], vrmAssetList->Package, *name_skeleton);
			rr = NewObject<UNodeMappingContainer>(vrmAssetList->Package, *name_rig, RF_Public | RF_Standalone);
		}

		new_sk[i] = ss;
		new_k[i] = base;
		new_rig[i] = rr;

		ss->Skeleton = base;
		ss->NodeMappingData.Empty();
		ss->NodeMappingData.Add(rr);


		if (i == 0) {
			// org -> humanoid
			renameToHumanoidBone(ss, vrmAssetList->VrmMetaObject, src_sk);
		} else {
			// humanoid -> ue4mannequin
			renameToUE4Bone(ss, new_sk[0]);
		}

		ss->CalculateInvRefMatrices();
		ss->CalculateExtendedBounds();
#if WITH_EDITORONLY_DATA
		ss->ConvertLegacyLODScreenSize();
#if	UE_VERSION_OLDER_THAN(4,20,0)
#else
		ss->UpdateGenerateUpToData();
#endif
#endif

#if WITH_EDITORONLY_DATA
		base->SetPreviewMesh(ss);
#endif
		base->RecreateBoneTree(ss);

		// retarget mode
		for (int bon = 0; bon < base->GetReferenceSkeleton().GetRawBoneNum(); ++bon) {
			auto type = src_k->GetBoneTranslationRetargetingMode(bon);
			base->SetBoneTranslationRetargetingMode(bon, type);
		}

		if (i == 0) {
			vrmAssetList->HumanoidSkeletalMesh = ss;
		}
	}

	return true;
}

VrmConvertHumanoid::VrmConvertHumanoid()
{
}

VrmConvertHumanoid::~VrmConvertHumanoid()
{
}
