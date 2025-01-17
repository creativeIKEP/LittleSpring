// VRM4U Copyright (c) 2019 Haruyoshi Yamamoto. This software is released under the MIT License.

#include "Engine.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleInterface.h"


#if PLATFORM_WINDOWS

#include "Windows/AllowWindowsPlatformTypes.h"
#include "shellapi.h"
#include "Windows/HideWindowsPlatformTypes.h"

#include "Windows/WindowsApplication.h"
#include "GenericPlatform/GenericApplication.h"
#include "VrmDropFiles.h"
#include "VrmRuntimeSettings.h"
#include "HAL/FileManager.h"


class FDropMessageHandler : public IWindowsMessageHandler { 
public:

	virtual bool ProcessMessage(HWND hwnd, uint32 msg, WPARAM wParam, LPARAM lParam, int32& OutResult) override {

		int32 NumFiles;
		HDROP hDrop;
		TCHAR NextFile[MAX_PATH];

		{
			const UObject *p = UVrmDropFilesComponent::getLatestActiveComponent();
			const UWorld* World = nullptr;
			if (p) {
				World = GEngine->GetWorldFromContextObject(p, EGetWorldErrorMode::ReturnNull);
			}

			{
				bool bCurrentState = false;
				if (World) {

					switch (World->WorldType) {
					case EWorldType::Game:
					case EWorldType::PIE:
						bCurrentState = true;
						break;
					}

				}
				static bool bDropEnable = false;
				if (bDropEnable != bCurrentState) {
					bDropEnable = bCurrentState;
					DragAcceptFiles(hwnd, bDropEnable);
					//DragAcceptFiles(GetForegroundWindow(), bDropEnable);
					DragAcceptFiles(GetActiveWindow(), bDropEnable);
				}
			}
		}

		if (msg == WM_DROPFILES)
		{
			hDrop = (HDROP)wParam;
			// Get the # of files being dropped.
			NumFiles = DragQueryFile(hDrop, -1, NULL, 0);

			for (int32 File = 0; File < NumFiles; File++)
			{
				// Get the next filename from the HDROP info.
				if (DragQueryFile(hDrop, File, NextFile, MAX_PATH) > 0)
				{
					FString Filepath = NextFile;
					//Do whatever you want with the filepath

					UVrmDropFilesComponent::StaticOnDropFilesDelegate.Broadcast(Filepath);

				}
			}
			return false;
		}    
		return false;
	}
};
#endif


#define LOCTEXT_NAMESPACE "FVRM4ULoaderModule"

class FVRM4ULoaderModule : public IModuleInterface {
#if PLATFORM_WINDOWS
	void *assimpDllHandle = nullptr;
#endif

	void StartupModule()
	{
		// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module

#if PLATFORM_WINDOWS
		const UVrmRuntimeSettings* Settings = GetDefault<UVrmRuntimeSettings>();
		if (Settings) {
			if (Settings->bDropVRMFileEnable) {
				static FDropMessageHandler DropMessageHandler;// = FDropMessageHandler();
															  //FWindowsApplication* WindowsApplication = (FWindowsApplication*)GenericApplication.Get();
				TSharedPtr<GenericApplication> App = FSlateApplication::Get().GetPlatformApplication();

				auto WindowsApplication = (FWindowsApplication*)(App.Get());

				//WindowsApplication->SetMessageHandler(DropMessageHandler);
				WindowsApplication->AddMessageHandler(DropMessageHandler);
			}
		}

		{
			{
				FString AbsPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*(FPaths::ProjectPluginsDir() / TEXT("VRM4U/ThirdParty/assimp/bin/x64")));
				//FPlatformProcess::AddDllDirectory(*AbsPath);
				assimpDllHandle = FPlatformProcess::GetDllHandle(*(AbsPath / TEXT("assimp-vc141-mt.dll")));
			}

			if (assimpDllHandle == nullptr) {
				FString AbsPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*(FPaths::EnginePluginsDir() / TEXT("VRM4U/ThirdParty/assimp/bin/x64")));
				//FPlatformProcess::AddDllDirectory(*AbsPath);
				assimpDllHandle = FPlatformProcess::GetDllHandle(*(AbsPath / TEXT("assimp-vc141-mt.dll")));
			}
		}

#endif

	}

	void ShutdownModule()
	{
		// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
		// we call this function before unloading the module.

#if PLATFORM_WINDOWS
		if (assimpDllHandle){
			FPlatformProcess::FreeDllHandle(assimpDllHandle);
			assimpDllHandle = nullptr;
		}
#endif
	}
};

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FVRM4ULoaderModule, VRM4ULoader)