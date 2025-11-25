#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "RenderResource.h"
#include "TextureResource.h"

using namespace UE::Geometry;

class VOXELCUT_API FToolSDFGenerator
{
public:
	// 预计算工具网格的SDF纹理
	bool PrecomputeSDF(
		const FDynamicMesh3& ToolMesh,       // 固定的工具网格
		const FTransform& ToolTransform,     // 工具的基础变换
		int32 TextureSize = 64,             // VolumeTexture Size
		float BoundsExpansion = 50.0f);      // 边界扩展量

	// 获取GPU可访问的SDF纹理资源
	FTextureRHIRef GetSDFTextureRHI() const { return SDFTextureRHI; }

	// 获取SDF的边界信息（用于UVW计算）
	const FAxisAlignedBox3d& GetSDFBounds() const { return SDFBounds; }

	// 获取VolumeTexture尺寸
	int32 GetVolumeSize() const { return VolumeSize; }

private:
	FTextureRHIRef SDFTextureRHI;       // GPU纹理资源
	FAxisAlignedBox3d SDFBounds;            // SDF覆盖的空间边界
	int32 VolumeSize;   
};