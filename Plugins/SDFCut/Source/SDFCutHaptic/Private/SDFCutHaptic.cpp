// Copyright Epic Games, Inc. All Rights Reserved.

#include "SDFCutHaptic.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FSDFCutHapticModule"

void FSDFCutHapticModule::StartupModule()
{
}

void FSDFCutHapticModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FSDFCutHapticModule, SDFCutHaptic)