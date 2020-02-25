// VRM4U Copyright (c) 2019 Haruyoshi Yamamoto. This software is released under the MIT License.
// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

#include "VrmConvertModel.h"
#include "VrmConvert.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/pbrmaterial.h>
#include <assimp/vrm/vrmmeta.h>

#include "VrmAssetListObject.h"
#include "VrmSkeleton.h"
#include "LoaderBPFunctionLibrary.h"

#include "Engine/SkeletalMesh.h"
#include "RenderingThread.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Animation/MorphTarget.h"

#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"


#include "Animation/AnimSequence.h"

#include "Async/ParallelFor.h"


#if WITH_EDITOR
typedef FSoftSkinVertex FSoftSkinVertexLocal;

#else
namespace {
	struct FSoftSkinVertexLocal
	{
		FVector			Position;

		// Tangent, U-direction
		FVector			TangentX;
		// Binormal, V-direction
		FVector			TangentY;
		// Normal
		FVector4		TangentZ;

		// UVs
		FVector2D		UVs[MAX_TEXCOORDS];
		// VertexColor
		FColor			Color;
		uint8			InfluenceBones[MAX_TOTAL_INFLUENCES];
		uint8			InfluenceWeights[MAX_TOTAL_INFLUENCES];

		/** If this vert is rigidly weighted to a bone, return true and the bone index. Otherwise return false. */
		//ENGINE_API bool GetRigidWeightBone(uint8& OutBoneIndex) const;

		/** Returns the maximum weight of any bone that influences this vertex. */
		//ENGINE_API uint8 GetMaximumWeight() const;

		/**
		* Serializer
		*
		* @param Ar - archive to serialize with
		* @param V - vertex to serialize
		* @return archive that was used
		*/
		//friend FArchive& operator<<(FArchive& Ar, FSoftSkinVertex& V);
	};
}
#endif

namespace {
	struct BoneMapOpt {
		int boneIndex;
		float weight;

		BoneMapOpt() : boneIndex(0), weight(0.f) {}

		bool operator<(const BoneMapOpt &b) const{
			return weight < b.weight;
		}

	};


	TArray<FString> addedList;
}

static const aiNode* GetBoneNodeFromMeshID(const int &meshID, const aiNode *node) {

	if (node == nullptr) {
		return nullptr;
	}
	for (uint32_t i = 0; i < node->mNumMeshes; ++i) {
		if (node->mMeshes[i] == meshID) {
			return node;
		}
	}

	for (uint32_t i = 0; i < node->mNumChildren; ++i) {
		auto a = GetBoneNodeFromMeshID(meshID, node->mChildren[i]);
		if (a) {
			return a;
		}
	}
	return nullptr;
}

static const  aiNode* GetNodeFromMeshID(int meshID, const aiScene *mScenePtr) {
	return GetBoneNodeFromMeshID(meshID, mScenePtr->mRootNode);
}

static int GetChildBoneLocal(const USkeleton *skeleton, const int32 ParentBoneIndex, TArray<int32> & Children) {
	Children.Reset();
	auto &r = skeleton->GetReferenceSkeleton();

	const int32 NumBones = r.GetRawBoneNum();
	for (int32 ChildIndex = ParentBoneIndex + 1; ChildIndex < NumBones; ChildIndex++)
	{
		if (ParentBoneIndex == r.GetParentIndex(ChildIndex))
		{
			Children.Add(ChildIndex);
		}
	}
	return Children.Num();
}



static void FindMeshInfo(const aiScene* scene, aiNode* node, FReturnedData& result)
{
	for (uint32 i = 0; i < node->mNumMeshes; i++)
	{
		FString Fs = UTF8_TO_TCHAR(node->mName.C_Str());
		int meshidx = node->mMeshes[i];
		aiMesh *mesh = scene->mMeshes[meshidx];
		FMeshInfo &mi = result.meshInfo[meshidx];

		//result.meshToIndex.FindOrAdd(mesh) = mi.Vertices.Num();

		bool bSkin = true;
		if (mesh->mNumBones == 0) {
			bSkin = false;
		}

		if (0) {
			int m = mesh->mNumVertices;
			mi.Vertices.Reserve(m);
			mi.Normals.Reserve(m);

			if (mesh->HasVertexColors(0)) {
				mi.VertexColors.Reserve(m);
			}
			if (mesh->HasTangentsAndBitangents()) {
				mi.Tangents.Reserve(m);
			}
			if (mesh->HasTextureCoords(0)) {
				mi.UV0.Reserve(m);
			}
		}

		//transform.
		aiMatrix4x4 tempTrans = node->mTransformation;
		if (bSkin == false) {
			auto *p = node->mParent;
			while (p) {
				aiMatrix4x4 aiMat = p->mTransformation;
				tempTrans *= aiMat;

				p = p->mParent;
			}
		}

		FMatrix tempMatrix;
		tempMatrix.M[0][0] = tempTrans.a1; tempMatrix.M[0][1] = tempTrans.b1; tempMatrix.M[0][2] = tempTrans.c1; tempMatrix.M[0][3] = tempTrans.d1;
		tempMatrix.M[1][0] = tempTrans.a2; tempMatrix.M[1][1] = tempTrans.b2; tempMatrix.M[1][2] = tempTrans.c2; tempMatrix.M[1][3] = tempTrans.d2;
		tempMatrix.M[2][0] = tempTrans.a3; tempMatrix.M[2][1] = tempTrans.b3; tempMatrix.M[2][2] = tempTrans.c3; tempMatrix.M[2][3] = tempTrans.d3;
		tempMatrix.M[3][0] = tempTrans.a4; tempMatrix.M[3][1] = tempTrans.b4; tempMatrix.M[3][2] = tempTrans.c4; tempMatrix.M[3][3] = tempTrans.d4;
		mi.RelativeTransform = FTransform(tempMatrix);

		auto &useFlag = mi.vertexUseFlag;
		if (VRMConverter::Options::Get().IsOptimizeVertex()) {
			// no morphtarget
			// optimize vertex

			// use flag
			useFlag.AddZeroed(mesh->mNumVertices);
			for (uint32_t f = 0; f < mesh->mNumFaces; ++f) {
				auto &face = mesh->mFaces[f];
				for (uint32_t d = 0; d < face.mNumIndices; ++d) {
					auto &ind = face.mIndices[d];
					useFlag[ind] = true;
				}
			}

			// optimize table
			TArray<uint32_t> useTable;
			useTable.AddZeroed(useFlag.Num());
			{
				int c = 0;
				for (int t = 0; t < useFlag.Num(); ++t) {
					useTable[t] = t - c;
					if (useFlag[t] == false) {
						++c;
					}
				}
			}

			// replace face index
			for (uint32_t f = 0; f < mesh->mNumFaces; ++f) {
				auto &face = mesh->mFaces[f];
				for (uint32_t d = 0; d < face.mNumIndices; ++d) {
					auto &ind = face.mIndices[d];
					face.mIndices[d] = useTable[face.mIndices[d]];
				}
			}

			// replace weight index
			for (uint32_t b = 0; b < mesh->mNumBones; ++b) {
				auto &bone = mesh->mBones[b];
				for (uint32_t w = 0; w < bone->mNumWeights; ++w) {
					auto &weight = bone->mWeights[w];

					uint32_t ind = weight.mVertexId;
					weight.mVertexId = useTable[ind];
					if (useFlag[ind] == 0) {
						weight.mWeight = 0.f;
					}
				}
			}
		}

		if (useFlag.Num() > 0) {
			mi.vertexIndexOptTable.SetNumZeroed(useFlag.Num());
		}

		mi.useVertexCount = 0;
		//vet
		for (uint32 j = 0; j < mesh->mNumVertices; ++j)
		{
			if ((int)j < useFlag.Num()) {
				if (useFlag[j] == false) {
					continue;
				}

				mi.vertexIndexOptTable[j] = mi.useVertexCount;
				mi.useVertexCount++;
			}

			FVector vertex = FVector(
				mesh->mVertices[j].x,
				mesh->mVertices[j].y,
				mesh->mVertices[j].z);

			vertex = mi.RelativeTransform.TransformFVector4(vertex);
			mi.Vertices.Push(vertex);

			//Normal
			if (mesh->HasNormals())
			{
				FVector normal = FVector(
					mesh->mNormals[j].x,
					mesh->mNormals[j].y,
					mesh->mNormals[j].z);

				//normal = mi.RelativeTransform.TransformFVector4(normal);
				mi.Normals.Push(normal);
			} else
			{
				mi.Normals.Push(FVector(0, 1, 0));
			}

			//UV Coordinates - inconsistent coordinates
			for (int u = 0; u < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++u) {
				if (mesh->HasTextureCoords(u))
				{
					if (mi.UV0.Num() <= u) {
						mi.UV0.Push(TArray<FVector2D>());
						if (u >= 1) {
							UE_LOG(LogTemp, Warning, TEXT("test uv2.\n"));
						}
					}
					FVector2D uv = FVector2D(mesh->mTextureCoords[u][j].x, -mesh->mTextureCoords[u][j].y);
					mi.UV0[u].Add(uv);
				}
			}

			//Tangent
			if (mesh->HasTangentsAndBitangents())
			{
				FVector v(mesh->mTangents[j].x, mesh->mTangents[j].y, mesh->mTangents[j].z);
				//FProcMeshTangent meshTangent = FProcMeshTangent(v.X, v.Y, v.Z);
				mi.Tangents.Push(v);
				//mi.MeshTangents.Push(meshTangent);
			}

			//Vertex color
			if (mesh->HasVertexColors(0))
			{
				FLinearColor color = FLinearColor(
					mesh->mColors[0][j].r,
					mesh->mColors[0][j].g,
					mesh->mColors[0][j].b,
					mesh->mColors[0][j].a
				);
				mi.VertexColors.Push(color);
			}

		}
	}
}


static void FindMesh(const aiScene* scene, aiNode* node, FReturnedData& retdata)
{
	FindMeshInfo(scene, node, retdata);

	for (uint32 m = 0; m < node->mNumChildren; ++m)
	{
		FindMesh(scene, node->mChildren[m], retdata);
	}
}

static UPhysicsConstraintTemplate *createConstraint(USkeletalMesh *sk, UPhysicsAsset *pa, VRM::VRMSpring &spring, FName con1, FName con2){
	UPhysicsConstraintTemplate *ct = NewObject<UPhysicsConstraintTemplate>(pa, NAME_None, RF_Transactional);
	pa->ConstraintSetup.Add(ct);

	//"skirt_01_01"
	ct->Modify(false);
	{
		FString orgname = con1.ToString() + TEXT("_") + con2.ToString();
		FString cname = orgname;
		int Index = 0;
		while(pa->FindConstraintIndex(*cname) != INDEX_NONE)
		{
			cname = FString::Printf(TEXT("%s_%d"), *orgname, Index++);
		}
		ct->DefaultInstance.JointName = *cname;
	}

	ct->DefaultInstance.ConstraintBone1 = con1;
	ct->DefaultInstance.ConstraintBone2 = con2;

	ct->DefaultInstance.SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Limited, 10);
	ct->DefaultInstance.SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Limited, 10);
	ct->DefaultInstance.SetAngularTwistLimit(EAngularConstraintMotion::ACM_Limited, 10);

	ct->DefaultInstance.ProfileInstance.ConeLimit.Stiffness = 100.f * spring.stiffness;
	ct->DefaultInstance.ProfileInstance.TwistLimit.Stiffness = 100.f * spring.stiffness;

	const int32 BoneIndex1 = sk->RefSkeleton.FindBoneIndex(ct->DefaultInstance.ConstraintBone1);
	const int32 BoneIndex2 = sk->RefSkeleton.FindBoneIndex(ct->DefaultInstance.ConstraintBone2);

	if (BoneIndex1 == INDEX_NONE || BoneIndex2 == INDEX_NONE) {
#if WITH_EDITOR
		if (VRMConverter::IsImportMode()) {
			ct->PostEditChange();
		}
#endif
		return ct;
	}

	check(BoneIndex1 != INDEX_NONE);
	check(BoneIndex2 != INDEX_NONE);

	const TArray<FTransform> &t = sk->RefSkeleton.GetRawRefBonePose();

	const FTransform BoneTransform1 = t[BoneIndex1];//sk->GetBoneTransform(BoneIndex1);
	FTransform BoneTransform2 = t[BoneIndex2];//EditorSkelComp->GetBoneTransform(BoneIndex2);

	{
		int32 tb = BoneIndex2;
		while (1) {
			int32 p = sk->RefSkeleton.GetRawParentIndex(tb);
			if (p < 0) break;

			const FTransform &f = t[p];
			//BoneTransform2 = f.GetRelativeTransform(BoneTransform2);

			if (p == BoneIndex1) {
				break;
			}
			tb = p;
		}
	}

	//auto b = BoneTransform1;
	ct->DefaultInstance.Pos1 = FVector::ZeroVector;
	ct->DefaultInstance.PriAxis1 = FVector(1, 0, 0);
	ct->DefaultInstance.SecAxis1 = FVector(0, 1, 0);


	auto r = BoneTransform2;// .GetRelativeTransform(BoneTransform2);
							//	auto r = BoneTransform1.GetRelativeTransform(BoneTransform2);
	auto twis = r.GetLocation().GetSafeNormal();
	auto p1 = twis;
	p1.X = p1.Z = 0.f;
	auto p2 = FVector::CrossProduct(twis, p1).GetSafeNormal();
	p1 = FVector::CrossProduct(p2, twis).GetSafeNormal();

	ct->DefaultInstance.Pos2 = -r.GetLocation();
	//ct->DefaultInstance.PriAxis2 = p1;
	//ct->DefaultInstance.SecAxis2 = p2;
	ct->DefaultInstance.PriAxis2 = r.GetUnitAxis(EAxis::X);
	ct->DefaultInstance.SecAxis2 = r.GetUnitAxis(EAxis::Y);

	// child 
	//ct->DefaultInstance.SetRefFrame(EConstraintFrame::Frame1, FTransform::Identity);
	// parent
	//ct->DefaultInstance.SetRefFrame(EConstraintFrame::Frame2, BoneTransform1.GetRelativeTransform(BoneTransform2));

#if WITH_EDITOR
	ct->SetDefaultProfile(ct->DefaultInstance);
	if (VRMConverter::IsImportMode()) {
		ct->PostEditChange();
	}
#endif
	//ct->DefaultInstance.InitConstraint();

	return ct;
}

static void CreateSwingTail(UVrmAssetListObject *vrmAssetList, VRM::VRMSpring &spring, FName &boneName, USkeletalBodySetup *bs, int BodyIndex1,
	TArray<int> &swingBoneIndexArray, int sboneIndex = -1) {

	USkeletalMesh *sk = vrmAssetList->SkeletalMesh;
	USkeleton *k = sk->Skeleton;
	UPhysicsAsset *pa = sk->PhysicsAsset;

	TArray<int32> child;
	int32 ii = k->GetReferenceSkeleton().FindBoneIndex(boneName);
	GetChildBoneLocal(k, ii, child);
	for (auto &c : child) {

		if (sboneIndex >= 0) {
			c = sboneIndex;
		}

		if (addedList.Find(k->GetReferenceSkeleton().GetBoneName(c).ToString().ToLower()) >= 0) {
			continue;
		}
		addedList.Add(k->GetReferenceSkeleton().GetBoneName(c).ToString().ToLower());


		USkeletalBodySetup *bs2 = Cast<USkeletalBodySetup>(StaticDuplicateObject(bs, pa, NAME_None));

		bs2->BoneName = k->GetReferenceSkeleton().GetBoneName(c);
		bs2->PhysicsType = PhysType_Simulated;
		bs2->CollisionReponse = EBodyCollisionResponse::BodyCollision_Enabled;
		//bs2->profile

		int BodyIndex2 = pa->SkeletalBodySetups.Add(bs2);
		auto *ct = createConstraint(sk, pa, spring, boneName, bs2->BoneName);
		pa->DisableCollision(BodyIndex1, BodyIndex2);

		swingBoneIndexArray.AddUnique(BodyIndex2);

		//CreateSwingTail(vrmAssetList, spring, bs2->BoneName, bs2, BodyIndex2, swingBoneIndexArray);
		if (sboneIndex >= 0) {
			break;
		}
	}
}

static void CreateSwingHead(UVrmAssetListObject *vrmAssetList, VRM::VRMSpring &spring, FName &boneName, TArray<int> &swingBoneIndexArray, int sboneIndex) {

	USkeletalMesh *sk = vrmAssetList->SkeletalMesh;
	USkeleton *k = sk->Skeleton;
	UPhysicsAsset *pa = sk->PhysicsAsset;

	{
		int i = sk->RefSkeleton.FindRawBoneIndex(boneName);
		if (i == INDEX_NONE) {
			return;
		}
	}
	//sk->RefSkeleton->GetParentIndex();

	USkeletalBodySetup *bs = nullptr;
	int BodyIndex1 = -1;

	if (addedList.Find(boneName.ToString().ToLower()) < 0) {
		addedList.Add(boneName.ToString().ToLower());

		bs = NewObject<USkeletalBodySetup>(pa, NAME_None, RF_Transactional);

		//int nodeID = mScenePtr->mRootNode->FindNode(sbone.c_str());
		//sk->RefSkeleton.GetRawRefBoneInfo[0].
		//sk->bonetree
		//bs->constrai
		FKAggregateGeom agg;
		FKSphereElem SphereElem;
		SphereElem.Center = FVector(0);
		SphereElem.Radius = spring.hitRadius * 100.f;
		agg.SphereElems.Add(SphereElem);


		bs->Modify();
		bs->BoneName = boneName;
		bs->AddCollisionFrom(agg);
		bs->CollisionTraceFlag = CTF_UseSimpleAsComplex;
		// newly created bodies default to simulating
		bs->PhysicsType = PhysType_Kinematic;	// fix
												//bs->get
		bs->CollisionReponse = EBodyCollisionResponse::BodyCollision_Disabled;
		bs->DefaultInstance.InertiaTensorScale.Set(2, 2, 2);
		bs->DefaultInstance.LinearDamping = 10.0f * spring.dragForce;
		bs->DefaultInstance.AngularDamping = 10.0f * spring.dragForce;

		bs->InvalidatePhysicsData();
		bs->CreatePhysicsMeshes();
		BodyIndex1 = pa->SkeletalBodySetups.Add(bs);

		//pa->UpdateBodySetupIndexMap();
#if WITH_EDITOR
	//pa->InvalidateAllPhysicsMeshes();
#endif
	} else {
		for (int i = 0; i < pa->SkeletalBodySetups.Num(); ++i) {
			auto &a = pa->SkeletalBodySetups[i];
			if (a->BoneName != boneName) {
				continue;
			}

			BodyIndex1 = i;
			bs = a;
			break;
		}
	}

	//aaaaaa2(vrmAssetList, spring, boneName, bs, BodyIndex1, swingBoneIndexArray, sboneIndex);
	if (BodyIndex1 >= 0) {
		CreateSwingTail(vrmAssetList, spring, boneName, bs, BodyIndex1, swingBoneIndexArray);
	}

	/*
	{
		//Material = (UMaterial*)StaticDuplicateObject(OriginalMaterial, GetTransientPackage(), NAME_None, ~RF_Standalone, UPreviewMaterial::StaticClass()); 
		TArray<int32> child;
		int32 ii = k->GetReferenceSkeleton().FindBoneIndex(boneName);
		GetChildBoneLocal(k, ii, child);
		for (auto &c : child) {
			USkeletalBodySetup *bs2 = Cast<USkeletalBodySetup>(StaticDuplicateObject(bs, pa, NAME_None));

			bs2->BoneName = k->GetReferenceSkeleton().GetBoneName(c);
			bs2->PhysicsType = PhysType_Simulated;
			bs2->CollisionReponse = EBodyCollisionResponse::BodyCollision_Enabled;
			//bs2->profile

			int BodyIndex2 = pa->SkeletalBodySetups.Add(bs2);
			createConstraint(sk, pa, spring, boneName, bs2->BoneName);
			pa->DisableCollision(BodyIndex1, BodyIndex2);

			swingBoneIndexArray.AddUnique(BodyIndex2);

			aaaaaa(vrmAssetList, spring, bs2->BoneName, swingBoneIndexArray);
		}
	}
	*/
}

bool VRMConverter::ConvertModel(UVrmAssetListObject *vrmAssetList, const aiScene *mScenePtr) {
	if (vrmAssetList == nullptr) {
		return false;
	}

	vrmAssetList->MeshReturnedData = MakeShareable(new FReturnedData());
	FReturnedData &result = *(vrmAssetList->MeshReturnedData);
	//FReturnedData &result = *(vrmAssetList->Result);

	result.bSuccess = false;
	result.meshInfo.Empty();
	result.NumMeshes = 0;

	if (mScenePtr == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("test null.\n"));
	}

	if (mScenePtr->HasMeshes())
	{
		result.meshInfo.SetNum(mScenePtr->mNumMeshes, false);

		FindMesh(mScenePtr, mScenePtr->mRootNode, result);

		for (uint32 i = 0; i < mScenePtr->mNumMeshes; ++i)
		{
			//Triangle number
			for (uint32 l = 0; l < mScenePtr->mMeshes[i]->mNumFaces; ++l)
			{
				for (uint32 m = 0; m < mScenePtr->mMeshes[i]->mFaces[l].mNumIndices; ++m)
				{
					if (m >= 3) {
						UE_LOG(LogTemp, Warning, TEXT("FindMeshInfo. %d\n"), m);
					}
					result.meshInfo[i].Triangles.Push(mScenePtr->mMeshes[i]->mFaces[l].mIndices[m]);
				}
			}
		}
		result.bSuccess = true;
	}

	USkeletalMesh *sk = nullptr;
	if (vrmAssetList->Package == GetTransientPackage()) {
		sk = NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, EObjectFlags::RF_Public | RF_Transient);
	}else {
		TArray<UObject*> ret;
		FString name = (FString(TEXT("SK_")) + vrmAssetList->BaseFileName);
		GetObjectsWithOuter(vrmAssetList->Package, ret);
		for (auto *a : ret) {
			auto s = a->GetName().ToLower();
			if (s.IsEmpty()) continue;

			if (s == name.ToLower()) {
				//a->ClearFlags(EObjectFlags::RF_Standalone);
				//a->SetFlags(EObjectFlags::RF_Public | RF_Transient);
				//a->ConditionalBeginDestroy();
				//a->Rename(TEXT("aaaaaaa"));
				//auto *q = Cast<USkeletalMesh>(a);
				//if (q) {
				//	q->Materials.Empty();
				//}
				static int ccc = 0;
				++ccc;
				a->Rename(*(FString(TEXT("need_reload_sk_VRM"))+FString::FromInt(ccc)), GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders);

				break;
			}
		}
		sk = NewObject<USkeletalMesh>(vrmAssetList->Package, *name, EObjectFlags::RF_Public | EObjectFlags::RF_Standalone);
	}

	{
#if WITH_EDITOR
		sk->PreEditChange(NULL);
		//Dirty the DDC Key for any imported Skeletal Mesh
#if	UE_VERSION_OLDER_THAN(4,24,0)
#else
		sk->InvalidateDeriveDataCacheGUID();
#endif
#endif
	}


	USkeleton *k = Options::Get().GetSkeleton();
	if (k == nullptr){
		if (vrmAssetList->Package == GetTransientPackage()) {
			k = NewObject<USkeleton>(GetTransientPackage(), NAME_None, EObjectFlags::RF_Public | RF_Transient);
		}else {
			TArray<UObject*> ret;
			FString name = (TEXT("SKEL_") + vrmAssetList->BaseFileName);
			GetObjectsWithOuter(vrmAssetList->Package, ret);
			for (auto *a : ret) {
				auto s = a->GetName().ToLower();
				if (s.IsEmpty()) continue;

				if (s == name.ToLower()) {
					a->ClearFlags(EObjectFlags::RF_Standalone);
					a->SetFlags(EObjectFlags::RF_Public | RF_Transient);
					a->ConditionalBeginDestroy();
					break;
				}
			}

			k = NewObject<USkeleton>(vrmAssetList->Package, *name, EObjectFlags::RF_Public | EObjectFlags::RF_Standalone);
		}
	}

	int allIndex = 0;
	int allVertex = 0;
	int uvNum = 1;
	{
		for (int meshID = 0; meshID < result.meshInfo.Num(); ++meshID) {
			allIndex += result.meshInfo[meshID].Triangles.Num();
			allVertex += result.meshInfo[meshID].Vertices.Num();
		}
		for (int meshID = 0; meshID < result.meshInfo.Num(); ++meshID) {
			auto &mInfo = result.meshInfo[meshID];
			uvNum = FMath::Max(uvNum, mInfo.UV0.Num());
			if (uvNum >= 2) {
				UE_LOG(LogTemp, Warning, TEXT("test uv2.\n"));
			}
		}
	}

	static int boneOffset = 0;
	{
		// name dup check
		sk->Skeleton = k;
		//k->MergeAllBonesToBoneTree(src);
		{
			USkeletalMesh *sk_tmp = NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, EObjectFlags::RF_Public | RF_Transient);
			UVrmSkeleton *k_tmp = NewObject<UVrmSkeleton>(GetTransientPackage(), NAME_None, EObjectFlags::RF_Public | RF_Transient);

			k_tmp->readVrmBone(const_cast<aiScene*>(mScenePtr), boneOffset);
			k_tmp->addIKBone(vrmAssetList);
			sk_tmp->Skeleton = k_tmp;
			sk_tmp->RefSkeleton = k_tmp->GetReferenceSkeleton();

			bool b = k->MergeAllBonesToBoneTree(sk_tmp);
			if (b == false) {
				// import failure
				return false;
			}
		}

		sk->RefSkeleton = k->GetReferenceSkeleton();
		//sk->RefSkeleton.RebuildNameToIndexMap();

		//sk->RefSkeleton.RebuildRefSkeleton(sk->Skeleton, true);
		//sk->Proc();

		//src->RefSkeleton = sk->RefSkeleton;

		//src->Skeleton->MergeAllBonesToBoneTree(sk);
		sk->CalculateInvRefMatrices();
		sk->CalculateExtendedBounds();
#if WITH_EDITOR
#if	UE_VERSION_OLDER_THAN(4,20,0)
#else
		sk->UpdateGenerateUpToData();
#endif
		sk->ConvertLegacyLODScreenSize();
		sk->GetImportedModel()->LODModels.Reset();
#endif

#if WITH_EDITORONLY_DATA
		k->SetPreviewMesh(sk);
#endif
		k->RecreateBoneTree(sk);
		{
			TArray<FName> BonesToRemove;
			k->RemoveVirtualBones(BonesToRemove);
		}

		// changet retarget option
		if (IsImportMode() == false){
			//k->Modify();

			for (int i = 0; i < k->GetReferenceSkeleton().GetRawBoneNum(); ++i) {
				//const int32 BoneIndex = k->GetReferenceSkeleton().FindBoneIndex(InBoneName);
				//k->SetBoneTranslationRetargetingMode(i, EBoneTranslationRetargetingMode::Skeleton);
				//FAssetNotifications::SkeletonNeedsToBeSaved(k);
			}
		}


		// sk end

		vrmAssetList->SkeletalMesh = sk;

#if	UE_VERSION_OLDER_THAN(4,20,0)
		sk->LODInfo.AddZeroed(1);
		//const USkeletalMeshLODSettings* DefaultSetting = sk->GetDefaultLODSetting();
		// if failed to get setting, that means, we don't have proper setting 
		// in that case, use last index setting
		//!DefaultSetting->SetLODSettingsToMesh(sk, 0);
#else
		{
			FSkeletalMeshLODInfo &info = sk->AddLODInfo();
		}
#endif

		//sk->CacheDerivedData();
		sk->AllocateResourceForRendering();
		FSkeletalMeshRenderData *p = sk->GetResourceForRendering();
		//p->Cache(sk);
		//sk->OnPostMeshCached().Broadcast(sk);
#if	UE_VERSION_OLDER_THAN(4,23,0)
		FSkeletalMeshLODRenderData *pRd = new(p->LODRenderData) FSkeletalMeshLODRenderData();
#else
		FSkeletalMeshLODRenderData *pRd = new FSkeletalMeshLODRenderData();
		p->LODRenderData.Add(pRd);
#endif

		{
			pRd->StaticVertexBuffers.PositionVertexBuffer.Init(allVertex);
			pRd->StaticVertexBuffers.ColorVertexBuffer.InitFromSingleColor(FColor(255, 255, 255, 255), allVertex);
			pRd->StaticVertexBuffers.StaticMeshVertexBuffer.Init(allVertex, uvNum);


#if WITH_EDITOR
			TArray<FSoftSkinVertex> Weight;
			Weight.SetNum(allVertex);
			pRd->SkinWeightVertexBuffer.Init(Weight);
#else
			{
				TArray< TSkinWeightInfo<false> > InWeights;
				InWeights.SetNum(allVertex);
				for (auto &a : InWeights) {
					memset(a.InfluenceBones, 0, sizeof(a.InfluenceBones));
					memset(a.InfluenceWeights, 0, sizeof(a.InfluenceWeights));
				}
				pRd->SkinWeightVertexBuffer = InWeights;
			}
#endif
		}

		sk->InitResources();

		sk->Materials.SetNum(vrmAssetList->Materials.Num());
		for (int i = 0; i < sk->Materials.Num(); ++i) {
			sk->Materials[i].MaterialInterface = vrmAssetList->Materials[i];
			sk->Materials[i].UVChannelData = FMeshUVChannelInfo(1);
			sk->Materials[i].MaterialSlotName = UTF8_TO_TCHAR(mScenePtr->mMaterials[i]->GetName().C_Str());
		}
		if (sk->Materials.Num() == 0) {
			sk->Materials.SetNum(1);
		}
	}
	//USkeleton* NewAsset = NewObject<USkeleton>();

	//USkeleton* NewAsset = nullptr;// = src->Skeleton;// DuplicateObject<USkeleton>(src, src->GetOuter(), TEXT("/Game/FirstPerson/Character/Mesh/SK_Mannequin_Arms_Skeletonaaaaaaa"));
	{
		FSkeletalMeshLODRenderData &rd = sk->GetResourceForRendering()->LODRenderData[0];

		FVector BoundMin(-100, -100, 0);
		FVector BoundMax(100, 100, 200);

		{
			int boneNum = sk->Skeleton->GetReferenceSkeleton().GetRawBoneNum();
			rd.RequiredBones.SetNum(boneNum);
			rd.ActiveBoneIndices.SetNum(boneNum);
			for (int i = 0; i < boneNum; ++i) {
				rd.RequiredBones[i] = i;
				rd.ActiveBoneIndices[i] = i;
			}
		}
		{
			FStaticMeshVertexBuffers	 &v = rd.StaticVertexBuffers;

			FSoftSkinVertexLocal softSkinVertexLocalZero;
			{
#if	UE_VERSION_OLDER_THAN(4,20,0)
				{
					FPackedNormal n(0);
					softSkinVertexLocalZero.Position = FVector::ZeroVector;
					softSkinVertexLocalZero.TangentX = softSkinVertexLocalZero.TangentY = softSkinVertexLocalZero.TangentZ = n;
				}
#else
				softSkinVertexLocalZero.Position = softSkinVertexLocalZero.TangentX = softSkinVertexLocalZero.TangentY = FVector::ZeroVector;
				softSkinVertexLocalZero.TangentZ.Set(0, 0, 0, 1);
#endif
				softSkinVertexLocalZero.Color = FColor::White;

				memset(softSkinVertexLocalZero.UVs, 0, sizeof(softSkinVertexLocalZero.UVs));
				memset(softSkinVertexLocalZero.InfluenceBones, 0, sizeof(softSkinVertexLocalZero.InfluenceBones));
				memset(softSkinVertexLocalZero.InfluenceWeights, 0, sizeof(softSkinVertexLocalZero.InfluenceWeights));
			}

			TArray<uint32> Triangles;
			TArray<FSoftSkinVertexLocal> Weight;
			Weight.SetNum(allVertex);
			for (auto &w : Weight) {
				w = softSkinVertexLocalZero;
			}

			//Weight.AddZeroed(allVertex);
			int currentIndex = 0;
			int currentVertex = 0;

#if WITH_EDITORONLY_DATA
#if	UE_VERSION_OLDER_THAN(4,23,0)
			new(sk->GetImportedModel()->LODModels) FSkeletalMeshLODModel();
#else
			sk->GetImportedModel()->LODModels.Add(new FSkeletalMeshLODModel());
#endif
			sk->GetImportedModel()->LODModels[0].Sections.SetNum(result.meshInfo.Num());
#endif

			for (int meshID = 0; meshID < result.meshInfo.Num(); ++meshID) {
				TArray<FSoftSkinVertexLocal> meshWeight;
				auto &mInfo = result.meshInfo[meshID];

				for (int i = 0; i < mInfo.Vertices.Num(); ++i) {
					FSoftSkinVertexLocal *meshS = new(meshWeight) FSoftSkinVertexLocal();
					*meshS = softSkinVertexLocalZero;
					auto a = result.meshInfo[meshID].Vertices[i] * 100.f;

					v.PositionVertexBuffer.VertexPosition(currentVertex + i).Set(-a.X, a.Z, a.Y);
					if (VRMConverter::Options::Get().IsVRMModel() == false) {
						v.PositionVertexBuffer.VertexPosition(currentVertex + i).Set(a.X, -a.Z, a.Y);
					}
					v.PositionVertexBuffer.VertexPosition(currentVertex + i) *= VRMConverter::Options::Get().GetModelScale();

					for (int u=0; u<mInfo.UV0.Num(); ++u){
						FVector2D uv(0, 0);
						if (i < mInfo.UV0[u].Num()) {
							uv = mInfo.UV0[u][i];
						}
						v.StaticMeshVertexBuffer.SetVertexUV(currentVertex + i, u, uv);
						meshS->UVs[u] = uv;
					}

					if (i < mInfo.Tangents.Num()){
						//v.StaticMeshVertexBuffer.SetVertexTangents(currentVertex + i, FVector(1, 0, 0), FVector(0, 1, 0), FVector(0, 0, 1));
						//v.StaticMeshVertexBuffer.SetVertexTangents(currentVertex + i, result.meshInfo[meshID].Tangents);
						auto &n = mInfo.Normals[i];
						FVector n_tmp(-n.X, n.Z, n.Y);
						FVector t_tmp(-mInfo.Tangents[i].X, mInfo.Tangents[i].Z, mInfo.Tangents[i].Y);

						t_tmp.Normalize();
						n_tmp.Normalize();

						meshS->TangentX = t_tmp;
						meshS->TangentY = n_tmp ^ t_tmp;
						meshS->TangentZ = n_tmp;

						v.StaticMeshVertexBuffer.SetVertexTangents(currentVertex + i, meshS->TangentX, meshS->TangentY, meshS->TangentZ);

					}

					if (i < mInfo.VertexColors.Num()) {
						auto &c = mInfo.VertexColors[i];
						meshS->Color = FColor(c.R, c.G, c.B, c.A);
					}

					auto aiS = rd.SkinWeightVertexBuffer.GetSkinWeightPtr<false>(i);
					//aiS->InfluenceWeights
					// InfluenceBones

					FSoftSkinVertexLocal &s = Weight[currentVertex + i];
					s.Position = v.PositionVertexBuffer.VertexPosition(currentVertex + i);
					meshS->Position = v.PositionVertexBuffer.VertexPosition(currentVertex + i);

					//s.InfluenceBones[0] = 0;// aiS->InfluenceBones[0];// +boneOffset;
					//meshS->InfluenceBones[0] = 0;
					//s.InfluenceWeights[0] = 255;
					//meshS->InfluenceWeights[0] = 255;

					//s.InfluenceWeights[0] = aiS->InfluenceWeights[0];
					//s.InfluenceWeights[1] = aiS->InfluenceWeights[1];


					//aiNode *bone = mScenePtr->mRootNode->FindNode( mScenePtr->mMeshes[meshID]->mBones[0]->mName );
				} // vertex loop

				auto &aiM = mScenePtr->mMeshes[meshID];
				TArray<int> bonemap;
				bonemap.Add(0);
				TArray<BoneMapOpt> boneAll;
				{
					BoneMapOpt o;
					o.boneIndex = 0;
					o.weight = -1.f;
					boneAll.Add(o);
				}
				//mScenePtr->mRootNode->mMeshes
				for (uint32 boneIndex = 0; boneIndex < aiM->mNumBones; ++boneIndex) {
					auto &aiB = aiM->mBones[boneIndex];

					const int b = sk->RefSkeleton.FindBoneIndex(UTF8_TO_TCHAR(aiB->mName.C_Str()));

					if (b < 0) {
						continue;
					}
					for (uint32 weightIndex = 0; weightIndex < aiB->mNumWeights; ++weightIndex) {
						auto &aiW = aiB->mWeights[weightIndex];

						if (aiW.mWeight == 0.f) {
							continue;
						}
						for (int jj = 0; jj < 8; ++jj) {
							auto &s = Weight[aiW.mVertexId + currentVertex];
							if (s.InfluenceWeights[jj] > 0) {
								continue;
							}

							int tabledIndex = 0;
							auto f = bonemap.Find(b);
							if (f != INDEX_NONE) {
								tabledIndex = f;
							}
							else {
								if (Options::Get().IsDebugOneBone() == false) {
									tabledIndex = bonemap.Add(b);
								}
							}
							if (tabledIndex > 255) {
								UE_LOG(LogTemp, Warning, TEXT("bonemap over!"));
							}

							if (Options::Get().IsDebugOneBone()) {
								tabledIndex = 0;
							}

							const float ww = FMath::Clamp(aiW.mWeight, 0.f, 1.f);
							s.InfluenceBones[jj] = tabledIndex;
							//s.InfluenceWeights[jj] = (uint8)FMath::TruncToInt(ww * 255.f + (0.5f - KINDA_SMALL_NUMBER));
							s.InfluenceWeights[jj] = (uint8)FMath::TruncToInt(ww * 255.f);

							meshWeight[aiW.mVertexId].InfluenceBones[jj] = s.InfluenceBones[jj];
							meshWeight[aiW.mVertexId].InfluenceWeights[jj] = s.InfluenceWeights[jj];

							if (Options::Get().IsMobileBone()){
								auto p = boneAll.FindByPredicate([&](BoneMapOpt &o) {return o.boneIndex == b; });
								if (p) {
									p->weight += aiW.mWeight;
								} else {
									BoneMapOpt o;
									o.boneIndex = b;
									o.weight = aiW.mWeight;
									boneAll.Add(o);
								}
							}

							break;
						}
					}
				}// bone loop

				// mobile remap
				if (Options::Get().IsMobileBone() && boneAll.Num() > 75) {
					TMap<int, int> mobileMap;

					auto bonemapNew = bonemap;

					while (boneAll.Num() > 75) {
						boneAll.Sort();

						// bone 0 == weight 0
						// search from 1
						auto &removed = boneAll[1];

						int findParent = removed.boneIndex;
						while (findParent >= 0) {
							findParent = sk->RefSkeleton.GetParentIndex(findParent);
							auto p = boneAll.FindByPredicate([&](const BoneMapOpt &o) {return o.boneIndex == findParent;});

							if (p == nullptr) {
								continue;
							}

							p->weight += removed.weight;
							
							while(auto a = mobileMap.FindKey(removed.boneIndex) ){
								mobileMap[*a] = p->boneIndex;
							}
							mobileMap.FindOrAdd(removed.boneIndex) = p->boneIndex;
							break;
						}

						bonemapNew.Remove(removed.boneIndex);
						boneAll.RemoveAt(1);
					}
					if (mobileMap.Num()) {
						for (auto &a : Weight) {
							for (int i = 0; i < 8; ++i) {
								auto &infBone = a.InfluenceBones[i];
								auto &infWeight = a.InfluenceWeights[i];
								if (bonemap.IsValidIndex(infBone) == false) {
									infWeight = 0.f;
									infBone = 0;
									continue;
								}
								auto srcBoneIndex = bonemap[infBone];
								auto f = mobileMap.Find(srcBoneIndex);
								if (f) {
									auto dstBoneIndex = *f;
									infBone = bonemapNew.IndexOfByKey(dstBoneIndex);
								} else {
									infBone = bonemapNew.IndexOfByKey(srcBoneIndex);
								}
							}
						}
						for (auto &a : meshWeight) {
							for (int i = 0; i < 8; ++i) {
								auto &infBone = a.InfluenceBones[i];
								auto &infWeight = a.InfluenceWeights[i];
								if (bonemap.IsValidIndex(infBone) == false) {
									infWeight = 0.f;
									infBone = 0;
									continue;
								}
								auto srcBoneIndex = bonemap[infBone];
								auto f = mobileMap.Find(srcBoneIndex);
								if (f) {
									auto dstBoneIndex = *f;
									infBone = bonemapNew.IndexOfByKey(dstBoneIndex);
								} else {
									infBone = bonemapNew.IndexOfByKey(srcBoneIndex);
								}
							}
						}
					}
					bonemap = bonemapNew;
				}// mobile remap

				if (VRMConverter::IsImportMode() == false) {
					rd.RenderSections.SetNum(result.meshInfo.Num());

					FSkelMeshRenderSection &NewRenderSection = rd.RenderSections[meshID];
					//NewRenderSection = rd.RenderSections[0];

					bool bUseMergeMaterial = Options::Get().IsMergeMaterial();
					if ((int)aiM->mMaterialIndex >= vrmAssetList->MaterialMergeTable.Num()) {
						bUseMergeMaterial = false;
					}
					if (bUseMergeMaterial) {
						NewRenderSection.MaterialIndex = vrmAssetList->MaterialMergeTable[aiM->mMaterialIndex];// ModelSection.MaterialIndex;
					}else {
						NewRenderSection.MaterialIndex = aiM->mMaterialIndex;// ModelSection.MaterialIndex;
					}
					if (NewRenderSection.MaterialIndex >= vrmAssetList->Materials.Num()) NewRenderSection.MaterialIndex = 0;
					NewRenderSection.BaseIndex = currentIndex;
					NewRenderSection.NumTriangles = result.meshInfo[meshID].Triangles.Num() / 3;
					//NewRenderSection.bRecomputeTangent = ModelSection.bRecomputeTangent;
					NewRenderSection.bCastShadow = true;// ModelSection.bCastShadow;
					NewRenderSection.BaseVertexIndex = currentVertex;// currentVertex;// currentVertex;// ModelSection.BaseVertexIndex;
															//NewRenderSection.ClothMappingData = ModelSection.ClothMappingData;
															//NewRenderSection.BoneMap.SetNum(1);//ModelSection.BoneMap;
															//NewRenderSection.BoneMap[0] = 10;
					//NewRenderSection.BoneMap.SetNum(sk->Skeleton->GetBoneTree().Num());//ModelSection.BoneMap;
					//for (int i = 0; i < NewRenderSection.BoneMap.Num(); ++i) {
					//	NewRenderSection.BoneMap[i] = i;
					//}
					if (bonemap.Num() > 0) {
						NewRenderSection.BoneMap.SetNum(bonemap.Num());//ModelSection.BoneMap;
						for (int i = 0; i < NewRenderSection.BoneMap.Num(); ++i) {
							NewRenderSection.BoneMap[i] = bonemap[i];
						}
					}else {
						NewRenderSection.BoneMap.SetNum(1);
						auto *p = GetNodeFromMeshID(meshID, mScenePtr);
						int32 i = k->GetReferenceSkeleton().FindBoneIndex(UTF8_TO_TCHAR(p->mName.C_Str()));
						if (i <= 0) {
							i = meshID;
						}
						NewRenderSection.BoneMap[0] = i;
					}


					NewRenderSection.NumVertices = result.meshInfo[meshID].Vertices.Num();// result.meshInfo[meshID].Triangles.Num();// allVertex;// result.meshInfo[meshID].Vertices.Num();// ModelSection.NumVertices;

					NewRenderSection.MaxBoneInfluences = 4;// ModelSection.MaxBoneInfluences;
															//NewRenderSection.CorrespondClothAssetIndex = ModelSection.CorrespondClothAssetIndex;
															//NewRenderSection.ClothingData = ModelSection.ClothingData;
					TMap<int32, TArray<int32>> OverlappingVertices;
					NewRenderSection.DuplicatedVerticesBuffer.Init(NewRenderSection.NumVertices, OverlappingVertices);
					NewRenderSection.bDisabled = false;// ModelSection.bDisabled;
														//RenderSections.Add(NewRenderSection);
														//rd.RenderSections[0] = NewRenderSection;
				}


				// normalize weight
				for (auto &w : meshWeight) {
					static int warnCount = 0;
					if (&w == &meshWeight[0]) {
						warnCount = 0;
					}
					int f = 0;
					int maxIndex = 0;
					int maxWeight = 0;
					for (int i = 0; i < 8; ++i) {
						f += w.InfluenceWeights[i];

						if (maxWeight < w.InfluenceWeights[i]) {
							maxWeight = w.InfluenceWeights[i];
							maxIndex = i;
						}
					}
					if (f > 255) {
						UE_LOG(LogTemp, Warning, TEXT("overr"));
						w.InfluenceWeights[0] -= (uint8)(f - 255);
					}
					if (f <= 254) {
						if (f <= (255-8)) {
							if (warnCount < 50) {
								UE_LOG(LogTemp, Warning, TEXT("less"));
								warnCount++;
							}
						}
						w.InfluenceWeights[maxIndex] += (uint8)(255 - f);
					}
				}

#if WITH_EDITORONLY_DATA
				{
					auto &s = sk->GetImportedModel()->LODModels[0].Sections[meshID];
					s.MaterialIndex = 0;

					TMap<int32, TArray<int32>> OverlappingVertices;

					{
						bool bUseMergeMaterial = Options::Get().IsMergeMaterial();
						if ((int)aiM->mMaterialIndex >= vrmAssetList->MaterialMergeTable.Num()) {
							bUseMergeMaterial = false;
						}
						if (bUseMergeMaterial) {
							s.MaterialIndex = vrmAssetList->MaterialMergeTable[aiM->mMaterialIndex];
						} else {
							s.MaterialIndex = aiM->mMaterialIndex;
						}
					}

					if (s.MaterialIndex >= vrmAssetList->Materials.Num()) s.MaterialIndex = 0;
					s.BaseIndex = currentIndex;
					s.NumTriangles = result.meshInfo[meshID].Triangles.Num() / 3;
					s.BaseVertexIndex = currentVertex;
					s.SoftVertices = meshWeight;
					if (bonemap.Num() > 0) {
						s.BoneMap.SetNum(bonemap.Num());//ModelSection.BoneMap;
						for (int i = 0; i < s.BoneMap.Num(); ++i) {
							s.BoneMap[i] = bonemap[i];
						}
					}else {
						s.BoneMap.SetNum(1);
						auto *p = GetNodeFromMeshID(meshID, mScenePtr);
						int32 i = k->GetReferenceSkeleton().FindBoneIndex(UTF8_TO_TCHAR(p->mName.C_Str()));
						if (i <= 0) {
							i = meshID;
						}
						s.BoneMap[0] = i;
					}
					s.NumVertices = meshWeight.Num();
					s.MaxBoneInfluences = 4;
				}
#endif

				//rd.MultiSizeIndexContainer.CopyIndexBuffer(result.meshInfo[0].Triangles);
				int t1 = Triangles.Num();
				Triangles.Append(result.meshInfo[meshID].Triangles);
				int t2 = Triangles.Num();

				for (int i = t1; i < t2; ++i) {
					Triangles[i] += currentVertex;
				}
				currentIndex += result.meshInfo[meshID].Triangles.Num();
				currentVertex += result.meshInfo[meshID].Vertices.Num();
				//rd.MultiSizeIndexContainer.update
			} // mesh loop

			if (Options::Get().IsMergePrimitive()) {
#if WITH_EDITORONLY_DATA
				// merge lod model section
				auto &LodModel = sk->GetImportedModel()->LODModels[0];
				for (int meshID = LodModel.Sections.Num() - 1; meshID>0; --meshID) {
					auto &s0 = LodModel.Sections[meshID-1];
					auto &s1 = LodModel.Sections[meshID];

					if (mScenePtr->mMeshes[meshID]->mNumAnimMeshes > 0) {
						// skip skin mesh
						continue;
					}
					if (s1.NumVertices == 0) {
						continue;
					}

					if (s0.MaterialIndex != s1.MaterialIndex) {
						continue;
					}
					auto newBoneMap = s0.BoneMap;
					for (auto &sv : s1.SoftVertices) {
						for (int i = 0; i < 8; ++i) {
							if (sv.InfluenceWeights[i] == 0) {
								continue;
							}
							int boneID = s1.BoneMap[sv.InfluenceBones[i]];

							int ind = 0;
							if (newBoneMap.Find(boneID, ind)) {
								sv.InfluenceBones[i] = ind;
							} else {
								sv.InfluenceBones[i] = newBoneMap.Add(boneID);
							}
						}
					}
					if (Options::Get().IsMobileBone()) {
						if (newBoneMap.Num() > 75) {
							continue;
						}
					}

					s0.SoftVertices.Append(s1.SoftVertices);
					s0.NumVertices += s1.NumVertices;
					s0.NumTriangles += s1.NumTriangles;

					s0.BoneMap = newBoneMap;

					s1.SoftVertices.SetNum(0);
					s1.NumVertices = 0;
				}
				for (int meshID = 0; meshID < LodModel.Sections.Num(); ++meshID) {
					auto &s1 = LodModel.Sections[meshID];

					if (s1.NumVertices > 0) {
						continue;
					}
					LodModel.Sections.RemoveAt(meshID);
					meshID--;
				}
#endif
			} // merge primitive

			if (1) {
				int warnCount = 0;
				for (auto &w : Weight) {
					int f = 0;
					int maxIndex = 0;
					int maxWeight = 0;
					for (int i = 0; i < 8; ++i) {
						f += w.InfluenceWeights[i];

						if (maxWeight < w.InfluenceWeights[i]) {
							maxWeight = w.InfluenceWeights[i];
							maxIndex = i;
						}
					}
					if (f > 255) {
						UE_LOG(LogTemp, Warning, TEXT("overr"));
						w.InfluenceWeights[0] -= (uint8)(f - 255);
					}
					if (f <= 254) {
						if (f <= (255 - 8)) {
							if (warnCount < 50) {
								UE_LOG(LogTemp, Warning, TEXT("less"));
								warnCount++;
							}
						}
						w.InfluenceWeights[maxIndex] += (uint8)(255 - f);
					}
				}
			}

#if WITH_EDITOR
			{
				FSkeletalMeshLODModel *p = &(sk->GetImportedModel()->LODModels[0]);
				p->NumVertices = allVertex;
				p->NumTexCoords = 1;// allVertex;
				p->IndexBuffer = Triangles;
				p->ActiveBoneIndices = rd.ActiveBoneIndices;
				p->RequiredBones = rd.RequiredBones;
			}
#endif
			//rd.StaticVertexBuffers.StaticMeshVertexBuffer.TexcoordDataPtr;

			if (VRMConverter::IsImportMode() == false) {
				ENQUEUE_RENDER_COMMAND(UpdateCommand)(
					[sk, Triangles, Weight](FRHICommandListImmediate& RHICmdList)
				{
					FSkeletalMeshLODRenderData &d = sk->GetResourceForRendering()->LODRenderData[0];

					if (d.MultiSizeIndexContainer.IsIndexBufferValid()) {
						d.MultiSizeIndexContainer.GetIndexBuffer()->ReleaseResource();
					}
					d.MultiSizeIndexContainer.RebuildIndexBuffer(sizeof(uint32), Triangles);
					d.MultiSizeIndexContainer.GetIndexBuffer()->InitResource();

					//d.AdjacencyMultiSizeIndexContainer.CopyIndexBuffer(Triangles);
					if (d.AdjacencyMultiSizeIndexContainer.IsIndexBufferValid()) {
						d.AdjacencyMultiSizeIndexContainer.GetIndexBuffer()->ReleaseResource();
					}
					d.AdjacencyMultiSizeIndexContainer.RebuildIndexBuffer(sizeof(uint32), Triangles);
					d.AdjacencyMultiSizeIndexContainer.GetIndexBuffer()->InitResource();

#if WITH_EDITOR
					d.SkinWeightVertexBuffer.Init(Weight);
#else
					{
						TArray< TSkinWeightInfo<false> > InWeights;
						InWeights.Reserve(Weight.Num());

						for (const auto &a : Weight) {
							auto n = new(InWeights) TSkinWeightInfo<false>;

							memcpy(n->InfluenceBones, a.InfluenceBones, sizeof(n->InfluenceBones));
							memcpy(n->InfluenceWeights, a.InfluenceWeights, sizeof(n->InfluenceWeights));
						}
						d.SkinWeightVertexBuffer = InWeights;
					}
#endif
					d.SkinWeightVertexBuffer.InitResource();

					d.StaticVertexBuffers.PositionVertexBuffer.UpdateRHI();
					d.StaticVertexBuffers.StaticMeshVertexBuffer.UpdateRHI();

					d.MultiSizeIndexContainer.GetIndexBuffer()->UpdateRHI();
					d.AdjacencyMultiSizeIndexContainer.GetIndexBuffer()->UpdateRHI();
					d.SkinWeightVertexBuffer.UpdateRHI();

					//src->GetResourceForRendering()->LODRenderData[0].rhi
					//v.PositionVertexBuffer.UpdateRHI();
				});
			}
		}
		{

			FBox BoundingBox(BoundMin, BoundMax);
			sk->SetImportedBounds(FBoxSphereBounds(BoundingBox));
		}

		{
#if WITH_EDITOR
			{
				sk->GetImportedModel()->LODModels[0].NumTexCoords = uvNum;
			}
			if (VRMConverter::IsImportMode()) {
				sk->UpdateUVChannelData(true);
			}
#endif
		}

#if WITH_EDITOR
		if (VRMConverter::IsImportMode()) {

			UProperty* ChangedProperty = FindField<UProperty>(USkeletalMesh::StaticClass(), "Materials");
			check(ChangedProperty);
			sk->PreEditChange(ChangedProperty);

			FPropertyChangedEvent PropertyUpdateStruct(ChangedProperty);
			sk->PostEditChangeProperty(PropertyUpdateStruct);

			sk->PostEditChange();
		}
#endif
	}

	UPhysicsAsset *pa = nullptr;
	{
		// remove
		if (vrmAssetList->Package == GetTransientPackage()) {
		} else {
			TArray<UObject*> ret;
			FString name = (TEXT("PHYS_") + vrmAssetList->BaseFileName);
			GetObjectsWithOuter(vrmAssetList->Package, ret);
			for (auto *a : ret) {
				auto s = a->GetName().ToLower();
				if (s.IsEmpty()) continue;

				if (s == name.ToLower()) {
					a->ClearFlags(EObjectFlags::RF_Standalone);
					a->SetFlags(EObjectFlags::RF_Public | RF_Transient);
					a->ConditionalBeginDestroy();
					break;
				}
			}
		}
	}
	if (mScenePtr->mVRMMeta && Options::Get().IsSkipPhysics()==false) {
		VRM::VRMMetadata *meta = reinterpret_cast<VRM::VRMMetadata*>(mScenePtr->mVRMMeta);
		if (meta->springNum > 0) {
			if (vrmAssetList->Package == GetTransientPackage()) {
				pa = NewObject<UPhysicsAsset>(GetTransientPackage(), NAME_None, EObjectFlags::RF_Public | RF_Transient, NULL);
			}else {
				FString name = (TEXT("PHYS_") + vrmAssetList->BaseFileName);
				pa = NewObject<UPhysicsAsset>(vrmAssetList->Package, *name, EObjectFlags::RF_Public | EObjectFlags::RF_Standalone);
			}
			pa->Modify();
#if WITH_EDITORONLY_DATA
			pa->SetPreviewMesh(sk);
#endif
			sk->PhysicsAsset = pa;

			addedList.Empty();
			{
				TArray<int> swingBoneIndexArray;

				for (int i = 0; i < meta->springNum; ++i) {
					auto &spring = meta->springs[i];
					for (int j = 0; j < spring.boneNum; ++j) {
						auto &sbone = spring.bones_name[j];

						FString s = UTF8_TO_TCHAR(sbone.C_Str());

						int sboneIndex = sk->RefSkeleton.FindRawBoneIndex(*s);
						if (sboneIndex == INDEX_NONE) {
							continue;
						}

						FName parentName = *s;

						//int parentIndex = sk->RefSkeleton.GetParentIndex(sboneIndex);
						//FName parentName = sk->RefSkeleton.GetBoneName(parentIndex);

						CreateSwingHead(vrmAssetList, spring, parentName, swingBoneIndexArray, sboneIndex);
						//aaaaaa(vrmAssetList, spring, parentName, swingBoneIndexArray, sboneIndex);

					}
				} // all spring
				{
					swingBoneIndexArray.Sort();

					for (int i = 0; i < swingBoneIndexArray.Num()-1; ++i) {
						for (int j = i+1; j < swingBoneIndexArray.Num(); ++j) {
							pa->DisableCollision(swingBoneIndexArray[i], swingBoneIndexArray[j]);
						}
					}
				}

				// collision
				for (int i = 0; i < meta->colliderGroupNum; ++i) {
					auto &c = meta->colliderGroups[i];

					USkeletalBodySetup *bs = nullptr;
					{
						FString s = UTF8_TO_TCHAR(c.node_name.C_Str());
						{
							int sboneIndex = sk->RefSkeleton.FindRawBoneIndex(*s);
							if (sboneIndex == INDEX_NONE) {
								continue;
							}
						}
						s = s.ToLower();

						// addlist changed recur...
						if (1){//addedList.Find(s) >= 0) {
							for (auto &a : pa->SkeletalBodySetups) {
								if (a->BoneName.IsEqual(*s)) {
									bs = a;
									break;
								}
							}

						}else {
							addedList.Add(s);
						}
					}
					if (bs == nullptr) {
						bs = NewObject<USkeletalBodySetup>(pa, NAME_None, RF_Transactional);
						bs->InvalidatePhysicsData();

						pa->SkeletalBodySetups.Add(bs);
					}

					FKAggregateGeom agg;
					for (int j = 0; j < c.colliderNum; ++j) {
						FKSphereElem SphereElem;
						VRM::vec3 v = {
							c.colliders[j].offset[0] * 100.f,
							c.colliders[j].offset[1] * 100.f,
							c.colliders[j].offset[2] * 100.f,
						};

						SphereElem.Center = FVector(-v[0], v[2], v[1]);
						SphereElem.Radius = c.colliders[j].radius * 100.f;
						agg.SphereElems.Add(SphereElem);
					}

					bs->Modify();
					bs->BoneName = UTF8_TO_TCHAR(c.node_name.C_Str());
					bs->AddCollisionFrom(agg);
					bs->CollisionTraceFlag = CTF_UseSimpleAsComplex;
					// newly created bodies default to simulating
					bs->PhysicsType = PhysType_Kinematic;	// fix!

															//bs.constra
															//sk->SkeletalBodySetups.Num();
					bs->CreatePhysicsMeshes();

#if WITH_EDITOR
					//pa->InvalidateAllPhysicsMeshes();
#endif
				}// collision
			}

			pa->UpdateBodySetupIndexMap();

			RefreshSkelMeshOnPhysicsAssetChange(sk);
#if WITH_EDITOR
			pa->RefreshPhysicsAssetChange();
#endif
			pa->UpdateBoundsBodiesArray();

#if WITH_EDITOR
			if (VRMConverter::IsImportMode()) {
				pa->PostEditChange();
			}
#endif

		}
		//FSkeletalMeshModel* ImportedResource = sk->GetImportedModel();
		//if (ImportedResource->LODModels.Num() == 0)
	}

	//NewAsset->FindSocket


#if WITH_EDITOR
	if (mScenePtr->mNumAnimations > 0){
		UAnimSequence *ase;
		ase = NewObject<UAnimSequence>(vrmAssetList->Package, *(TEXT("A_") + vrmAssetList->BaseFileName), EObjectFlags::RF_Public | EObjectFlags::RF_Standalone);

		ase->CleanAnimSequenceForImport();

		ase->SetSkeleton(k);

		float totalTime = 0.f;
		int totalFrameNum = 0;
		if (1){

			//TArray<AnimationTransformDebug::FAnimationTransformDebugData> TransformDebugData;

			for (uint32_t animNo = 0; animNo < mScenePtr->mNumAnimations; animNo++) {
				aiAnimation* aiA = mScenePtr->mAnimations[animNo];
				
				for (uint32_t chanNo = 0; chanNo < aiA->mNumChannels; chanNo++) {
					aiNodeAnim* aiNA = aiA->mChannels[chanNo];

					FRawAnimSequenceTrack RawTrack;

					if (chanNo == 0) {
						for (uint32_t i = 0; i < aiNA->mNumPositionKeys; ++i) {
							const auto &v = aiNA->mPositionKeys[i].mValue;
							//FVector pos(v.x, v.y, v.z);
							FVector pos(-v.x, v.z, v.y);
							//pos *= VRMConverter::Options::Get().GetModelScale();
							if (VRMConverter::Options::Get().IsVRMModel() == false) {
								pos.X *= -1.f;
								pos.Y *= -1.f;
							}
							RawTrack.PosKeys.Add(pos);

							totalTime = FMath::Max((float)aiNA->mPositionKeys[i].mTime, totalTime);
						}
						if (chanNo == 0) {
							if (RawTrack.PosKeys.Num()) {
								//ase->bEnableRootMotion = true;
							}
						}
					}

					if (chanNo > 0 || 1) {
						for (uint32_t i = 0; i < aiNA->mNumRotationKeys; ++i) {
							const auto &v = aiNA->mRotationKeys[i].mValue;

							FQuat q(v.x, v.y, v.z, v.w);

							

							if (1) {
								q = FQuat(-v.x, v.y, v.z, -v.w);

								{
									FQuat d = FQuat(FVector(1, 0, 0), -PI / 2.f);
									q = d * q * d.Inverse();
								}
							}

							//FQuat q(v.x, v.y, v.z, v.w);
							//FVector a = q.GetRotationAxis();
							//q = FQuat(FVector(0, 0, 1), PI) * q;
							//FMatrix m = q.GetRotationAxis
							//q = FRotator(0, 90, 0).Quaternion() * q;
							RawTrack.RotKeys.Add(q);

							totalTime = FMath::Max((float)aiNA->mRotationKeys[i].mTime, totalTime);
						}

						for (uint32_t i = 0; i < aiNA->mNumScalingKeys; ++i) {
							const auto &v = aiNA->mScalingKeys[i].mValue;
							FVector s(v.x, v.y, v.z);
							RawTrack.ScaleKeys.Add(s);

							totalTime = FMath::Max((float)aiNA->mScalingKeys[i].mTime, totalTime);
						}
					}

					if (RawTrack.RotKeys.Num() || RawTrack.PosKeys.Num() || RawTrack.ScaleKeys.Num()) {

						if (RawTrack.PosKeys.Num() == 0) {
							RawTrack.PosKeys.Add(FVector::ZeroVector);
						}
						if (RawTrack.RotKeys.Num() == 0) {
							RawTrack.RotKeys.Add(FQuat::Identity);
						}
						if (RawTrack.ScaleKeys.Num() == 0) {
							RawTrack.ScaleKeys.Add(FVector::OneVector);
						}

						int32 NewTrackIdx = ase->AddNewRawTrack(UTF8_TO_TCHAR(aiNA->mNodeName.C_Str()), &RawTrack);
					}

					totalFrameNum = FMath::Max(totalFrameNum, RawTrack.RotKeys.Num());
					totalFrameNum = FMath::Max(totalFrameNum, RawTrack.PosKeys.Num());

					totalTime = totalFrameNum / aiA->mTicksPerSecond;

#if	UE_VERSION_OLDER_THAN(4,22,0)
					ase->NumFrames = totalFrameNum;
#else
					ase->SetRawNumberOfFrame(totalFrameNum);
#endif
				}
			}
		}

		{
			//TArray<struct FRawAnimSequenceTrack>& RawAnimationData = ase->RawAnimationData;

			FRawAnimSequenceTrack RawTrack;
			RawTrack.PosKeys.Empty();
			RawTrack.RotKeys.Empty();
			RawTrack.ScaleKeys.Empty();

			RawTrack.PosKeys.Add(FVector(10, 10, 10));
			RawTrack.PosKeys.Add(FVector(20, 200, 200));

			RawTrack.RotKeys.Add(FQuat::Identity);
			RawTrack.RotKeys.Add(FQuat::Identity);

			RawTrack.ScaleKeys.Add(FVector(1, 1, 1));
			RawTrack.ScaleKeys.Add(FVector(1, 1, 1));


			//int32 NewTrackIdx = ase->AddNewRawTrack(TEXT("hip"), &RawTrack);
			//int32 NewTrackIdx = ase->AddNewRawTrack(RawTrack);
			//RawAnimationData.Add(RawTrack);
			//ase->AnimationTrackNames.Add(TEXT("chest"));


			//const int32 RefBoneIndex = k->GetReferenceSkeleton().FindBoneIndex(TEXT("chest"));
			//check(RefBoneIndex != INDEX_NONE);
			//ase->TrackToSkeletonMapTable.Add(FTrackToSkeletonMap(RefBoneIndex));

			//ase->NumFrames = 2;
			//ase->SequenceLength = 1;
			//ase->SequenceLength = totalTime / 60.f;
			ase->SequenceLength = totalTime;


			//AnimationAsset->AnimationTrackNames.Add(TEXT("chest"));


			ase->MarkRawDataAsModified();
		}

		const bool bSourceDataExists = ase->HasSourceRawData();
		if (bSourceDataExists)
		{
			ase->BakeTrackCurvesToRawAnimation();
		} else {
			ase->PostProcessSequence();
		}
		//AnimationTransformDebug::OutputAnimationTransformDebugData(TransformDebugData, TotalNumKeys, RefSkeleton);
	}
#endif

	return true;
}


VrmConvertModel::VrmConvertModel()
{
}

VrmConvertModel::~VrmConvertModel()
{
}
