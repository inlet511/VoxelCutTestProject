#pragma once 
#include "CoreMinimal.h"
#include "ShaderParameterStruct.h"
#include "ToolSDFGenerator.h"


struct FlatOctreeNode
{
	float BoundsMin[3];
	float BoundsMax[3];
	float Voxel;
};


// 发送给Dispatch函数的参数
struct VOXELCUTSHADERS_API FVoxelCutCSParams
{
	TSharedPtr<FToolSDFGenerator> ToolSDFGenerator;
	TArray<FlatOctreeNode> OctreeNodesArray;
	FTransform ToolTransform;
};


// 定义ToolTransform UniformBuffer,用于传递ToolTransform
BEGIN_UNIFORM_BUFFER_STRUCT(FToolUB, )
	SHADER_PARAMETER(FMatrix44f, ToolInverseTransform)
	SHADER_PARAMETER(FVector3f, ToolBoundsLocalMin)
	SHADER_PARAMETER(FVector3f, ToolBoundsLocalMax)
	SHADER_PARAMETER(int32, VolumeTextureSize)
END_UNIFORM_BUFFER_STRUCT()


class FVoxelCutCS: public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelCutCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelCutCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_TEXTURE(Texture3D<float>, ToolSDF)
		SHADER_PARAMETER_SAMPLER(SamplerState, ToolSDFSampler)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FlatOctreeNode>, InputBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FlatOctreeNode>, OutputBuffer)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FToolUB, ToolUB)
	END_SHADER_PARAMETER_STRUCT()


static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 64);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 1);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};



// This is a public interface that we define so outside code can invoke our compute shader.
class VOXELCUTSHADERS_API FVoxlCutShaderInterface {
public:
	// Executes this shader on the render thread
	static void DispatchRenderThread(FRHICommandListImmediate& RHICmdList, FVoxelCutCSParams Params,TFunction<void(TArray<FlatOctreeNode>)> AsyncCallback);

	// Executes this shader on the render thread from the game thread via EnqueueRenderThreadCommand
	static void DispatchGameThread(FVoxelCutCSParams Params,TFunction<void(TArray<FlatOctreeNode>)> AsyncCallback)
	{
		ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
		[Params, AsyncCallback](FRHICommandListImmediate& RHICmdList)
		{
			DispatchRenderThread(RHICmdList, Params, AsyncCallback);
		});
	}

	// Dispatches this shader. Can be called from any thread
	static void Dispatch(FVoxelCutCSParams Params, TFunction<void(TArray<FlatOctreeNode>)> AsyncCallback)
	{
		if (IsInRenderingThread()) {
			DispatchRenderThread(GetImmediateCommandList_ForRenderCommand(), Params, AsyncCallback);
		}else{
			DispatchGameThread(Params, AsyncCallback);
		}
	}

};

