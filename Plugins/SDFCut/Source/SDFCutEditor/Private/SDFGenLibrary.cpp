// Fill out your copyright notice in the Description page of Project Settings.


#include "SDFGenLibrary.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Spatial/FastWinding.h"

using namespace UE::Geometry;

UVolumeTexture* USDFGenLibrary::GenerateSDFFromStaticMesh(UStaticMesh* InputMesh, FString PackagePath,
                                                          FString AssetName, int32 ResolutionXY, int32 Slices, float BoundsScale, bool bGenerate2D)
{
    if (!InputMesh)
    {
        UE_LOG(LogTemp, Error, TEXT("Input Mesh is null!"));
        return nullptr;
    }
    if (AssetName.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("AssetName is null!"));
        return nullptr;
    }
    // 自动修正路径：确保以 / 结尾
    if (!PackagePath.EndsWith(TEXT("/")))
    {
        PackagePath += TEXT("/");
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
    FDynamicMeshAABBTree3 AABBTree(&DynMesh);

    TFastWindingTree<FDynamicMesh3> FastWinding(&AABBTree);


    // 3. 计算体素网格参数
    // -----------------------------------------------------------------------
    FBox Bounds = InputMesh->GetBoundingBox();
    FVector Center = Bounds.GetCenter();
    FVector OriginalExtent = Bounds.GetExtent();

    double MaxHalfSize = OriginalExtent.GetMax();
    FVector CubicExtent = FVector(MaxHalfSize) * BoundsScale;

    FVector MinPos = Center - CubicExtent;
    FVector Size = CubicExtent * 2.0f;

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
        FVector3d NearestSurfPoint  = AABBTree.FindNearestPoint(SamplePos);
        // FVector3d 支持直接相减和 SquaredLength()
       double DistSq = (SamplePos - NearestSurfPoint).SquaredLength();
       float Distance = (float)FMath::Sqrt(DistSq);

        // 2. 计算 Winding Number (判断内外)
        bool bIsInside = FastWinding.IsInside(SamplePos);

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
    FString VolAssetName = AssetName;
    if (!VolAssetName.EndsWith(TEXT("_Vol")))
    {
        VolAssetName += TEXT("_Vol");
    }

    FString VolPackageName = PackagePath + VolAssetName;

    // 检查是否已经存在一个不同类型的资产 (防止 Crash)
    UObject* ExistingObject = StaticFindObject(UObject::StaticClass(), nullptr, *VolPackageName);
    if (ExistingObject && ExistingObject->GetClass() != UVolumeTexture::StaticClass())
    {
        UE_LOG(LogTemp, Warning, TEXT("Name collision detected! Appending GUID to avoid crash."));
        VolAssetName += TEXT("_") + FGuid::NewGuid().ToString().Left(4);
        VolPackageName = PackagePath + VolAssetName;
    }

    UPackage* Package = CreatePackage(*VolPackageName);
    Package->FullyLoad();

    UVolumeTexture* NewTexture = NewObject<UVolumeTexture>(Package, *VolAssetName, RF_Public | RF_Standalone | RF_MarkAsRootSet);
    
    // 设置纹理属性
    NewTexture->Source.Init(ResolutionXY, ResolutionXY, Slices, 1, ETextureSourceFormat::TSF_R32F);
    NewTexture->SRGB = false;
    NewTexture->CompressionSettings = TC_SingleFloat; // 高精度
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

    // =========================================================
    // 6. 保存 2D Texture Atlas (POT 修正版)
    // =========================================================
    if (bGenerate2D)
    {
        // 算法：寻找最小的 POT 矩形布局
        // 初始为 1x1
        int32 NumCols = 1;
        int32 NumRows = 1;

        // 只要格子不够放 Slices，就倍增
        // 优先增加较小的一边，保持接近正方形
        while (NumCols * NumRows < Slices)
        {
            if (NumCols <= NumRows)
            {
                NumCols *= 2;
            }
            else
            {
                NumRows *= 2;
            }
        }

        // 此时 NumCols 和 NumRows 都是 2 的幂
        // 且 ResolutionXY 也是 2 的幂
        // 所以 AtlasWidth 和 AtlasHeight 必然是 2 的幂
        int32 AtlasWidth = NumCols * ResolutionXY;
        int32 AtlasHeight = NumRows * ResolutionXY;

        // 准备 2D 数据数组
        TArray<float> AtlasData;
        AtlasData.SetNumZeroed(AtlasWidth * AtlasHeight); // 初始化为0

        // 并行搬运数据
        ParallelFor(Slices, [&](int32 SliceIndex)
            {
                int32 GridX = SliceIndex % NumCols;
                int32 GridY = SliceIndex / NumCols;

                int32 XOffset = GridX * ResolutionXY;
                int32 YOffset = GridY * ResolutionXY;

                for (int32 y = 0; y < ResolutionXY; y++)
                {
                    for (int32 x = 0; x < ResolutionXY; x++)
                    {
                        int32 SrcIndex = SliceIndex * (ResolutionXY * ResolutionXY) + y * ResolutionXY + x;
                        int32 DestIndex = (YOffset + y) * AtlasWidth + (XOffset + x);

                        if (RawSDFData.IsValidIndex(SrcIndex))
                        {
                            AtlasData[DestIndex] = RawSDFData[SrcIndex];
                        }
                    }
                }
            });

        // 创建资源
        FString Tex2DAssetName = AssetName + TEXT("_Atlas");
        FString Tex2DPackageName = PackagePath + Tex2DAssetName;

        // 简单的重名检查
        UObject* ExistingAtlas = StaticFindObject(UObject::StaticClass(), nullptr, *Tex2DPackageName);
        if (ExistingAtlas && ExistingAtlas->GetClass() != UTexture2D::StaticClass())
        {
            Tex2DAssetName += TEXT("_") + FGuid::NewGuid().ToString().Left(4);
            Tex2DPackageName = PackagePath + Tex2DAssetName;
        }

        UPackage* Tex2DPackage = CreatePackage(*Tex2DPackageName);
        Tex2DPackage->FullyLoad();

        UTexture2D* NewTex2D = NewObject<UTexture2D>(Tex2DPackage, *Tex2DAssetName, RF_Public | RF_Standalone | RF_MarkAsRootSet);

        NewTex2D->Source.Init(AtlasWidth, AtlasHeight, 1, 1, ETextureSourceFormat::TSF_R32F);
        NewTex2D->SRGB = false;
        NewTex2D->CompressionSettings = TC_SingleFloat;
        NewTex2D->MipGenSettings = TMGS_NoMipmaps;
        NewTex2D->Filter = TF_Bilinear;
        NewTex2D->AddressX = TA_Clamp;
        NewTex2D->AddressY = TA_Clamp;

        // 关键：设置 PowerOfTwoMode 为 None，因为我们已经确保了它是 POT，不需要引擎再填充
        NewTex2D->PowerOfTwoMode = ETexturePowerOfTwoSetting::None;

        uint8* AtlasMipData = NewTex2D->Source.LockMip(0);
        FMemory::Memcpy(AtlasMipData, AtlasData.GetData(), AtlasData.Num() * sizeof(float));
        NewTex2D->Source.UnlockMip(0);

        NewTex2D->UpdateResource();
        Tex2DPackage->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(NewTex2D);

        UE_LOG(LogTemp, Log, TEXT("SDF 2D Atlas Created: %s (Size: %dx%d, Grid: %dx%d)"),
            *Tex2DPackageName, AtlasWidth, AtlasHeight, NumCols, NumRows);
    }

    return NewTexture;
	
}
