#pragma once
#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

// 局部更新参数
BEGIN_UNIFORM_BUFFER_STRUCT(FCutUB, )
	// 物体和工具的本地边界
	SHADER_PARAMETER(FVector3f, ObjectLocalBoundsMin)
	SHADER_PARAMETER(FVector3f, ObjectLocalBoundsMax)
	SHADER_PARAMETER(FVector3f, ToolLocalBoundsMin)
	SHADER_PARAMETER(FVector3f, ToolLocalBoundsMax)
	    
	// 物体局部坐标系到工具局部坐标系的变换
	SHADER_PARAMETER(FMatrix44f, ObjectToToolTransform)
	SHADER_PARAMETER(FMatrix44f, ToolToObjectTransform)
	    
	// SDF参数
	SHADER_PARAMETER(FIntVector, SDFDimensions)
	SHADER_PARAMETER(float, VoxelSize)
	SHADER_PARAMETER(float, IsoValue)
	    
	// 更新区域（物体局部坐标系的体素范围）
	SHADER_PARAMETER(FIntVector, UpdateRegionMin)
	SHADER_PARAMETER(FIntVector, UpdateRegionMax)
END_UNIFORM_BUFFER_STRUCT()

// Compute Shader声明
class FUpdateSDFCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUpdateSDFCS);
	SHADER_USE_PARAMETER_STRUCT(FUpdateSDFCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FCutUB, Params)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, OriginalSDF)
		SHADER_PARAMETER_SAMPLER(SamplerState, OriginalSDFSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, ToolSDF)
		SHADER_PARAMETER_SAMPLER(SamplerState, ToolSDFSampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, DynamicSDF)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};