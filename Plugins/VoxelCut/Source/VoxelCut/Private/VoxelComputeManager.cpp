// VoxelComputeManager.cpp
#include "VoxelComputeManager.h"
#include "MaVoxelData.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"


void FVoxelComputeManager::UpdateVoxelsWithComputeShader(
    const FDynamicMesh3& ToolMesh,
    const FTransform& ToolTransform,
    FMaVoxelData& VoxelData,
    const FAxisAlignedBox3d& UpdateBounds)
{
    // 1. 准备刀具网格数据
    TArray<FVector3f> ToolVertices;
    TArray<uint32> ToolIndices;
    
    // 转换顶点数据
    for (int32 VertexID : ToolMesh.VertexIndicesItr())
    {
        FVector3f Vertex = (FVector3f)ToolMesh.GetVertex(VertexID);
        ToolVertices.Add(Vertex);
    }
    
    // 转换索引数据
    for (int32 TriangleID : ToolMesh.TriangleIndicesItr())
    {
        FIndex3i Tri = ToolMesh.GetTriangle(TriangleID);
        ToolIndices.Add(Tri.A);
        ToolIndices.Add(Tri.B);
        ToolIndices.Add(Tri.C);
    }

    // 2. 收集需要更新的体素数据
    TArray<FVector3d> VoxelPositions;
    TArray<float> CurrentValues;
    
    // 使用八叉树收集受影响区域的体素
    TArray<FOctreeNode*> AffectedNodes;
    VoxelData.OctreeRoot.CollectAffectedNodes(UpdateBounds, AffectedNodes);


    // 3. 创建GPU缓冲区
    FRHICommandListImmediate& RHICmdList = GRHICommandList.GetImmediateCommandList();
    

    }

    // 4. 设置ComputeShader参数
    FVoxelUpdateCS::FParameters Parameters;
    


    // 5. 调度ComputeShader
    FIntVector GroupCount = FIntVector(
        FMath::DivideAndRoundUp(CurrentValues.Num(), 64),
        1, 1);
    
    TShaderMapRef<FVoxelUpdateCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    RHICmdList.SetComputeShader(ComputeShader.GetComputeShader());
    
    SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
    RHICmdList.DispatchComputeShader(GroupCount.X, GroupCount.Y, GroupCount.Z);

    // 6. 读回结果
    if (CurrentValues.Num() > 0)
    {
        void* ResultData = RHILockStructuredBuffer(VoxelValuesBuffer, 0, CurrentValues.Num() * sizeof(float), RLM_ReadOnly);
        FMemory::Memcpy(CurrentValues.GetData(), ResultData, CurrentValues.Num() * sizeof(float));
        RHIUnlockStructuredBuffer(VoxelValuesBuffer);
    }

    // 7. 更新八叉树数据
    int32 ValueIndex = 0;
    for (FOctreeNode* Node : AffectedNodes)
    {
        if (Node->bIsLeaf && !Node->bIsEmpty)
        {
            int32 VoxelsPerSide = Node->VoxelsPerSide;
            
            for (int32 Z = 0; Z < VoxelsPerSide; Z++)
            {
                for (int32 Y = 0; Y < VoxelsPerSide; Y++)
                {
                    for (int32 X = 0; X < VoxelsPerSide; X++)
                    {
                        FVector3d LocalPos = FVector3d(X, Y, Z) * VoxelSize;
                        FVector3d WorldPos = Node->Bounds.Min + LocalPos;
                        
                        if (UpdateBounds.Contains(WorldPos))
                        {
                            int32 Index = Z * VoxelsPerSide * VoxelsPerSide + Y * VoxelsPerSide + X;
                            Node->Voxels[Index] = CurrentValues[ValueIndex++];
                        }
                    }
                }
            }
        }
    }

    // 8. 释放资源
    if (VertexBufferSRV) VertexBufferSRV->Release();
    if (IndexBufferSRV) IndexBufferSRV->Release();
    if (VoxelValuesUAV) VoxelValuesUAV->Release();
}