// VRM4U Copyright (c) 2019 Haruyoshi Yamamoto. This software is released under the MIT License.

#include "VRM4UImporterFactory.h"
#include "VRM4UImporterLog.h"
#include "AssetToolsModule.h"
#include "AssetRegistryModule.h"
#include "PackageTools.h"
#include "Misc/Paths.h"
#include "Engine/SkeletalMesh.h"
//#include "Runtime/CoreUObject/Public/UObject/ConstructorHelpers.h"
#include "UObject/ConstructorHelpers.h"
#include "LoaderBPFunctionLibrary.h"
#include "VrmAssetListObject.h"
#include "VrmRuntimeSettings.h"
#include "VrmOptionWindow.h"
#include "Engine/Blueprint.h"
#include "Templates/SharedPointer.h"
#include "Misc/FeedbackContext.h"
#include "FileHelpers.h"

#include "Interfaces/IMainFrameModule.h"
#include "Widgets/SWindow.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "VrmOptionWindow.h"
#include "VrmImportUI.h"
#include "VrmConvert.h"
#include "VrmLicenseObject.h"

#define LOCTEXT_NAMESPACE "VRMImporter"


/*
UCLASS(config=EditorPerProjectUserSettings, AutoExpandCategories=(FTransform), HideCategories=Object, MinimalAPI)
class UFbxImportUI : public UObject, public IImportSettingsParser
{
}

*/


namespace {
	void RenameAsset(UObject* Asset, const FString& NewName, FText& ErrorMessage)
	{
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		TArray<FAssetRenameData> AssetsAndNames;
		const FString PackagePath = FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName());
		new(AssetsAndNames) FAssetRenameData(Asset, PackagePath, NewName);
		AssetToolsModule.Get().RenameAssetsWithDialog(AssetsAndNames);
	}
}

UVRM4UImporterFactory::UVRM4UImporterFactory(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{

	TArray<FString> table = {
		TEXT("vrm"),
		TEXT("glb"),
		TEXT("bvh"),
	};

	{
		const UVrmRuntimeSettings* Settings = GetDefault<UVrmRuntimeSettings>();
		if (Settings) {
			table.Append(Settings->extList);
		}
	}


	for (auto &a : table) {
		Formats.Add(a + TEXT(";Model VRM4U"));
	}

	bCreateNew = false;
	bEditorImport = true;

	ImportPriority = FMath::Max(0, DefaultImportPriority - 10);
}


bool UVRM4UImporterFactory::FactoryCanImport(const FString& Filename)
{
	fullFileName.Empty();

	const FString Extension = FPaths::GetExtension(Filename);
	bool allowAll = false;

	const UVrmRuntimeSettings* Settings = GetDefault<UVrmRuntimeSettings>();
	if (Settings) {
		if (Settings->bAllowAllAssimpFormat) {
			allowAll = true;
		}
	}


	//if( Extension == TEXT("vrm") || Extension == TEXT("gltf") || Extension == TEXT("glb"))
	if(allowAll || Extension == TEXT("vrm") || Extension == TEXT("glb") || Extension == TEXT("bvh"))
	{
		fullFileName = Filename;
		return true;
	}
	return false;
}

UClass* UVRM4UImporterFactory::ResolveSupportedClass()
{
	UClass* ImportClass = USkeletalMesh::StaticClass();

	return ImportClass;
}

/*
UObject* UVRMImporterFactory::FactoryCreateFile
(
	UClass* Class,
	UObject* InParent,
	FName Name,
	EObjectFlags Flags,
	const FString& InFilename,
	const TCHAR* Parms,
	FFeedbackContext* Warn,
	bool& bOutOperationCanceled
)
{
	return nullptr;
}
*/
template<class T>
T* GetObjectFromStringAsset(FStringAssetReference const& AssetRef)
{
	UObject* AlreadyLoadedObj = AssetRef.ResolveObject();
	if (AlreadyLoadedObj)
	{
		return Cast<T>(AlreadyLoadedObj);
	}

	UObject* NewlyLoadedObj = AssetRef.TryLoad();
	if (NewlyLoadedObj)
	{
		return Cast<T>(NewlyLoadedObj);
	}

	return nullptr;
}


UObject* UVRM4UImporterFactory::FactoryCreateBinary(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, UObject* Context, const TCHAR* Type, const uint8*& Buffer, const uint8* BufferEnd, FFeedbackContext* Warn)
{
	if (fullFileName.IsEmpty()) {
		return nullptr;
	}
	
	static UVrmImportUI *ImportUI = nullptr;
	TAssetPtr<UObject> refPointerToLic;
	{
		if (ImportUI == nullptr){
			ImportUI = NewObject<UVrmImportUI>(this, NAME_None, RF_NoFlags);
			ImportUI->AddToRoot();
		}
		ImportUI->TitleAuthor.Empty();
		ImportUI->Thumbnail = nullptr;

		if (1){
			auto *p = ULoaderBPFunctionLibrary::GetVRMMeta(fullFileName);
			if (p) {
				ImportUI->TitleAuthor = TEXT("\"") + p->title + TEXT("\"") + TEXT(" / ") + TEXT("\"") + p->author + TEXT("\"");
				ImportUI->Thumbnail = p->thumbnail;

				refPointerToLic = p;
			}
		}


		TSharedPtr<SWindow> ParentWindow;

		if( FModuleManager::Get().IsModuleLoaded( "MainFrame" ) )
		{
			IMainFrameModule& MainFrame = FModuleManager::LoadModuleChecked<IMainFrameModule>( "MainFrame" );
			ParentWindow = MainFrame.GetParentWindow();
		}

		// Compute centered window position based on max window size, which include when all categories are expanded
		const float FbxImportWindowWidth = 410.0f*2.f;
		const float FbxImportWindowHeight = 750.0f;
		FVector2D FbxImportWindowSize = FVector2D(FbxImportWindowWidth, FbxImportWindowHeight); // Max window size it can get based on current slate


		FSlateRect WorkAreaRect = FSlateApplicationBase::Get().GetPreferredWorkArea();
		FVector2D DisplayTopLeft(WorkAreaRect.Left, WorkAreaRect.Top);
		FVector2D DisplaySize(WorkAreaRect.Right - WorkAreaRect.Left, WorkAreaRect.Bottom - WorkAreaRect.Top);

		float ScaleFactor = FPlatformApplicationMisc::GetDPIScaleFactorAtPoint(DisplayTopLeft.X, DisplayTopLeft.Y);
		FbxImportWindowSize *= ScaleFactor;

		FVector2D WindowPosition = (DisplayTopLeft + (DisplaySize - FbxImportWindowSize) / 2.0f) / ScaleFactor;


		TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(NSLOCTEXT("UnrealEd", "VRMImportOpionsTitle", "VRM Import Options"))
			.SizingRule(ESizingRule::Autosized)
			.AutoCenter(EAutoCenter::None)
			.ClientSize(FbxImportWindowSize)
			.ScreenPosition(WindowPosition);

		TSharedPtr<SVrmOptionWindow> VrmOptionWindow;
		Window->SetContent
		(
			SAssignNew(VrmOptionWindow, SVrmOptionWindow)
			.ImportUI(ImportUI)
			.WidgetWindow(Window)
			.FullPath(FText::FromString(fullFileName))
			//.ForcedImportType( bForceImportType ? TOptional<EFBXImportType>( ImportType ) : TOptional<EFBXImportType>() )
			//.IsObjFormat( bIsObjFormat )
			.MaxWindowHeight(FbxImportWindowHeight)
			.MaxWindowWidth(FbxImportWindowWidth)
		);

		// @todo: we can make this slow as showing progress bar later
		FSlateApplication::Get().AddModalWindow(Window, ParentWindow);

		if (VrmOptionWindow->ShouldImport() == false) {
			return nullptr;
		}

	}

	//static ConstructorHelpers::FObjectFinder<UObject> MatClass(TEXT("/Game/test/NewMaterial.NewMaterial"));
	//static ConstructorHelpers::FObjectFinder<UClass> MatClass(TEXT("Blueprint'/VRM4U/VrmObjectListBP.VrmObjectListBP_C'"));
	//static ConstructorHelpers::FObjectFinder<UVrmAssetListObject> MatClass(TEXT("Blueprint'/VRM4U/VrmObjectListBP'"));
	//static ConstructorHelpers::FObjectFinder<UObject> MatClass(TEXT("/VRM4U/VrmObjectListBP.VrmObjectListBP"));
	//UVrmAssetListObject *m = Cast<UVrmAssetListObject>(MatClass.Object);

	//UObject* objFinder = StaticLoadObject(UVrmAssetListObject::StaticClass(), nullptr, TEXT("/VRM4U/VrmObjectListBP.VrmObjectListBP_C"));
	//UObject* objFinder = NewObject<UVrmAssetListObject>(InParent, NAME_None, RF_Transactional);

	TAssetPtr<UVrmAssetListObject> m;
	TArray< TAssetPtr<UObject> > tt;

	tt.Add(InParent);

	//TRefCountPtr<UVrmAssetListObject> m;
	//UVrmAssetListObject *m = nullptr;
	TAssetPtr<UClass> c;
	{
		const UVrmRuntimeSettings* Settings = GetDefault<UVrmRuntimeSettings>();

		{
			FSoftObjectPath r = Settings->AssetListObject; //(TEXT("/VRM4U/VrmObjectListBP.VrmObjectListBP"));
			UObject *u = r.TryLoad();
			if (u) {
				if (Cast<UBlueprint>(u)) {
					c = (UClass*)(Cast<UBlueprint>(u)->GeneratedClass);
				}
			}
		}

		if (c == nullptr) {
			FSoftObjectPath r(TEXT("/VRM4U/VrmAssetListObjectBP.VrmAssetListObjectBP"));
			UObject *u = r.TryLoad();
			if (u) {
				c = (UClass*)(Cast<UBlueprint>(u)->GeneratedClass);
			}
		}

		if (c == nullptr) {
			c = UVrmAssetListObject::StaticClass();
		}

		m = NewObject<UVrmAssetListObject>((UObject*)GetTransientPackage(), c.Get());
	}

	//UVrmAssetListObject *m = Cast<UVrmAssetListObject>(u);
	//FSoftClassPath r(TEXT("/VRM4U/VrmObjectListBP.VrmObjectListBP"));
	//UObject *u = r.TryLoad();
	//auto aaa = NewObject<UObject>(c);

	if (m) {
		//auto a = NewObject<UVrmAssetListObject>(MatClass.Object, NAME_None, RF_Transactional);
		//MatClass.Object; 
		//ULoaderBPFunctionLibrary::LoadVRMFile(nullptr, fullFileName);

		GWarn->BeginSlowTask( NSLOCTEXT("UnrealEd", "ImportVRM", "Importing VRM"), true );

		int ret = true;
		auto &g = VRMConverter::Options::Get();
		g.SetVrmOption(ImportUI);
		ULoaderBPFunctionLibrary::SetImportMode(true, Cast<UPackage>(InParent));
		{
			UVrmAssetListObject *mret = nullptr;
			ret = ULoaderBPFunctionLibrary::LoadVRMFile(m.Get(), mret, fullFileName);
		}

		/*
		{
			const bool bCheckDirty = false;
			const bool bPromptToSave = false;
			TArray<UPackage* > p;
			p.Add(Cast<UPackage>(InParent));

			p[0]->Modify();
			p[0]->PostEditChange();
			p[0]->FullyLoad();
			p[0]->AddToRoot();
			FEditorFileUtils::PromptForCheckoutAndSave(p, bCheckDirty, bPromptToSave);

			p[0]->SetDirtyFlag(false);
			//p[0]->AddToRoot();

			//p[0]->Rename(*(p[0]->GetName() + TEXT("aatest")), nullptr);

			//ContentBrowserUtils::RenameAsset(p[0], NewName, ErrorMessage);
			FText ErrorMessage;
			//RenameAsset(p[0], *(p[0]->GetName() + TEXT("aatest")), ErrorMessage);
			RenameAsset(p[0], (TEXT("vrm_dummy_to_delete")), ErrorMessage);

			p[0]->Rename(nullptr, GetTransientPackage(), 0);
			p[0]->RemoveFromRoot();

			//FEditorFileUtils::PromptForCheckoutAndSave(p, bCheckDirty, bPromptToSave);
		}
		*/

		ULoaderBPFunctionLibrary::SetImportMode(false, nullptr);
		g.SetVrmOption(nullptr);

		GWarn->EndSlowTask();

		//bool bDeleteSucceeded = ObjectTools::DeleteSingleObject( ExistingObject );
		CollectGarbage( GARBAGE_COLLECTION_KEEPFLAGS );

		if (ret == false) {
			return nullptr;
		}
	}

	return InParent;
}
UObject* UVRM4UImporterFactory::FactoryCreateText(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, UObject* Context, const TCHAR* Type, const TCHAR*& Buffer, const TCHAR* BufferEnd, FFeedbackContext* Warn)
{
	return nullptr;
}

/*
UObject* UVRMImporterFactory::CreateNewAsset(UClass* AssetClass, const FString& TargetPath, const FString& DesiredName, EObjectFlags Flags)
{
	FAssetToolsModule& AssetToolsModule = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools");

	// Create a unique package name and asset name for the frame
	const FString TentativePackagePath = PackageTools::SanitizePackageName(TargetPath + TEXT("/") + DesiredName);
	FString DefaultSuffix;
	FString AssetName;
	FString PackageName;
	AssetToolsModule.Get().CreateUniqueAssetName(TentativePackagePath, DefaultSuffix,  PackageName,  AssetName);

	// Create a package for the asset
	UObject* OuterForAsset = CreatePackage(nullptr, *PackageName);

	// Create a frame in the package
	UObject* NewAsset = NewObject<UObject>(OuterForAsset, AssetClass, *AssetName, Flags);
	FAssetRegistryModule::AssetCreated(NewAsset);

	NewAsset->Modify();
	return NewAsset;
}
*/

/*
UObject* USpriterImporterFactory::ImportAsset(const FString& SourceFilename, const FString& TargetSubPath)
{
	FAssetToolsModule& AssetToolsModule = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools");

	TArray<FString> FileNames;
	FileNames.Add(SourceFilename);

	TArray<UObject*> ImportedAssets = AssetToolsModule.Get().ImportAssets(FileNames, TargetSubPath);
	return (ImportedAssets.Num() > 0) ? ImportedAssets[0] : nullptr;
}
*/

//////////////////////////////////////////////////////////////////////////

//#undef SPRITER_IMPORT_ERROR





////

UVrmImportUI::UVrmImportUI(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bIsReimport = false;
	//bAutomatedImportShouldDetectType = true;
	//Make sure we are transactional to allow undo redo
	this->SetFlags(RF_Transactional);

	/*
	StaticMeshImportData = CreateDefaultSubobject<UFbxStaticMeshImportData>(TEXT("StaticMeshImportData"));
	StaticMeshImportData->SetFlags(RF_Transactional);
	StaticMeshImportData->LoadOptions();

	SkeletalMeshImportData = CreateDefaultSubobject<UFbxSkeletalMeshImportData>(TEXT("SkeletalMeshImportData"));
	SkeletalMeshImportData->SetFlags(RF_Transactional);
	SkeletalMeshImportData->LoadOptions();

	AnimSequenceImportData = CreateDefaultSubobject<UFbxAnimSequenceImportData>(TEXT("AnimSequenceImportData"));
	AnimSequenceImportData->SetFlags(RF_Transactional);
	AnimSequenceImportData->LoadOptions();

	TextureImportData = CreateDefaultSubobject<UFbxTextureImportData>(TEXT("TextureImportData"));
	TextureImportData->SetFlags(RF_Transactional);
	TextureImportData->LoadOptions();
	*/
}


bool UVrmImportUI::CanEditChange( const UProperty* InProperty ) const
{
	bool bIsMutable = Super::CanEditChange( InProperty );
	/*
	if( bIsMutable && InProperty != NULL )
	{
		FName PropName = InProperty->GetFName();

		if(PropName == TEXT("StartFrame") || PropName == TEXT("EndFrame"))
		{
			bIsMutable = AnimSequenceImportData->AnimationLength == FBXALIT_SetRange && bImportAnimations;
		}
		else if(PropName == TEXT("bImportCustomAttribute") || PropName == TEXT("AnimationLength"))
		{
			bIsMutable = bImportAnimations;
		}

		if(bIsObjImport && InProperty->GetBoolMetaData(TEXT("OBJRestrict")))
		{
			bIsMutable = false;
		}
	}
*/
	return bIsMutable;
}

void UVrmImportUI::ParseFromJson(TSharedRef<class FJsonObject> ImportSettingsJson)
{
	/*
	// Skip instanced object references. 
	int64 SkipFlags = CPF_InstancedReference;
	FJsonObjectConverter::JsonObjectToUStruct(ImportSettingsJson, GetClass(), this, 0, SkipFlags);

	bAutomatedImportShouldDetectType = true;
	if(ImportSettingsJson->TryGetField("MeshTypeToImport").IsValid())
	{
		// Import type was specified by the user if MeshTypeToImport exists
		bAutomatedImportShouldDetectType = false;
	}

	const TSharedPtr<FJsonObject>* StaticMeshImportJson = nullptr;
	ImportSettingsJson->TryGetObjectField(TEXT("StaticMeshImportData"), StaticMeshImportJson);
	if(StaticMeshImportJson)
	{
		FJsonObjectConverter::JsonObjectToUStruct(StaticMeshImportJson->ToSharedRef(), StaticMeshImportData->GetClass(), StaticMeshImportData, 0, 0);
	}

	const TSharedPtr<FJsonObject>* SkeletalMeshImportJson = nullptr;
	ImportSettingsJson->TryGetObjectField(TEXT("SkeletalMeshImportData"), SkeletalMeshImportJson);
	if (SkeletalMeshImportJson)
	{
		FJsonObjectConverter::JsonObjectToUStruct(SkeletalMeshImportJson->ToSharedRef(), SkeletalMeshImportData->GetClass(), SkeletalMeshImportData, 0, 0);
	}

	const TSharedPtr<FJsonObject>* AnimImportJson = nullptr;
	ImportSettingsJson->TryGetObjectField(TEXT("AnimSequenceImportData"), AnimImportJson);
	if (AnimImportJson)
	{
		FJsonObjectConverter::JsonObjectToUStruct(AnimImportJson->ToSharedRef(), AnimSequenceImportData->GetClass(), AnimSequenceImportData, 0, 0);
	}

	const TSharedPtr<FJsonObject>* TextureImportJson = nullptr;
	ImportSettingsJson->TryGetObjectField(TEXT("TextureImportData"), TextureImportJson);
	if (TextureImportJson)
	{
		FJsonObjectConverter::JsonObjectToUStruct(TextureImportJson->ToSharedRef(), TextureImportData->GetClass(), TextureImportData, 0, 0);
	}
	*/
}

void UVrmImportUI::ResetToDefault()
{
	ReloadConfig();
}





#undef LOCTEXT_NAMESPACE