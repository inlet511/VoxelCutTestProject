#pragma once 
#include "CoreMinimal.h"
#include "ShaderParameterStruct.h"
#include "ToolSDFGenerator.h"

struct FlatOctreeNode
{
	FVector BoundsMin;
	FVector BoundsMax;
	float Voxels[8]; // 固定成8个体素
};


// 发送给Dispatch函数的参数
struct VOXELCUTSHADERS_API FVoxelCutCSParams
{
	TSharedPtr<FToolSDFGenerator> ToolSDF;
	TArray<FlatOctreeNode> OctreeNodesArray;
	FTransform ToolTransform;
};


// 定义ToolTransform UniformBuffer,用于传递ToolTransform
BEGIN_UNIFORM_BUFFER_STRUCT(FToolTransformUniformBuffer, )
	SHADER_PARAMETER(FMatrix44f, ToolTransform)
END_UNIFORM_BUFFER_STRUCT()

// 添加SDF边界UniformBuffer定义
BEGIN_UNIFORM_BUFFER_STRUCT(FSDFBoundsUniformBuffer, )
	SHADER_PARAMETER(FVector3f, SDFBoundsMin)
	SHADER_PARAMETER(FVector3f, SDFBoundsMax)
	SHADER_PARAMETER(int32, VolumeTextureSize)
END_UNIFORM_BUFFER_STRUCT()

class FVoxelCutCS: public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelCutCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelCutCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_TEXTURE(Texture3D<int16>, ToolSDF)
		SHADER_PARAMETER_SAMPLER(SamplerState, ToolSDFSampler)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FlatOctreeNode>, InputBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FlatOctreeNode>, OutputBuffer)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FToolTransformUniformBuffer, ToolTransformUniformBuffer)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSDFBoundsUniformBuffer, SDFBoundsUniformBuffer)
	END_SHADER_PARAMETER_STRUCT()


static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 1);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);		
	}
};



// This is a public interface that we define so outside code can invoke our compute shader.
class VOXELCUTSHADERS_API FVoxlCutShaderInterface {
public:
	// Executes this shader on the render thread
	static void DispatchRenderThread(FVoxelCutCSParams Params,FRHICommandListImmediate& RHICmdList,TFunction<void(TArray<FlatOctreeNode>)> AsyncCallback);

	// Executes this shader on the render thread from the game thread via EnqueueRenderThreadCommand
	static void DispatchGameThread(FVoxelCutCSParams Params,TFunction<void(TArray<FlatOctreeNode>)> AsyncCallback)
	{
		ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
		[Params, AsyncCallback](FRHICommandListImmediate& RHICmdList)
		{
			DispatchRenderThread(Params, RHICmdList, AsyncCallback);
		});
	}

	// Dispatches this shader. Can be called from any thread
	static void Dispatch(FVoxelCutCSParams Params, TFunction<void(TArray<FlatOctreeNode>)> AsyncCallback)
	{
		if (IsInRenderingThread()) {
			DispatchRenderThread(Params, GetImmediateCommandList_ForRenderCommand(), AsyncCallback);
		}else{
			DispatchGameThread(Params, AsyncCallback);
		}
	}

};

