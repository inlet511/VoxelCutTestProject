// Fill out your copyright notice in the Description page of Project Settings.


#include "SDFGenLibrary.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"

UVolumeTexture* USDFGenLibrary::GenerateSDFFromStaticMesh(UStaticMesh* InputMesh, FString PackagePath,
                                                          FString AssetName, int32 ResolutionXY, int32 Slices, float BoundsScale)
{
if (!InputMesh)
    {
        UE_LOG(LogTemp, Error, TEXT("Input Mesh is null!"));
        return nullptr;
    }

    // 1. 获取 MeshDescription 并转换为 DynamicMesh3
    // -----------------------------------------------------------------------
    UE::Geometry::FDynamicMesh3 DynMesh;
    FMeshDescriptionToDynamicMesh Converter;
    
    // 获取 LOD 0 的 MeshDescription
    const FMeshDescription* MeshDesc = InputMesh->GetMeshDescription(0);
    if (!MeshDesc)
    {
        UE_LOG(LogTemp, Error, TEXT("Cannot get MeshDescription from StaticMesh. Ensure 'Allow CPU Access' is on if shipping."));
        return nullptr;
    }

    Converter.Convert(MeshDesc, DynMesh);

    // 2. 构建空间查询结构 (AABB Tree 和 Winding Number Engine)
    // -----------------------------------------------------------------------
    // AABB Tree 用于快速查找最近点距离
    UE::Geometry::TMeshAABBTree3<UE::Geometry::FDynamicMesh3> Spatial(&DynMesh);
    Spatial.Build();

    // Fast Winding Number 用于判断点在内部还是外部 (Inside/Outside)
    // 需要确保 Mesh 是封闭的 (Watertight) 效果才最好，但 FWN 对烂模型也有一定鲁棒性
    UE::Geometry::FDynamicMeshAABBTree3 WindingEngine(&DynMesh);
    WindingEngine.Build();

    // 3. 计算体素网格参数
    // -----------------------------------------------------------------------
    FBox Bounds = InputMesh->GetBoundingBox();
    FVector Center = Bounds.GetCenter();
    FVector Extent = Bounds.GetExtent() * BoundsScale; // 稍微扩大一点范围，避免边界SDF被切断
    FVector MinPos = Center - Extent;
    FVector Size = Extent * 2.0f;

    FVector VoxelSize;
    VoxelSize.X = Size.X / (float)ResolutionXY;
    VoxelSize.Y = Size.Y / (float)ResolutionXY;
    VoxelSize.Z = Size.Z / (float)Slices;

    int32 TotalVoxels = ResolutionXY * ResolutionXY * Slices;
    TArray<float> RawSDFData;
    RawSDFData.SetNumUninitialized(TotalVoxels);

    // 4. 并行计算 SDF
    // -----------------------------------------------------------------------
    ParallelFor(TotalVoxels, [&](int32 Index)
    {
        // 将 1D Index 转换为 3D 坐标索引
        int32 Z = Index / (ResolutionXY * ResolutionXY);
        int32 Temp = Index % (ResolutionXY * ResolutionXY);
        int32 Y = Temp / ResolutionXY;
        int32 X = Temp % ResolutionXY;

        // 计算当前体素在世界空间的位置 (采样中心点)
        FVector3d SamplePos;
        SamplePos.X = MinPos.X + (X + 0.5) * VoxelSize.X;
        SamplePos.Y = MinPos.Y + (Y + 0.5) * VoxelSize.Y;
        SamplePos.Z = MinPos.Z + (Z + 0.5) * VoxelSize.Z;

        // 1. 计算距离平方
        FVector3d NearestSurfPoint  = Spatial.FindNearestPoint(SamplePos);
        // FVector3d 支持直接相减和 SquaredLength()
       double DistSq = (SamplePos - NearestSurfPoint).SquaredLength();
       float Distance = (float)FMath::Sqrt(DistSq);

        // 2. 计算 Winding Number (判断内外)
        bool bIsInside = WindingEngine.IsInside(SamplePos);

        // SDF 定义: 内部为负，外部为正
        if (bIsInside)
        {
            Distance *= -1.0f;
        }

        // 存储数据
        RawSDFData[Index] = Distance;
    });

    // 5. 创建 Volume Texture 资源
    // -----------------------------------------------------------------------
    FString PackageName = PackagePath + AssetName;
    UPackage* Package = CreatePackage(*PackageName);
    Package->FullyLoad();

    UVolumeTexture* NewTexture = NewObject<UVolumeTexture>(Package, *AssetName, RF_Public | RF_Standalone | RF_MarkAsRootSet);
    
    // 设置纹理属性
    NewTexture->Source.Init(ResolutionXY, ResolutionXY, Slices, 1, ETextureSourceFormat::TSF_R32F);
    NewTexture->SRGB = false;
    NewTexture->CompressionSettings = TC_HDR; // 高精度
    NewTexture->MipGenSettings = TMGS_NoMipmaps; // 通常SDF不需要Mipmap，或者根据需求开启

    // 6. 填充纹理数据
    // -----------------------------------------------------------------------
    uint8* MipData = NewTexture->Source.LockMip(0);
    // TSF_R32F 对应 float，直接内存拷贝
    FMemory::Memcpy(MipData, RawSDFData.GetData(), RawSDFData.Num() * sizeof(float));
    NewTexture->Source.UnlockMip(0);

    // 7. 更新资源并保存
    // -----------------------------------------------------------------------
    NewTexture->UpdateResource();
    Package->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(NewTexture);

    // 这一步是可选的：保存到磁盘
    /*
    FString FilePath = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    UPackage::SavePackage(Package, NewTexture, *FilePath, SaveArgs);
    */

    UE_LOG(LogTemp, Log, TEXT("SDF Volume Texture Created: %s"), *PackageName);

    return NewTexture;
	
}
