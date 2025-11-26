#include "VoxelCutComputePass.h"
#include "CoreMinimal.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "ShaderParameterUtils.h"
#include "RHIStaticStates.h"
#include "Shader.h"
#include "RHI.h"
#include "GlobalShader.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "UniformBuffer.h"
#include "RHICommandList.h"
#include "ShaderCompilerCore.h"
#include "EngineDefines.h"
#include "RendererInterface.h"
#include "RenderResource.h"
#include "RenderGraphResources.h"
#include "MeshPassProcessor.inl"
#include "GlobalShader.h"
#include "RHIGPUReadback.h"

IMPLEMENT_GLOBAL_SHADER(FVoxelCutCS, TEXT("/Project/Shaders/VoxelCutCS.usf"), TEXT("MainCS"), SF_Compute);

void FVoxlCutShaderInterface::DispatchRenderThread(
		FRHICommandListImmediate& RHICmdList,
		TFunction<void(int OutputVal)> AsyncCallback
	)
{
	FRDGBuilder GraphBuilder(RHICmdList);
	{
		TShaderMapRef<FVoxelCutCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FVoxelCutCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVoxelCutCS::FParameters>();
		
	}
}