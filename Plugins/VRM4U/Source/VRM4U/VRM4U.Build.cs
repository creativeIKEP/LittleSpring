// VRM4U Copyright (c) 2019 Haruyoshi Yamamoto. This software is released under the MIT License.

using UnrealBuildTool;
using System.IO;

public class VRM4U : ModuleRules
{
    private string ModulePath
    {
        get { return ModuleDirectory; }
    }

    private string ThirdPartyPath
    {
        get { return Path.GetFullPath(Path.Combine(ModulePath, "../../ThirdParty/")); }
    }

    public VRM4U(ReadOnlyTargetRules Target) : base(Target)
	{
	    PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicIncludePaths.AddRange(
			new string[] {
                //"VRM4U/Public",
				// ... add public include paths required here ...
			}
		);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
                //"VRM4U/Private",
                // ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
                "CoreUObject",
                "Engine",
                "RHI",
                "RenderCore",
                "AnimGraphRuntime",

                // ... add other public dependencies that you statically link with here ...
			}
			);


        PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
                "Engine",
				"AssetRegistry",
				"CinematicCamera",
				// ... add private dependencies that you statically link with here ...	
			}
			);

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
            }
            );
    }
}
