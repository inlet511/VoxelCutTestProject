#pragma once

#include "CoreMinimal.h"
#include "BoxTypes.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

// 局部更新参数
BEGIN_SHADER_PARAMETER_STRUCT(FLocalUpdateParams, )
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
END_SHADER_PARAMETER_STRUCT()

// Marching Cubes参数
BEGIN_SHADER_PARAMETER_STRUCT(FMCParams, )
	SHADER_PARAMETER(FVector3f, SDFBoundsMin)
	SHADER_PARAMETER(FVector3f, SDFBoundsMax)
	SHADER_PARAMETER(FIntVector, SDFDimensions)
	SHADER_PARAMETER(float, IsoValue)
    
	// 生成区域（只生成变化区域的网格）
	SHADER_PARAMETER(FIntVector, GenerateRegionMin)
	SHADER_PARAMETER(FIntVector, GenerateRegionMax)
END_SHADER_PARAMETER_STRUCT()

// Compute Shader声明
class FLocalSDFUpdateCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FLocalSDFUpdateCS);
	SHADER_USE_PARAMETER_STRUCT(FLocalSDFUpdateCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLocalUpdateParams, Params)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, OriginalSDF)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, ToolSDF)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, DynamicSDF)
		SHADER_PARAMETER_SAMPLER(SamplerState, OriginalSDFSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, ToolSDFSampler)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FLocalMarchingCubesCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FLocalMarchingCubesCS);
	SHADER_USE_PARAMETER_STRUCT(FLocalMarchingCubesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FMCParams, Params)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, DynamicSDF)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector3f>, OutVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FIntVector>, OutTriangles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<int>, VertexCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<int>, TriangleCounter)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FLocalSDFUpdateCS, "/GPUSDFCutter/Private/LocalSDFUpdate.usf", "LocalSDFUpdateKernel", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLocalMarchingCubesCS, "/GPUSDFCutter/Private/LocalMarchingCubes.usf", "LocalMarchingCubesKernel", SF_Compute);