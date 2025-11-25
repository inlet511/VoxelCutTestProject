// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelCut.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FVoxelCutModule"

void FVoxelCutModule::StartupModule()
{
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("VoxelCut"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Project/Shaders"),PluginShaderDir);
}

void FVoxelCutModule::ShutdownModule()
{
	
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FVoxelCutModule, VoxelCut)