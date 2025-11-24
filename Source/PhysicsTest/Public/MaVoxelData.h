// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"

using namespace UE::Geometry;

// 八叉树节点
struct PHYSICSTEST_API FOctreeNode
{
	FAxisAlignedBox3d Bounds;
	TArray<FOctreeNode> Children;
	TArray<float> Voxels; // 叶子节点存储体素数据
	int32 VoxelsPerSide = 0; // 每边体素数量
	int32 Depth = 0;
	bool bIsLeaf = true;
	bool bIsEmpty = true; // 标记节点是否为空（优化用）
	

	void Subdivide(double MinVoxelSize);
	bool ContainsPoint(const FVector3d& Point) const;
	bool IntersectsBounds(const FAxisAlignedBox3d& OtherBounds) const;
	void CollectAffectedNodes(const FAxisAlignedBox3d& InBounds, TArray<FOctreeNode*>& OutNodes);
};

// 体素数据容器
struct PHYSICSTEST_API FMaVoxelData
{
	// 控制Voxel精度的参数
	double MarchingCubeSize = 2.0f; // Marching Cubes的体素大小
	int32 MaxOctreeDepth = 6; // 最大深度，控制精度
	double MinVoxelSize = 0.5; // 最小体素大小

	FOctreeNode OctreeRoot;

	void Reset();
	bool IsValid() const { return  !OctreeRoot.Bounds.IsEmpty(); }
	
	void BuildOctreeFromMesh(const FDynamicMesh3& Mesh, const FTransform& Transform);
	float GetValueAtPosition(const FVector3d& WorldPos) const;
	void UpdateLeafNode(
		FOctreeNode& LeafNode,
		const FAxisAlignedBox3d& UpdateBounds,
		const TFunctionRef<float(const FVector3d&)>& UpdateFunction); 
	void UpdateRegion(const FAxisAlignedBox3d& UpdateBounds, const TFunctionRef<float(const FVector3d&)>& UpdateFunction);

	// 调试
	void DebugLogOctreeStats() const;

	// 获取用于Marching Cubes的边界
	FAxisAlignedBox3d GetOctreeBounds() const { return OctreeRoot.Bounds; }

private:
	// 内部辅助方法
	float CalculateDistanceToMesh(const FDynamicMeshAABBTree3& Spatial, 
								TFastWindingTree<FDynamicMesh3>& Winding,
								const FVector3d& Pos) const;
};
