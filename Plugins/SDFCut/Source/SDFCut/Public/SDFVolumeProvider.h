// SDFVolumeProvider.h
#pragma once

#include "CoreMinimal.h"

/** 
 * 纯C++接口，用于高性能SDF查询 
 * 避免使用 UInterface 带来的 Cast 开销
 */
class ISDFVolumeProvider
{
public:
	virtual ~ISDFVolumeProvider() {}

	// 将世界坐标转换为体素空间的坐标 (用于后续查询)
	virtual bool WorldToVoxelSpace(const FVector& WorldLocation, FVector& OutVoxelCoord) const = 0;

	// 采样 SDF 值 (Trilinear)
	virtual float SampleSDF(const FVector& VoxelCoord) const = 0;

	// 采样 材质 ID
	virtual int32 SampleMaterialID(const FVector& VoxelCoord) const = 0;

	// 获取体素尺寸 (用于深度计算)
	virtual float GetVoxelSize() const = 0;

	// 获取读写锁 (用于线程安全)
	virtual FRWLock& GetDataLock() = 0;
};