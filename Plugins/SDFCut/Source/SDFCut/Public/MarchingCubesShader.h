#pragma once
#include "CoreMinimal.h"
#include "BoxTypes.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

// Marching Cubes参数
BEGIN_UNIFORM_BUFFER_STRUCT(FMCUB, )
	SHADER_PARAMETER(FVector3f, SDFBoundsMin)
	SHADER_PARAMETER(FVector3f, SDFBoundsMax)
	SHADER_PARAMETER(FIntVector, SDFDimensions)
	SHADER_PARAMETER(float, CubeSize)
	SHADER_PARAMETER(float, IsoValue)
    
	// 生成区域（只生成变化区域的网格）
	SHADER_PARAMETER(FIntVector, GenerateRegionMin)
	SHADER_PARAMETER(FIntVector, GenerateRegionMax)
END_UNIFORM_BUFFER_STRUCT()


class FLocalMarchingCubesCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FLocalMarchingCubesCS);
	SHADER_USE_PARAMETER_STRUCT(FLocalMarchingCubesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FMCUB, Params)
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