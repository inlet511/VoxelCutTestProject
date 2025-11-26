#include "MaVoxelData.h"

#include "DynamicMesh/MeshTransforms.h"
#include "Spatial/FastWinding.h"

UE_DISABLE_OPTIMIZATION
using namespace UE::Geometry;

void FOctreeNode::Subdivide(double MinVoxelSize)
{
	if (!bIsLeaf) return;
    
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
    FVector3d ExpandedMin = WorldBounds.Min - FVector3d(2.0 * MarchingCubeSize);
    FVector3d ExpandedMax = WorldBounds.Max + FVector3d(2.0 * MarchingCubeSize);
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
            
            // 为叶子节点分配体素存储（2x2x2 最小分辨率）
            int32 VoxelsPerSide = 2;
            // if (MinNodeSize > MinVoxelSize * 2) 
            // {
            //     VoxelsPerSide = 4; // 中等大小节点
            // }
            // if (MinNodeSize > MinVoxelSize * 4) 
            // {
            //     VoxelsPerSide = 8; // 较大节点
            // }
            Node.VoxelsPerSide = VoxelsPerSide;
            
            // 计算叶子节点内每个体素的值
            FVector3d VoxelSizeLeaf = Node.Bounds.Extents() / (VoxelsPerSide - 1);
            Node.bIsEmpty = true;
            
        	std::atomic<bool> bNodeNonEmpty(false);  // 原子变量，用于线程安全地更新节点状态

        	// 并行处理Z轴
        	ParallelFor(VoxelsPerSide, [&](int32 Z)
			{
				for (int32 Y = 0; Y < VoxelsPerSide; Y++)
				{
					for (int32 X = 0; X < VoxelsPerSide; X++)
					{
						FVector3d LocalPos = FVector3d(X, Y, Z) * VoxelSizeLeaf;
						FVector3d WorldPos = Node.Bounds.Min + LocalPos;
            
						float Distance = CalculateDistanceToMesh(Spatial, Winding, WorldPos);
						int32 Index = Z * VoxelsPerSide * VoxelsPerSide + Y * VoxelsPerSide + X;
						Node.Voxels[Index] = Distance;
            
						// 检查节点是否变为非空
						if (Distance < NodeSize.GetMax())
						{
							bNodeNonEmpty = true;  // 原子操作，线程安全
						}
					}
				}
			});

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
            
            // 在叶子节点内插值
            FVector3d LocalPos = Point - Node.Bounds.Min;
            FVector3d Size = Node.Bounds.Max - Node.Bounds.Min;
            
            int32 VoxelsPerSide = Node.VoxelsPerSide;
            if (VoxelsPerSide <= 1) return 1.0f;
            
            FVector3d VoxelSizeLeaf = Size / (VoxelsPerSide - 1);
            
            FVector3d Coord = LocalPos / VoxelSizeLeaf;
            int32 X = FMath::Clamp(FMath::FloorToInt(Coord.X), 0, VoxelsPerSide - 2);
            int32 Y = FMath::Clamp(FMath::FloorToInt(Coord.Y), 0, VoxelsPerSide - 2);
            int32 Z = FMath::Clamp(FMath::FloorToInt(Coord.Z), 0, VoxelsPerSide - 2);
            
            // 三线性插值
            double u = FMath::Clamp(Coord.X - X, 0.0, 1.0);
            double v = FMath::Clamp(Coord.Y - Y, 0.0, 1.0);
            double w = FMath::Clamp(Coord.Z - Z, 0.0, 1.0);
            
            auto GetVoxel = [&](int32 dx, int32 dy, int32 dz) -> float
            {
                int32 Index = (Z + dz) * VoxelsPerSide * VoxelsPerSide + 
                             (Y + dy) * VoxelsPerSide + (X + dx);
                return (Index >= 0 && Index < 8) ? Node.Voxels[Index] : 1.0f;
            };
            
            float v000 = GetVoxel(0, 0, 0);
            float v100 = GetVoxel(1, 0, 0);
            float v010 = GetVoxel(0, 1, 0);
            float v110 = GetVoxel(1, 1, 0);
            float v001 = GetVoxel(0, 0, 1);
            float v101 = GetVoxel(1, 0, 1);
            float v011 = GetVoxel(0, 1, 1);
            float v111 = GetVoxel(1, 1, 1);
            
            float x00 = FMath::Lerp(v000, v100, u);
            float x10 = FMath::Lerp(v010, v110, u);
            float x01 = FMath::Lerp(v001, v101, u);
            float x11 = FMath::Lerp(v011, v111, u);
            
            float y0 = FMath::Lerp(x00, x10, v);
            float y1 = FMath::Lerp(x01, x11, v);
            
            return FMath::Lerp(y0, y1, w);
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
        
        // 更新叶子节点内的体素
        int32 VoxelsPerSide = LeafNode.VoxelsPerSide;
        if (VoxelsPerSide <= 1) return;
        
        FVector3d VoxelSizeLeaf = (LeafNode.Bounds.Max - LeafNode.Bounds.Min) / (VoxelsPerSide - 1);
        
        for (int32 Z = 0; Z < VoxelsPerSide; Z++)
        {
            for (int32 Y = 0; Y < VoxelsPerSide; Y++)
            {
                for (int32 X = 0; X < VoxelsPerSide; X++)
                {
                    FVector3d LocalPos = FVector3d(X, Y, Z) * VoxelSizeLeaf;
                    FVector3d WorldPos = LeafNode.Bounds.Min + LocalPos;
                    
                    if (UpdateBounds.Contains(WorldPos))
                    {
                        float NewValue = UpdateFunction(WorldPos);
                        int32 Index = Z * VoxelsPerSide * VoxelsPerSide + Y * VoxelsPerSide + X;
                        
                        if (Index >= 0 && Index < 8)
                        {
                            LeafNode.Voxels[Index] = NewValue;
                        }
                    }
                }
            }
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
    int32 TotalVoxels = 0;
    
    TFunction<void(const FOctreeNode&)> CountNodes = [&](const FOctreeNode& Node)
    {
        if (Node.bIsLeaf)
        {
            LeafCount++;
            if (!Node.bIsEmpty) 
            {
                NonEmptyLeafCount++;
                TotalVoxels += 8;
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
    
    UE_LOG(LogTemp, Warning, TEXT("八叉树统计: 总叶子节点=%d, 非空叶子=%d, 存储体素数=%d"), 
           LeafCount, NonEmptyLeafCount, TotalVoxels);
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