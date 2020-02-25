// VRM4U Copyright (c) 2019 Haruyoshi Yamamoto. This software is released under the MIT License.

#include "VrmAssetListThumbnailRenderer.h"
#include "Engine/EngineTypes.h"
#include "CanvasItem.h"
#include "Engine/Texture2D.h"
#include "CanvasTypes.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Brushes/SlateColorBrush.h"

#include "VrmAssetListObject.h"
#include "VrmLicenseObject.h"
#include "VrmMetaObject.h"

//////////////////////////////////////////////////////////////////////////
// UPaperSpriteThumbnailRenderer

UClass* FAssetTypeActions_VrmAssetList::GetSupportedClass() const {
	return UVrmAssetListObject::StaticClass();
}
FText FAssetTypeActions_VrmAssetList::GetName() const {
	return NSLOCTEXT("AssetTypeActions", "FAssetTypeActions_VrmAssetList", "Vrm Asset List");
}
UClass* FAssetTypeActions_VrmLicense::GetSupportedClass() const {
	return UVrmLicenseObject::StaticClass();
}
FText FAssetTypeActions_VrmLicense::GetName() const {
	return NSLOCTEXT("AssetTypeActions", "FAssetTypeActions_VrmLicense", "Vrm License");
}
UClass* FAssetTypeActions_VrmMeta::GetSupportedClass() const {
	return UVrmMetaObject::StaticClass();
}
FText FAssetTypeActions_VrmMeta::GetName() const {
	return NSLOCTEXT("AssetTypeActions", "FAssetTypeActions_VrmMeta", "Vrm Meta");
}

TSharedPtr<SWidget> FAssetTypeActions_VrmBase::GetThumbnailOverlay(const FAssetData& AssetData) const {

	FString str;
	FColor col(0, 0, 0, 0);

	if (str.Len() == 0) {
		TWeakObjectPtr<UVrmAssetListObject> a = Cast<UVrmAssetListObject>(AssetData.GetAsset());
		if (a.Get()) {
			str = TEXT(" AssetList ");
		}
	}
	if (str.Len() == 0){
		TWeakObjectPtr<UVrmLicenseObject> a = Cast<UVrmLicenseObject>(AssetData.GetAsset());
		if (a.Get()) {
			str = TEXT(" License ");
			col.A = 128;
		}
	}
	if (str.Len() == 0) {
		TWeakObjectPtr<UVrmMetaObject> a = Cast<UVrmMetaObject>(AssetData.GetAsset());
		if (a.Get()) {
			str = TEXT(" Meta ");
			col.A = 128;
		}
	}

	FText txt = FText::FromString(str);
	return SNew(SBorder)
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		//.Padding(FMargin(4))
		//.Padding(FMargin(4))
		//.BorderImage(new FSlateColorBrush(FColor::White))
		.BorderImage(new FSlateColorBrush(col))
		//.AutoWidth()
		[
			SNew(STextBlock)
			.Text(txt)
			.HighlightText(txt)
			.HighlightColor(FColor(64,64,64))
			.ShadowOffset(FVector2D(1.0f, 1.0f))
		];

	//return FAssetTypeActions_Base::GetThumbnailOverlay(AssetData);
}


UVrmAssetListThumbnailRenderer::UVrmAssetListThumbnailRenderer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UVrmAssetListThumbnailRenderer::GetThumbnailSize(UObject* Object, float Zoom, uint32& OutWidth, uint32& OutHeight) const {
	UVrmAssetListObject* a = Cast<UVrmAssetListObject>(Object);

	if (a) {
		if (a->VrmLicenseObject) {
			auto tex = a->SmallThumbnailTexture;
			if (tex == nullptr) {
				tex = a->VrmLicenseObject->thumbnail;
			}
			if (tex) {
				return Super::GetThumbnailSize(tex, Zoom, OutWidth, OutHeight);
			}
	}
	}
	Super::GetThumbnailSize(Object, Zoom, OutWidth, OutHeight);
}


void UVrmAssetListThumbnailRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas)
{
	UTexture2D *tex = nullptr;

	if (tex == nullptr){
		UVrmAssetListObject* a = Cast<UVrmAssetListObject>(Object);
		if (a) {
			tex = a->SmallThumbnailTexture;
			if (tex == nullptr) {
				if (a->VrmLicenseObject) {
					tex = a->VrmLicenseObject->thumbnail;
				}
			}
		}
	}
	if (tex == nullptr) {
		TArray<UObject*> ret;
		{
			UVrmMetaObject* a = Cast<UVrmMetaObject>(Object);
			if (a) {
				UPackage *pk = a->GetOutermost();
				GetObjectsWithOuter(pk, ret);
			}
		}
		{
			UVrmLicenseObject* a = Cast<UVrmLicenseObject>(Object);
			if (a) {
				UPackage *pk = a->GetOutermost();
				GetObjectsWithOuter(pk, ret);
			}
		}

		for (auto *obj : ret) {
			UVrmAssetListObject* t = Cast<UVrmAssetListObject>(obj);
			if (t == nullptr) {
				continue;
			}
			tex = t->SmallThumbnailTexture;
			if (tex == nullptr) {
				if (t->VrmLicenseObject) {
					tex = t->VrmLicenseObject->thumbnail;
				}
			}
			break;
		}
		if (tex == nullptr) {
			UVrmLicenseObject* a = Cast<UVrmLicenseObject>(Object);
			if (a) {
				tex = a->thumbnail;
			}
		}
	}

	if (tex) {
		return Super::Draw(tex, X, Y, Width, Height, RenderTarget, Canvas);
	}

	return Super::Draw(Object, X, Y, Width, Height, RenderTarget, Canvas);
}

