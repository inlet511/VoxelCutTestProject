#pragma once

#include "CoreMinimal.h"
#include "BoxTypes.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

BEGIN_UNIFORM_BUFFER_STRUCT(FCutterUniform, )
	SHADER_PARAMETER(FMatrix44f, ToolTransform)
	SHADER_PARAMETER(FMatrix44f, ToolInverseTransform)
	SHADER_PARAMETER(FVector3f, ToolAABBMin)
	SHADER_PARAMETER(FVector3f, ToolAABBMax)
	SHADER_PARAMETER(float, IsoValue)
	SHADER_PARAMETER(float, CubeSize)
END_UNIFORM_BUFFER_STRUCT()

// SDF纹理参数
BEGIN_UNIFORM_BUFFER_STRUCT(FSDFTextureUniform, )
	SHADER_PARAMETER_TEXTURE(Texture3D<float>, OriginalSDF)  // 原始物体SDF纹理
	SHADER_PARAMETER_SAMPLER(SamplerState, OriginalSDFSampler)
	SHADER_PARAMETER_TEXTURE(Texture3D<float>, ToolSDF)      // 切削工具SDF纹理
	SHADER_PARAMETER_SAMPLER(SamplerState, ToolSDFSampler)
	SHADER_PARAMETER_TEXTURE(RWTexture3D<float>, DynamicSDF) // 动态更新的SDF（可写）
END_UNIFORM_BUFFER_STRUCT()

// Marching Cubes输出参数
BEGIN_UNIFORM_BUFFER_STRUCT(FMCOutputUniform, )
	SHADER_PARAMETER_UAV(RWStructuredBuffer<float3>, OutVertices)  // 顶点缓冲区
	SHADER_PARAMETER_UAV(RWStructuredBuffer<int3>, OutTriangles)  // 三角形缓冲区
	SHADER_PARAMETER_UAV(RWBuffer<int>, VertexCounter)            // 顶点计数器
	SHADER_PARAMETER_UAV(RWBuffer<int>, TriangleCounter)          // 三角形计数器
END_UNIFORM_BUFFER_STRUCT()

// 合并所有Shader参数
struct FCutterShaderParams
{
	FCutterUniform GlobalParams;
	FSDFTextureUniform SDFParams;
	FMCOutputUniform MCOutputParams;
	UE::Geometry::FAxisAlignedBox3d DynamicSDFBounds;  // 动态SDF的整体范围
	FIntVector DynamicSDFDimensions;   // 动态SDF的分辨率（体素数量）
};

class FDynamicSDFUpdateCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDynamicSDFUpdateCS);
	SHADER_USE_PARAMETER_STRUCT(FDynamicSDFUpdateCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(Parameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FCutterUniform, GlobalParams)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSDFTextureUniform, SDFParams)
		SHADER_PARAMETER(FVector3f, DynamicSDFBoundsMin)
		SHADER_PARAMETER(FVector3f, DynamicSDFBoundsMax)
		SHADER_PARAMETER(FIntVector, DynamicSDFDimensions)
	END_SHADER_PARAMETER_STRUCT()
};

class FMarchingCubesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMarchingCubesCS);
	SHADER_USE_PARAMETER_STRUCT(FMarchingCubesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(Parameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FCutterUniform, GlobalParams)
		SHADER_PARAMETER_STRUCT_INCLUDE(FMCOutputUniform, MCOutputParams)
		SHADER_PARAMETER(FVector3f, DynamicSDFBoundsMin)
		SHADER_PARAMETER(FVector3f, DynamicSDFBoundsMax)
		SHADER_PARAMETER(FIntVector, DynamicSDFDimensions)
		SHADER_PARAMETER_TEXTURE(RWTexture3D<float>, DynamicSDF)
	END_SHADER_PARAMETER_STRUCT()
};