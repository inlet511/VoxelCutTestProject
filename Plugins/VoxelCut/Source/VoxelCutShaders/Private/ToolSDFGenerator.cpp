#include "ToolSDFGenerator.h"
#include "RenderUtils.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Spatial/FastWinding.h"
#include "RHI.h"
#include "RHIResources.h"
#include "RenderingThread.h"

void FToolSDFGenerator::PrecomputeSDFAsync(
    const FDynamicMesh3& ToolMesh,
    int32 TextureSize,
    TFunction<void(bool)> OnComplete
)
{
    // 复制输入数据到新的计算数据结构
    TUniquePtr<FComputeData> ComputeData = MakeUnique<FComputeData>();
    ComputeData->ToolMesh = ToolMesh;
    ComputeData->TextureSize = TextureSize;
    ComputeData->CompleteCallback = OnComplete;

    // 先在工作线程计算SDF数据
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, Data = MoveTemp(ComputeData)]() mutable
    {
        ComputeSDFData(MoveTemp(Data));
    });
}

void FToolSDFGenerator::ComputeSDFData(TUniquePtr<FComputeData> Data)
{
    // 1. 计算工具网格边界
    Data->Bounds = Data->ToolMesh.GetBounds();
    FVector3d SDFExtent = Data->Bounds.Max - Data->Bounds.Min;

    // 2. 创建空间查询结构
    FDynamicMeshAABBTree3 ToolSpatial(&Data->ToolMesh);
    TFastWindingTree<FDynamicMesh3> ToolWinding(&ToolSpatial);

    // 3. 初始化Volume数据
    Data->VolumeData.SetNumZeroed(Data->TextureSize * Data->TextureSize * Data->TextureSize);
    int32 TotalVoxels = Data->VolumeData.Num();

    // 计算最大可能距离
    double MaxPossibleDist = SDFExtent.GetMax() * 2.0;

    // 4. 并行计算每个体素的符号距离
    ParallelFor(Data->TextureSize, [&](int32 Z)
    {
        for (int32 Y = 0; Y < Data->TextureSize; Y++)
        {
            for (int32 X = 0; X < Data->TextureSize; X++)
            {
                // 计算采样位置
                FVector3d VoxelUVW(
                    (double)X / (Data->TextureSize - 1),
                    (double)Y / (Data->TextureSize - 1),
                    (double)Z / (Data->TextureSize - 1)
                );
                
                FVector3d SamplePos = Data->Bounds.Min + VoxelUVW * SDFExtent;

                // 计算符号距离
                double NearestDistSqr;
                int32 NearestTriID = ToolSpatial.FindNearestTriangle(SamplePos, NearestDistSqr);
                
                float SignedDist = (float)MaxPossibleDist;
                if (NearestTriID != IndexConstants::InvalidID)
                {
                    double NearestDist = FMath::Sqrt(NearestDistSqr);
                    bool bInside = ToolWinding.IsInside(SamplePos);
                    SignedDist = (float)(bInside ? -NearestDist : NearestDist);
                }

                // 编码距离值
                //int16 Encoded = (int16)FMath::Clamp(SignedDist, -32767.0f, 32767.0f);
                int32 Index = Z * (Data->TextureSize * Data->TextureSize) + Y * Data->TextureSize + X;
                if (Index < TotalVoxels) // 边界检查
                {
                    Data->VolumeData[Index] = SignedDist;
                }
            }
        }
    });

    // 5. 提交到渲染线程创建纹理
    ENQUEUE_RENDER_COMMAND(CreateSDFTexture)(
        [this, Data = MoveTemp(Data)](FRHICommandListImmediate& RHICmdList) mutable
        {
            CreateTextureOnRenderThread(MoveTemp(Data));
        }
    );
}

void FToolSDFGenerator::CreateTextureOnRenderThread(TUniquePtr<FComputeData> Data)
{
    check(IsInRenderingThread());
    check(Data.IsValid());

    const int32 TextureSize = Data->TextureSize;
    const int32 TotalVoxels = TextureSize * TextureSize * TextureSize;
    const int32 BytesPerVoxel = sizeof(float);
    const int32 TotalBytes = TotalVoxels * BytesPerVoxel; 

    // 1. 验证数据有效性
    if (Data->VolumeData.Num() != TotalVoxels || TotalBytes <= 0)
    {
        UE_LOG(LogTemp, Error, TEXT("SDF数据无效：期望%d个体素，实际%d个"), TotalVoxels, Data->VolumeData.Num());
        if (Data->CompleteCallback)
        {
            AsyncTask(ENamedThreads::GameThread, [CompleteCallback = MoveTemp(Data->CompleteCallback)]()
            {
                CompleteCallback(false);
            });
        }
        return;
    }
    
    // 2. 计算D3D12纹理数据布局参数
    const uint32 SourceRowPitch = TextureSize * BytesPerVoxel;       // 一行（X）的字节数
    const uint32 SourceDepthPitch = SourceRowPitch * TextureSize;    // 一层（X*Y）的字节数

    // 2. 创建空的3D纹理（无初始BulkData）
    FRHITextureCreateDesc TextureDesc = FRHITextureCreateDesc::Create3D(TEXT("ToolSDFVolumeTexture"),
        FIntVector(TextureSize,TextureSize,TextureSize),
        PF_R32_FLOAT)
        .SetNumMips(1)
        .SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUWritable) // 允许CPU更新
        .SetInitialState(ERHIAccess::CopyDest);                      // 初始状态为拷贝目标

    FTextureRHIRef NewTextureRHI = RHICreateTexture(TextureDesc);
    if (!NewTextureRHI.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("创建SDF 3D纹理失败"));
        if (Data->CompleteCallback)
        {
            AsyncTask(ENamedThreads::GameThread, [CompleteCallback = MoveTemp(Data->CompleteCallback)]()
            {
                CompleteCallback(false);
            });
        }
        return;
    }

    // 3. 通过RHICmdList上传数据（D3D12推荐方式）
    FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
    
    // 准备数据指针（确保是连续内存）
    const uint8* SDFDataPtr = reinterpret_cast<const uint8*>(Data->VolumeData.GetData());
    const FUpdateTextureRegion3D UpdateRegion(
        0, 0, 0,                    // 起始X/Y/Z
        0, 0, 0,                    // 起始Mip/Array/Slice
        TextureSize, TextureSize, TextureSize // 宽度/高度/深度
    );

    // 上传数据到纹理
    RHICmdList.UpdateTexture3D(
        NewTextureRHI,              // 目标纹理
        0,                          // Mip层级
        UpdateRegion,              
        SourceRowPitch,            
        SourceDepthPitch,
        SDFDataPtr
    );

    // 4. 将纹理状态切换为Shader可读
    RHICmdList.Transition(FRHITransitionInfo(
        NewTextureRHI,
        ERHIAccess::CopyDest,
        ERHIAccess::SRVMask
    ));

    // 5. 线程安全更新成员变量
    {
        FScopeLock Lock(&TextureCritical);
        SDFTextureRHI = NewTextureRHI;
        SDFBounds = Data->Bounds;
        VolumeSize = TextureSize;
    }

    // 6. 通知完成（游戏线程）
    if (Data->CompleteCallback)
    {
        AsyncTask(ENamedThreads::GameThread, [CompleteCallback = MoveTemp(Data->CompleteCallback), NewTextureRHI]()
        {
            CompleteCallback(NewTextureRHI.IsValid());
        });
    }
}