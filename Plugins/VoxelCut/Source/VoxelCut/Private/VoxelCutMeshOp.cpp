// Fill out your copyright notice in the Description page of Project Settings.


#include "VoxelCutMeshOp.h"

#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/MeshTransforms.h"
#include "DynamicMesh/MeshNormals.h"
#include "Operations/MeshBoolean.h"
#include "Generators/MarchingCubes.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "HAL/PlatformTime.h"
#include "VoxelCutComputePass.h"

using namespace UE::Geometry;

UE_DISABLE_OPTIMIZATION

void FVoxelCutMeshOp::SetTransform(const FTransformSRT3d& Transform)
{
	ResultTransform = Transform;
}

void FVoxelCutMeshOp::CalculateResult(FProgressCancel* Progress)
{
	if (Progress && Progress->Cancelled())
	{
		return;
	}

	if (!bVoxelDataInitialized)
	{
		UE_LOG(LogTemp, Error, TEXT("Initialize Voxel Data First! (Call InitializeVoxelData())"));
		return;
	}

	//double CutStart = FPlatformTime::Seconds();  // 开始时间    
	// 增量更新：基于现有体素数据进行切削
	if (!IncrementalCut(Progress))
	{
		return;
	}
	//double CutEnd = FPlatformTime::Seconds();    // 结束时间
	// 打印切削耗时
	//double CutTimeMs = (CutEnd - CutStart) * 1000.0;
	//UE_LOG(LogTemp, Log, TEXT("切削操作（IncrementalCut）耗时: %.2f 毫秒"), CutTimeMs);

	// 生成最终网格 - 修正计时
	double GenerateStart = FPlatformTime::Seconds(); // 开始时间
	// 生成最终网格
	ConvertVoxelsToMesh(*PersistentVoxelData, Progress);

	double GenerateEnd = FPlatformTime::Seconds(); // 结束时间

	// 打印模型生成耗时
	double GenerateTimeMs = (GenerateEnd - GenerateStart) * 1000.0;
	UE_LOG(LogTemp, Warning, TEXT("模型生成耗时: %.2f 毫秒"), GenerateTimeMs);
}

bool FVoxelCutMeshOp::InitializeVoxelData(FProgressCancel* Progress)
{
	if (!TargetMesh)
	{
		return false;
	}

	// 创建新的体素数据容器
	if (!PersistentVoxelData.IsValid())
	{
		PersistentVoxelData = MakeShared<FMaVoxelData>();
		PersistentVoxelData->MarchingCubeSize = MarchingCubeSize;
		PersistentVoxelData->MaxOctreeDepth = MaxOctreeDepth;
		PersistentVoxelData->MinVoxelSize = MinVoxelSize;
	}

	// 体素化目标网格
	bool success = VoxelizeMesh(*TargetMesh, TargetTransform, *PersistentVoxelData, Progress);

	bVoxelDataInitialized = true;
	return success;
}

bool FVoxelCutMeshOp::IncrementalCut(FProgressCancel* Progress)
{
	if (!PersistentVoxelData.IsValid() || !CutToolMesh)
	{
		return false;
	}

	// 局部更新：只更新受刀具影响的区域
	UpdateLocalRegion(*PersistentVoxelData, *CutToolMesh,
	                  CutToolTransform, Progress);


	return !(Progress && Progress->Cancelled());
}

double GetDistanceToMesh(const FDynamicMeshAABBTree3& Spatial, TFastWindingTree<FDynamicMesh3> Winding,
                         const FVector3d& LocalPoint, const FVector3d& WorldPoint)
{
	double NearestDistSqr;
	int NearestTriID = Spatial.FindNearestTriangle(LocalPoint, NearestDistSqr);

	if (NearestTriID == IndexConstants::InvalidID)
	{
		return TNumericLimits<double>::Max();
	}

	// 计算有符号距离（内部为负，外部为正）    
	bool bInSide = Winding.IsInside(WorldPoint);

	return FMathd::Sqrt(NearestDistSqr) * (bInSide ? -1 : 1);
}


bool FVoxelCutMeshOp::VoxelizeMesh(const FDynamicMesh3& Mesh, const FTransform& Transform,
                                   FMaVoxelData& VoxelData, FProgressCancel* Progress)
{
	if (Mesh.TriangleCount() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("VoxelizeMesh: Input mesh has no triangles"));
		return false;
	}
	double StartTime = FPlatformTime::Seconds();

	// 从模型构建八叉树
	VoxelData.BuildOctreeFromMesh(Mesh, Transform);

	double EndTime = FPlatformTime::Seconds();
	UE_LOG(LogTemp, Warning, TEXT("网格体素化耗时: %.2f 毫秒"), (EndTime - StartTime) * 1000.0);

	return true;
}


void FVoxelCutMeshOp::UpdateLocalRegion(FMaVoxelData& TargetVoxels, const FDynamicMesh3& ToolMesh,
                                        const FTransform& ToolTransform, FProgressCancel* Progress)
{
	if (!TargetVoxels.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("UpdateLocalRegion: TargetVoxels is not valid"));
		return;
	}

	double StartTime = FPlatformTime::Seconds();

	// 切削工具的扩展边界
	FAxisAlignedBox3d OriginalBounds = ToolMesh.GetBounds();
	FAxisAlignedBox3d TransformedBounds(OriginalBounds, ToolTransform);
	FVector3d ExpandedMin = TransformedBounds.Min - FVector3d(UpdateMargin * TargetVoxels.MarchingCubeSize);
	FVector3d ExpandedMax = TransformedBounds.Max + FVector3d(UpdateMargin * TargetVoxels.MarchingCubeSize);
	FAxisAlignedBox3d ToolExtendedBounds(ExpandedMin, ExpandedMax);

	double EndTime1 = FPlatformTime::Seconds();

	// 1. 收集受到影响的叶子节点
	AffectedNodes.Empty();
	TargetVoxels.OctreeRoot.CollectAffectedNodes(TransformedBounds, AffectedNodes);
	uint32 NodeCount = AffectedNodes.Num();
	if (NodeCount == 0)
		return;

	TArray<FlatOctreeNode> FlatOctreeNodes;
	FlatOctreeNodes.SetNum(NodeCount);
	// 现在可以安全访问下标
	for (uint32 i = 0; i < NodeCount; i++)
	{
		// 赋值边界
		/*FlatOctreeNodes[i].BoundsMax = AffectedNodes[i]->Bounds.Max;
		FlatOctreeNodes[i].BoundsMin = AffectedNodes[i]->Bounds.Min;*/
		FlatOctreeNodes[i].BoundsMin[0] = AffectedNodes[i]->Bounds.Min.X;
		FlatOctreeNodes[i].BoundsMin[1] = AffectedNodes[i]->Bounds.Min.Y;
		FlatOctreeNodes[i].BoundsMin[2] = AffectedNodes[i]->Bounds.Min.Z;

		FlatOctreeNodes[i].BoundsMax[0] = AffectedNodes[i]->Bounds.Max.X;
		FlatOctreeNodes[i].BoundsMax[1] = AffectedNodes[i]->Bounds.Max.Y;
		FlatOctreeNodes[i].BoundsMax[2] = AffectedNodes[i]->Bounds.Max.Z;


		// 安全拷贝Voxels数组（确保源数据有效）
		if (AffectedNodes[i] != nullptr)
		{
			// 拷贝8个float（匹配Voxels[8]的大小）
			FMemory::Memcpy(
				FlatOctreeNodes[i].Voxels, // 目标：当前元素的Voxels数组
				AffectedNodes[i]->Voxels, // 源：AffectedNodes的Voxels数组
				sizeof(float) * 8 // 固定大小：8个float
			);
		}
		else
		{
			// 兜底：源节点为空时清空Voxels
			FMemory::Memzero(FlatOctreeNodes[i].Voxels, sizeof(float) * 8);
			UE_LOG(LogTemp, Warning, TEXT("AffectedNodes[%d]为空，跳过Voxels拷贝"), i);
		}
	}
	// 2. 设置发送给GPU的参数
	FVoxelCutCSParams Params;
	Params.ToolSDFGenerator = ToolSDFGenerator;
	Params.ToolTransform = ToolTransform;
	Params.OctreeNodesArray = FlatOctreeNodes;

	// 3. 调用ComputeShader并设置回调
	FVoxlCutShaderInterface::Dispatch(
		Params,
	    [this, AffectedNodesCopy = AffectedNodes](TArray<FlatOctreeNode> ResultNodes)
	    {
		    // 4. 处理GPU返回的结果
		    if (ResultNodes.Num() != AffectedNodesCopy.Num())
		    {
			    UE_LOG(LogTemp, Error, TEXT("Compute shader result count mismatch"));
			    return;
		    }
		                                 
		    // 5. 更新体素数据
		    for (int32 i = 0; i < AffectedNodesCopy.Num(); i++)
		    {
			    FOctreeNode* Node = AffectedNodesCopy[i];
			    const FlatOctreeNode& ResultNode = ResultNodes[i];
			    FMemory::Memcpy(Node->Voxels, ResultNode.Voxels, sizeof(float) * 8);
		    }

		    // 6. 触发模型更新回调
		    if (OnVoxelDataUpdated.IsBound())
		    {
			    OnVoxelDataUpdated.Execute();
		    }
	    });


	double EndTime = FPlatformTime::Seconds();
	//UE_LOG(LogTemp, Warning, TEXT("局部区域更新整体耗时: %.2f 毫秒, 工具信息准备耗时：%.5f, 占比：%.5f, 更新了 %d 个体素"), (EndTime - StartTime) * 1000.0,(EndTime1-StartTime), (EndTime1 - StartTime)/(EndTime-StartTime), UpdatedVoxels);

	// 切削后对局部区域进行高斯平滑（减少体素值突变）
	//SmoothLocalVoxels(TargetVoxels, VoxelMin, VoxelMax, 1);
}


void FVoxelCutMeshOp::ConvertVoxelsToMesh(const FMaVoxelData& Voxels, FProgressCancel* Progress)
{
	if (Progress && Progress->Cancelled()) return;

	double StartTime = FPlatformTime::Seconds();

	FMarchingCubes MarchingCubes;
	// 使用八叉树边界
	MarchingCubes.Bounds = Voxels.GetOctreeBounds();
	MarchingCubes.CubeSize = Voxels.MarchingCubeSize;

	// 使用八叉树进行采样
	MarchingCubes.Implicit = [&Voxels](const FVector3d& Pos) -> double
	{
		return Voxels.GetValueAtPosition(Pos);
	};

	MarchingCubes.IsoValue = 0.0f;

	ResultMesh->Copy(&MarchingCubes.Generate());

	// 平滑模型
	//SmoothGeneratedMesh(*ResultMesh, SmoothingIteration);

	UE_LOG(LogTemp, Warning, TEXT("Generated mesh triangle count: %d"), ResultMesh->TriangleCount());

	if (ResultMesh->TriangleCount() > 0)
	{
		ResultMesh->ReverseOrientation(true);
		FMeshNormals::QuickComputeVertexNormals(*ResultMesh);

		// 复原位置
		FTransform InverseTargetTransform = TargetTransform.Inverse();
		MeshTransforms::ApplyTransform(*ResultMesh, InverseTargetTransform, true);
	}
}

void FVoxelCutMeshOp::SmoothGeneratedMesh(FDynamicMesh3& Mesh, int32 Iterations)
{
	if (Mesh.VertexCount() == 0) return;

	for (int32 Iter = 0; Iter < Iterations; Iter++)
	{
		TArray<FVector3d> NewPositions;
		NewPositions.SetNum(Mesh.MaxVertexID());

		// 对每个顶点应用拉普拉斯平滑
		for (int32 VertexID : Mesh.VertexIndicesItr())
		{
			FVector3d CurrentPos = Mesh.GetVertex(VertexID);
			FVector3d NeighborAverage = FVector3d::Zero();
			int32 NeighborCount = 0;

			// 计算相邻顶点的平均位置
			for (int32 NeighborID : Mesh.VtxVerticesItr(VertexID))
			{
				NeighborAverage += Mesh.GetVertex(NeighborID);
				NeighborCount++;
			}

			if (NeighborCount > 0)
			{
				NeighborAverage /= NeighborCount;
				// 向相邻顶点平均位置移动
				NewPositions[VertexID] = FMath::Lerp(CurrentPos, NeighborAverage, this->SmoothingStrength);
			}
			else
			{
				NewPositions[VertexID] = CurrentPos;
			}
		}

		// 应用新位置
		for (int32 VertexID : Mesh.VertexIndicesItr())
		{
			Mesh.SetVertex(VertexID, NewPositions[VertexID]);
		}
	}

	// 重新计算法线
	FMeshNormals::QuickComputeVertexNormals(Mesh);
}

UE_ENABLE_OPTIMIZATION
