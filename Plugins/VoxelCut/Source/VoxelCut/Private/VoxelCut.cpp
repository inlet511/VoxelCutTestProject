// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelCut.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FVoxelCutModule"

void FVoxelCutModule::StartupModule()
{	
}

void FVoxelCutModule::ShutdownModule()
{
	
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FVoxelCutModule, VoxelCut)