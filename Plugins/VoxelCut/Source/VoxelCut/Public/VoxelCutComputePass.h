#pragma once 
#include "CoreMinimal.h"
#include "ShaderParameterStruct.h"

struct OctreeNodeToGPU
{
	FVector BoundsMin;
	FVector BoundsMax;
	float Voxels[8]; // 固定成8个体素
};

// 定义Uniform Buffer,用于传递ToolTransform
BEGIN_UNIFORM_BUFFER_STRUCT(FMyUniform, )
SHADER_PARAMETER(FMatrix, ToolTransform)
END_UNIFORM_BUFFER_STRUCT()
IMPLEMENT_UNIFORM_BUFFER_STRUCT(FMyUniform, "MyUniform")

class FVoxelCutCS: public FGlobalShader
{
	DECLARE_GLBAL_SHADER(FVoxelCutCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelCutCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<OctreeNodeToGPU>, OctreeNodeArrayBuffer)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FMyUniform,UniformBuffer)
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
class COMPUTESHADER_API FExampleComputeShaderInterface {
public:
	// Executes this shader on the render thread
	static void DispatchRenderThread(
		FRHICommandListImmediate& RHICmdList,
		FExampleComputeShaderDispatchParams Params,
		TFunction<void(int OutputVal)> AsyncCallback
	);

	// Executes this shader on the render thread from the game thread via EnqueueRenderThreadCommand
	static void DispatchGameThread(
		FExampleComputeShaderDispatchParams Params,
		TFunction<void(int OutputVal)> AsyncCallback
	)
	{
		ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
		[Params, AsyncCallback](FRHICommandListImmediate& RHICmdList)
		{
			DispatchRenderThread(RHICmdList, Params, AsyncCallback);
		});
	}

	// Dispatches this shader. Can be called from any thread
	static void Dispatch(
		FExampleComputeShaderDispatchParams Params,
		TFunction<void(int OutputVal)> AsyncCallback
	)
	{
		if (IsInRenderingThread()) {
			DispatchRenderThread(GetImmediateCommandList_ForRenderCommand(), Params, AsyncCallback);
		}else{
			DispatchGameThread(Params, AsyncCallback);
		}
	}
};

