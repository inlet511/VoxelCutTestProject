// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelCutShaders.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FVoxelCutShadersModule"

void FVoxelCutShadersModule::StartupModule()
{
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("VoxelCut"))->GetBaseDir(), TEXT("Shaders"));	
    AddShaderSourceDirectoryMapping(TEXT("/Project/Shaders"), PluginShaderDir);    
}

void FVoxelCutShadersModule::ShutdownModule()
{
	
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FVoxelCutShadersModule, VoxelCutShaders)