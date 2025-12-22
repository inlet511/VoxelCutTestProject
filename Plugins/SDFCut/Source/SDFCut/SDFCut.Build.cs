// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class SDFCut : ModuleRules
{
	public SDFCut(ReadOnlyTargetRules Target) : base(Target)
	{
		//bUsePrecompiled = true;
		
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"GeometryCore", "GeometryFramework",
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Renderer",
				"RenderCore",
				"RHI",
				"GeometryFramework", 
				"Projects", 
				"MeshConversion"
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
