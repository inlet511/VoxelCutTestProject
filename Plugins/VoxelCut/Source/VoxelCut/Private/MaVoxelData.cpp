#include "MaVoxelData.h"

#include "DynamicMesh/MeshTransforms.h"
#include "Spatial/FastWinding.h"

UE_DISABLE_OPTIMIZATION
using namespace UE::Geometry;

void FOctreeNode::Subdivide(double MinVoxelSize)
{
	if (!bIsLeaf) return;

    Voxel = 10000.0f; // 非叶子节点体素无关紧要，但初始化为一个大值
    
	FVector3d Center = Bounds.Center();
    FVector3d Size = Bounds.Max - Bounds.Min;
    
	// 如果节点尺寸已经小于最小体素尺寸，不进行细分
	if (Size.X < MinVoxelSize  && Size.Y < MinVoxelSize  && Size.Z < MinVoxelSize )
	{
		return;
	}
    
	Children.SetNum(8);
    
	for (int32 i = 0; i < 8; i++)
	{
		FVector3d ChildMin;
		FVector3d ChildMax;
        
		if (i & 1)
		{
		    ChildMin.X = Center.X;
		    ChildMax.X = Bounds.Max.X;
		}	        
	    else
	    {
	        ChildMin.X = Bounds.Min.X;
	        ChildMax.X = Center.X;	        
	    }
		if (i & 2)
		{
		    ChildMin.Y = Center.Y;
		    ChildMax.Y = Bounds.Max.Y;
		}
	    else
	    {
	        ChildMin.Y = Bounds.Min.Y;
	        ChildMax.Y = Center.Y;
	    }
		if (i & 4)
		{
		    ChildMin.Z = Center.Z;
		    ChildMax.Z = Bounds.Max.Z;
		}
	    else
	    {
	        ChildMin.Z = Bounds.Min.Z;
	        ChildMax.Z = Center.Z;
	    }

	    // 添加边界验证
	    if (ChildMin.X >= ChildMax.X || ChildMin.Y >= ChildMax.Y || ChildMin.Z >= ChildMax.Z)
	    {
	        UE_LOG(LogTemp, Error, TEXT("无效的子节点边界[%d]: Min(%s), Max(%s)"), 
                   i, *ChildMin.ToString(), *ChildMax.ToString());
	        // 使用安全的边界
	        ChildMin = Bounds.Min;
	        ChildMax = Bounds.Max;
	    }
        
		Children[i].Bounds = FAxisAlignedBox3d(ChildMin, ChildMax);
		Children[i].Depth = Depth + 1;
		Children[i].bIsLeaf = true;
		Children[i].bIsEmpty = true;
	}
    
	bIsLeaf = false;
}

bool FOctreeNode::ContainsPoint(const FVector3d& Point) const
{
	return Bounds.Contains(Point);
}

bool FOctreeNode::IntersectsBounds(const FAxisAlignedBox3d& OtherBounds) const
{
	return Bounds.Intersects(OtherBounds);
}

void FOctreeNode::CollectAffectedNodes(const FAxisAlignedBox3d& InBounds, TArray<FOctreeNode*>& OutNodes)
{
	if (!IntersectsBounds(InBounds)) return;
	if (bIsLeaf==true) {
		OutNodes.Add(this);
	} else {
		for (FOctreeNode& Child : Children) {
			Child.CollectAffectedNodes(InBounds, OutNodes);
		}
	}
}

void FMaVoxelData::Reset()
{
	OctreeRoot = FOctreeNode();
}

void FMaVoxelData::BuildOctreeFromMesh(const FDynamicMesh3& Mesh, const FTransform& Transform)
{
	if (Mesh.TriangleCount() == 0) 
    {
        UE_LOG(LogTemp, Warning, TEXT("BuildOctreeFromMesh: Mesh has no triangles"));
        return;
    }
    
    double StartTime = FPlatformTime::Seconds();
    
    // 计算网格边界
    FAxisAlignedBox3d LocalBounds = Mesh.GetBounds();
    FAxisAlignedBox3d WorldBounds(LocalBounds, Transform);
    
    // 设置八叉树根节点边界（稍微扩展）
    FVector3d ExpandedMin = WorldBounds.Min - FVector3d(4.0 * MarchingCubeSize);
    FVector3d ExpandedMax = WorldBounds.Max + FVector3d(4.0 * MarchingCubeSize);
    OctreeRoot.Bounds = FAxisAlignedBox3d(ExpandedMin, ExpandedMax);
    OctreeRoot.Depth = 0;
    OctreeRoot.bIsLeaf = true;
    OctreeRoot.bIsEmpty = true;
    
    // 创建空间查询结构
    FDynamicMesh3 WorldSpaceMesh = Mesh;
    MeshTransforms::ApplyTransform(WorldSpaceMesh, Transform, true);
    FDynamicMeshAABBTree3 Spatial(&WorldSpaceMesh);    
    TFastWindingTree<FDynamicMesh3> Winding(&Spatial);
    
    // 递归构建八叉树
    TFunction<void(FOctreeNode&)> BuildNode = [&](FOctreeNode& Node)
    {
        FVector3d NodeSize = Node.Bounds.Max - Node.Bounds.Min;
        double MinNodeSize = NodeSize.GetMin();
        
        // 如果节点足够小或者达到最大深度，设为叶子节点
        if (MinNodeSize <= MinVoxelSize || Node.Depth >= MaxOctreeDepth)
        {
            Node.bIsLeaf = true;
            
            // 计算叶子节点内每个体素的值
            FVector3d VoxelSize = Node.Bounds.Max - Node.Bounds.Min;
            Node.bIsEmpty = true;
            
        	std::atomic<bool> bNodeNonEmpty(false);  // 原子变量，用于线程安全地更新节点状态


			FVector3d WorldPos = Node.Bounds.Center();            
			float Distance = CalculateDistanceToMesh(Spatial, Winding, WorldPos);
			Node.Voxel = Distance;
            
			// 检查节点是否变为非空
			if (Distance < NodeSize.GetMax())
			{
				bNodeNonEmpty = true;  // 原子操作，线程安全
			}


        	Node.bIsEmpty = !bNodeNonEmpty;
        }
        else
        {
            // 需要继续细分
            Node.Subdivide(MinVoxelSize);
            for (FOctreeNode& Child : Node.Children)
            {
                BuildNode(Child);
            }
            
            // 检查子节点是否都为空
            Node.bIsEmpty = true;
            for (const FOctreeNode& Child : Node.Children)
            {
                if (!Child.bIsEmpty)
                {
                    Node.bIsEmpty = false;
                    break;
                }
            }
        }
    };
    
    BuildNode(OctreeRoot);
    
    double EndTime = FPlatformTime::Seconds();
    UE_LOG(LogTemp, Warning, TEXT("八叉树构建耗时: %.2f 毫秒"), (EndTime - StartTime) * 1000.0);

    DebugLogOctreeStats();
}

float FMaVoxelData::GetValueAtPosition(const FVector3d& WorldPos) const
{
    // 八叉树查询
    TFunction<float(const FOctreeNode&, const FVector3d&)> QueryNode = 
    [&](const FOctreeNode& Node, const FVector3d& Point) -> float
    {
        if (!Node.ContainsPoint(Point)) return 1.0f;
        
        if (Node.bIsLeaf)
        {
            if (Node.bIsEmpty) return 1.0f;
            
            return Node.Voxel;
        }
        else
        {
            // 查询子节点
            for (const FOctreeNode& Child : Node.Children)
            {
                if (Child.ContainsPoint(Point))
                {
                    return QueryNode(Child, Point);
                }
            }
            return 1.0f;
        }
    };
    
    return QueryNode(OctreeRoot, WorldPos);
}

void FMaVoxelData::UpdateLeafNode(
	FOctreeNode& LeafNode,
	const FAxisAlignedBox3d& UpdateBounds,
	const TFunctionRef<float(const FVector3d&)>& UpdateFunction)
{  
    if (LeafNode.bIsLeaf)
    {
        if (LeafNode.bIsEmpty) return;

        // 更新叶子节点内的单个体素
        // 计算节点中心点作为体素位置
        FVector3d NodeCenter = (LeafNode.Bounds.Min + LeafNode.Bounds.Max) * 0.5;

        // 检查中心点是否在更新范围内
        if (UpdateBounds.Contains(NodeCenter))
        {
            float NewValue = UpdateFunction(NodeCenter);
            LeafNode.Voxel = NewValue;
        }
    }
      
}

void FMaVoxelData::UpdateRegion(const FAxisAlignedBox3d& UpdateBounds,
                                const TFunctionRef<float(const FVector3d&)>& UpdateFunction)
{
	// 1. 收集受到影响的叶子节点
	TArray<FOctreeNode*> AffectedNodes;
	OctreeRoot.CollectAffectedNodes(UpdateBounds, AffectedNodes);

	// 2. 并行处理所有叶子节点
	ParallelFor(AffectedNodes.Num(), [&](int32 i){
		FOctreeNode* Node = AffectedNodes[i];
		UpdateLeafNode(*Node,UpdateBounds,UpdateFunction);
	});
	
}

void FMaVoxelData::DebugLogOctreeStats() const
{
    int32 LeafCount = 0;
    int32 NonEmptyLeafCount = 0;

    
    TFunction<void(const FOctreeNode&)> CountNodes = [&](const FOctreeNode& Node)
    {
        if (Node.bIsLeaf)
        {
            LeafCount++;
            if (!Node.bIsEmpty) 
            {
                NonEmptyLeafCount++;
            }
        }
        else
        {
            for (const FOctreeNode& Child : Node.Children)
            {
                CountNodes(Child);
            }
        }
    };
    
    CountNodes(OctreeRoot);
    
    UE_LOG(LogTemp, Warning, TEXT("八叉树统计: 总叶子节点=%d, 非空叶子=%d"), 
           LeafCount, NonEmptyLeafCount);
}

float FMaVoxelData::CalculateDistanceToMesh(const FDynamicMeshAABBTree3& Spatial,
	TFastWindingTree<FDynamicMesh3>& Winding, const FVector3d& Pos) const
{    
    // 使用AABB树查找最近三角形
    double NearestDistSqr; 
    int32 NearestTriID = Spatial.FindNearestTriangle(Pos, NearestDistSqr);
    
    if (NearestTriID == IndexConstants::InvalidID)
    {
        return TNumericLimits<float>::Max(); // 没有找到三角形，返回最大距离
    }
    double NearestDist = FMath::Sqrt(NearestDistSqr);
    
    // 使用绕数法判断点在网格内部还是外部
    bool bIsInside = Winding.IsInside(Pos);
    
    // 内部点距离为负，外部点距离为正
    float SignedDistance = bIsInside ? -NearestDist : NearestDist;
    
    return (float)SignedDistance;
}
UE_ENABLE_OPTIMIZATION