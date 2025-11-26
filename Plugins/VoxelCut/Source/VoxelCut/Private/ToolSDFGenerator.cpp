#include "ToolSDFGenerator.h"
#include "RenderUtils.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/MeshTransforms.h"
#include "Spatial/FastWinding.h"
#include "RHI.h"
#include "RHIResources.h"


bool FToolSDFGenerator::PrecomputeSDF(
    const FDynamicMesh3& ToolMesh, 
    int32 TextureSize)
{
    VolumeSize = TextureSize;
    
    // 1. 计算工具网格在自身空间的边界
    FAxisAlignedBox3d OriginalBounds = ToolMesh.GetBounds();

    // 扩展边界以包含足够的周边区域
    SDFBounds.Min = OriginalBounds.Min;
    SDFBounds.Max = OriginalBounds.Max;
    FVector3d SDFExtent = SDFBounds.Max - SDFBounds.Min;

    // 2. 创建空间查询结构（用于距离计算）
    FDynamicMeshAABBTree3 ToolSpatial(&ToolMesh);
    TFastWindingTree<FDynamicMesh3> ToolWinding(&ToolSpatial);

    // 3. 初始化VolumeTexture数据
    TArray<uint8> SDFVolumeData;
    int32 TotalVoxels = VolumeSize * VolumeSize * VolumeSize;
    SDFVolumeData.SetNumZeroed(TotalVoxels);

    // 4. 并行计算每个体素的符号距离
    ParallelFor(VolumeSize, [&](int32 Z)
    {
        for (int32 Y = 0; Y < VolumeSize; Y++)
        {
            for (int32 X = 0; X < VolumeSize; X++)
            {
                // 计算体素在世界空间中的位置
                FVector3d VoxelUVW(
                    (double)X / (VolumeSize - 1),
                    (double)Y / (VolumeSize - 1), 
                    (double)Z / (VolumeSize - 1)
                );
                
                FVector3d SamplePos = SDFBounds.Min + VoxelUVW * SDFExtent;

                // 计算符号距离
                double NearestDistSqr;
                int32 NearestTriID = ToolSpatial.FindNearestTriangle(SamplePos, NearestDistSqr);
                
                float SignedDist;
                if (NearestTriID == IndexConstants::InvalidID)
                {
                    // 超出工具范围，设为最大距离
                    SignedDist = BoundsExpansion;
                }
                else
                {
                    double NearestDist = FMath::Sqrt(NearestDistSqr);
                    // 判断内外（内部为负，外部为正）
                    bool bInside = ToolWinding.IsInside(SamplePos);
                    SignedDist = bInside ? -NearestDist : NearestDist;
                }

                // 归一化到[0,1]范围并编码为8位
                float NormDist = FMath::Clamp(SignedDist / BoundsExpansion, -1.0f, 1.0f);
                uint8 Encoded = (uint8)((NormDist + 1.0f) * 0.5f * 255.0f);
                
                // 计算线性索引
                int32 Index = Z * (VolumeSize * VolumeSize) + Y * VolumeSize + X;
                SDFVolumeData[Index] = Encoded;
            }
        }
    });

    // 5. 创建和上传纹理数据
    FRHITextureCreateDesc TextureDesc = FRHITextureCreateDesc::Create3D(TEXT("ToolSDFVolumeTexture"))
        .SetExtent(VolumeSize)
        .SetFormat(PF_G8)
        .SetNumMips(1)
        .SetFlags(ETextureCreateFlags::ShaderResource)
        .SetInitialState(ERHIAccess::SRVMask);

    // 创建包含初始数据的批量数据对象
    class FSDFBulkData : public FResourceBulkDataInterface
    {
    public:
        FSDFBulkData(const TArray<uint8>& InData, int32 InSize) 
            : Data(InData), Size(InSize) {}
        
        virtual const void* GetResourceBulkData() const override { return Data.GetData(); }
        virtual uint32 GetResourceBulkDataSize() const override { return Size; }
        virtual void Discard() override {}
        
    private:
        const TArray<uint8>& Data;
        int32 Size;
    };

    FSDFBulkData* BulkData = new FSDFBulkData(SDFVolumeData, TotalVoxels);
    TextureDesc.SetBulkData(BulkData);

    // 创建纹理
    SDFTextureRHI = RHICreateTexture(TextureDesc);
    
    if (!SDFTextureRHI.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to create 3D texture for SDF"));
        delete BulkData;
        return false;
    }

    UE_LOG(LogTemp, Log, TEXT("Successfully created SDF Volume Texture: %dx%dx%d"), 
           VolumeSize, VolumeSize, VolumeSize);

    return true;
}