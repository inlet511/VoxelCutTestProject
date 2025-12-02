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


void FVoxelCutMeshOp::UpdateLocalRegion()
{
	if (!PersistentVoxelData.IsValid()|| !CutToolMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("UpdateLocalRegion: [PersistentVoxelData OR CutToolMesh] is not valid"));
		return;
	}

	// 切削工具的扩展边界
	FAxisAlignedBox3d OriginalBounds = CutToolMesh->GetBounds();
	FAxisAlignedBox3d TransformedBounds(OriginalBounds, CutToolTransform);

	double StartTime = FPlatformTime::Seconds();

	// 1. 收集受到影响的叶子节点
	AffectedNodes.Empty();
	PersistentVoxelData->OctreeRoot.CollectAffectedNodes(TransformedBounds, AffectedNodes);
	uint32 NodeCount = AffectedNodes.Num();
	// 如果没有受到影响的叶子节点，直接返回，并设置状态
	if (NodeCount == 0)
	{
		if (OnVoxelDataUpdated.IsBound())
		{
			OnVoxelDataUpdated.Execute(false);
		}
		return;
	}
		

	double EndTime = FPlatformTime::Seconds();

	UE_LOG(LogTemp, Warning, TEXT("收集受影响叶子节点耗时: %.2f 毫秒"), (EndTime-StartTime) * 1000.0f);
	
	TArray<FlatOctreeNode> FlatOctreeNodes;
	FlatOctreeNodes.SetNum(NodeCount);
	for (uint32 i = 0; i < NodeCount; i++)
	{
		// 赋值边界
		FlatOctreeNodes[i].BoundsMin[0] = AffectedNodes[i]->Bounds.Min.X;
		FlatOctreeNodes[i].BoundsMin[1] = AffectedNodes[i]->Bounds.Min.Y;
		FlatOctreeNodes[i].BoundsMin[2] = AffectedNodes[i]->Bounds.Min.Z;

		FlatOctreeNodes[i].BoundsMax[0] = AffectedNodes[i]->Bounds.Max.X;
		FlatOctreeNodes[i].BoundsMax[1] = AffectedNodes[i]->Bounds.Max.Y;
		FlatOctreeNodes[i].BoundsMax[2] = AffectedNodes[i]->Bounds.Max.Z;

		if (AffectedNodes[i] != nullptr)
		{
			FlatOctreeNodes[i].Voxel = AffectedNodes[i]->Voxel;			
		}
		else
		{
			FlatOctreeNodes[i].Voxel = 1.0f;
		}
	}
	// 2. 设置发送给GPU的参数
	FVoxelCutCSParams Params;
	Params.ToolSDFGenerator = ToolSDFGenerator;
	Params.ToolTransform = CutToolTransform;
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
				Node->Voxel = ResultNode.Voxel;
		    	if (ResultNode.Voxel > 0.0f)
		    	{
		    		Node->bIsEmpty = true;
		    	}
		    }

		    // 6. 触发模型更新回调
		    if (OnVoxelDataUpdated.IsBound())
		    {
			    OnVoxelDataUpdated.Execute(true);
		    }
	    });
}




void RecursivelyLogOctreeNode(const FOctreeNode& Node, int32 Level)
{
	if (Node.bIsEmpty) return;
	else
	{
		if (Node.bIsLeaf)
		{
			UE_LOG(LogTemp, Warning, TEXT("Leaf, L:%d, Bounds Min: %s, Bounds Max: %s, Value: %.2f"), Level, *Node.Bounds.Min.ToString(), *Node.Bounds.Max.ToString(), Node.Voxel);
		}
		else {
			UE_LOG(LogTemp, Warning, TEXT("NoneLeaf, L:%d, Bounds Min: %s, Bounds Max %s"), Level, *Node.Bounds.Min.ToString(), *Node.Bounds.Max.ToString());

			int32 NewLevel = Level + 1;
			for (auto ChildNode : Node.Children)
			{
				RecursivelyLogOctreeNode(ChildNode, NewLevel);
			}
		}

	}

}

void LogVoxelData(const FMaVoxelData& Voxels)
{
	FOctreeNode RootNode = Voxels.OctreeRoot;
	RecursivelyLogOctreeNode(RootNode, 0);
}


void FVoxelCutMeshOp::ConvertVoxelsToMesh(const FMaVoxelData& Voxels, FProgressCancel* Progress)
{
	if (Progress && Progress->Cancelled()) return;

	// 检查体素数据有效性
	if (!Voxels.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid voxel data for mesh generation"));
		return;
	}

	// 检查八叉树边界
	FAxisAlignedBox3d Bounds = Voxels.GetOctreeBounds();
	if (Bounds.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Empty octree bounds"));
		return;
	}

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
	MarchingCubes.Generate();

	ResultMesh->Copy(&MarchingCubes);
	
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

void FVoxelCutMeshOp::CalculateResult(FProgressCancel* Progress)
{
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

void FVoxelCutMeshOp::PrintOctreeNodeRecursive(const FOctreeNode& Node, int32 Depth)
{
	// 创建缩进字符串以便于阅读层级关系
	FString Indent;
	for (int32 i = 0; i < Depth; i++)
	{
		Indent += "  ";
	}

	FString NodeType = Node.bIsLeaf ? TEXT("叶子") : TEXT("分支");
	FString EmptyStatus = Node.bIsEmpty ? TEXT("空") : TEXT("非空");

	UE_LOG(LogTemp, Warning, TEXT("%s---------------"), *NodeType);

	if (Node.bIsLeaf)
	{

		if (!Node.bIsEmpty)
		{
			UE_LOG(LogTemp, Warning, TEXT("%s  体素值样本: %.2f"), *Indent, Node.Voxel);
		}
	}
	// 递归处理子节点
	for (const FOctreeNode& Child : Node.Children)
	{
		PrintOctreeNodeRecursive(Child, Depth + 1);
	}
}

UE_ENABLE_OPTIMIZATION
