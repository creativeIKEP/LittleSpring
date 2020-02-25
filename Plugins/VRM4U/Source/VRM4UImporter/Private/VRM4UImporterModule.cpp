
#include "CoreMinimal.h"
#include "VRM4UImporterLog.h"
#include "VrmAssetListThumbnailRenderer.h"
#include "VrmAssetListObject.h"
#include "VrmLicenseObject.h"
#include "VrmMetaObject.h"

#include "AssetToolsModule.h"
#include "Modules/ModuleManager.h"
#include "Internationalization/Internationalization.h"
#include "ThumbnailRendering/ThumbnailManager.h"


#define LOCTEXT_NAMESPACE "VRM4UImporter"

DEFINE_LOG_CATEGORY(LogVRM4UImporter);

//////////////////////////////////////////////////////////////////////////
// FSpriterImporterModule

class FVRM4UImporterModule : public FDefaultModuleImpl
{
public:
	virtual void StartupModule() override
	{
		{
			auto &a = UThumbnailManager::Get();
			a.RegisterCustomRenderer(UVrmAssetListObject::StaticClass(), UVrmAssetListThumbnailRenderer::StaticClass());
			a.RegisterCustomRenderer(UVrmLicenseObject::StaticClass(), UVrmAssetListThumbnailRenderer::StaticClass());
			a.RegisterCustomRenderer(UVrmMetaObject::StaticClass(), UVrmAssetListThumbnailRenderer::StaticClass());
		}

		{
			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
			AssetTools.RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_VrmAssetList));
			AssetTools.RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_VrmLicense));
			AssetTools.RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_VrmMeta));
		}
	}

	virtual void ShutdownModule() override
	{
		if (UObjectInitialized()){
			UThumbnailManager::Get().UnregisterCustomRenderer(UVrmAssetListObject::StaticClass());
			UThumbnailManager::Get().UnregisterCustomRenderer(UVrmLicenseObject::StaticClass());
			UThumbnailManager::Get().UnregisterCustomRenderer(UVrmMetaObject::StaticClass());
		}
	}
};

//////////////////////////////////////////////////////////////////////////

IMPLEMENT_MODULE(FVRM4UImporterModule, VRM4UImporter);

//////////////////////////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE
