// Copyright Epic Games, Inc. All Rights Reserved.

#include "SDFCut.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FSDFCutModule"

void FSDFCutModule::StartupModule()
{
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("SDFCut"))->GetBaseDir(), TEXT("Shaders"));	
    AddShaderSourceDirectoryMapping(TEXT("/SDF/Shaders"), PluginShaderDir);  
}

void FSDFCutModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FSDFCutModule, SDFCut)