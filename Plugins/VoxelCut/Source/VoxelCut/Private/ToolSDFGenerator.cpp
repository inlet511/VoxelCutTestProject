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
                int16 Encoded = (int16)FMath::Clamp(SignedDist, -32767.0f, 32767.0f);
                int32 Index = Z * (Data->TextureSize * Data->TextureSize) + Y * Data->TextureSize + X;
                if (Index < TotalVoxels) // 边界检查
                {
                    Data->VolumeData[Index] = Encoded;
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

    // 创建纹理描述
    FRHITextureCreateDesc TextureDesc = FRHITextureCreateDesc::Create3D(TEXT("ToolSDFVolumeTexture"))
        .SetExtent(Data->TextureSize)
        .SetFormat(PF_R16_SINT)
        .SetNumMips(1)
        .SetFlags(ETextureCreateFlags::ShaderResource)
        .SetInitialState(ERHIAccess::SRVMask);

    // 复制体积数据到渲染线程可访问的内存
    TArray<int16> SDFVolumeData = MoveTemp(Data->VolumeData);

    // 创建包含初始数据的批量数据对象
    struct FSDFBulkData : public FResourceBulkDataInterface
    {
        FSDFBulkData(TArray<int16>&& InData) : Data(MoveTemp(InData)) {}
        
        virtual const void* GetResourceBulkData() const override { return Data.GetData(); }
        virtual uint32 GetResourceBulkDataSize() const override { return Data.Num() * sizeof(int16); }
        virtual void Discard() override {}
        
    private:
        TArray<int16> Data;
    };

    TextureDesc.SetBulkData(new FSDFBulkData(MoveTemp(SDFVolumeData)));

    // 创建纹理资源
    FTextureRHIRef NewTextureRHI = RHICreateTexture(TextureDesc);

    // 线程安全地更新成员变量
    FScopeLock Lock(&TextureCritical);
    SDFTextureRHI = NewTextureRHI;
    SDFBounds = Data->Bounds;
    VolumeSize = Data->TextureSize;

    // 通知完成（回调在游戏线程执行）
    if (Data->CompleteCallback)
    {
        // 如果需要在游戏线程执行回调，使用AsyncTask切换
        AsyncTask(ENamedThreads::GameThread, [CompleteCallback = MoveTemp(Data->CompleteCallback), NewTextureRHI]()
        {
            CompleteCallback(NewTextureRHI.IsValid());
        });
    }
}