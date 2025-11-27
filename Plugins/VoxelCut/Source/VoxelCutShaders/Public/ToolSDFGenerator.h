#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"

using namespace UE::Geometry;

class VOXELCUTSHADERS_API FToolSDFGenerator
{
public:
	// 预计算工具网格的SDF纹理（异步执行）
	void PrecomputeSDFAsync(
		const FDynamicMesh3& ToolMesh,
		int32 TextureSize = 64,
		TFunction<void(bool)> OnComplete = nullptr
	);

	// 获取GPU可访问的SDF纹理资源（仅在渲染线程使用）
	FTextureRHIRef GetSDFTextureRHI() const { 
		FScopeLock Lock(&TextureCritical);
		return SDFTextureRHI; 
	}

	// 获取SDF的边界信息
	const FAxisAlignedBox3d& GetSDFBounds() const { return SDFBounds; }

	// 获取VolumeTexture尺寸
	int32 GetVolumeSize() const { return VolumeSize; }

private:
	mutable FCriticalSection TextureCritical; // 保护RHI资源访问
	FTextureRHIRef SDFTextureRHI;       // GPU纹理资源
	FAxisAlignedBox3d SDFBounds;        // SDF覆盖的空间边界
	int32 VolumeSize = 0;

	// 内部数据结构用于线程间传递
	struct FComputeData
	{
		FDynamicMesh3 ToolMesh;
		int32 TextureSize;
		FAxisAlignedBox3d Bounds;
		TArray<int16> VolumeData;
		TFunction<void(bool)> CompleteCallback;
	};

	void ComputeSDFData(TUniquePtr<FComputeData> Data);
	void CreateTextureOnRenderThread(TUniquePtr<FComputeData> Data);
};