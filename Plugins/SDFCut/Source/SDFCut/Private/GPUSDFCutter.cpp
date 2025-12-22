// Fill out your copyright notice in the Description page of Project Settings.


#include "GPUSDFCutter.h"

#include "Engine/VolumeTexture.h"
#include "RenderGraphUtils.h"
#include "UpdateSDFShader.h"
#include "Engine/TextureRenderTargetVolume.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TextureResource.h"
#include "RHIStaticStates.h"
#include "RenderResource.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Components/StaticMeshComponent.h"


IMPLEMENT_UNIFORM_BUFFER_STRUCT(FCutUB, "CutUB");
IMPLEMENT_GLOBAL_SHADER(FUpdateSDFCS, "/SDF/Shaders/DynamicSDFUpdateCS.usf", "LocalSDFUpdateKernel", SF_Compute);


UGPUSDFCutter::UGPUSDFCutter()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

bool UGPUSDFCutter::WorldToVoxelSpace(const FVector& WorldLocation, FVector& OutVoxelCoord) const
{
	if (!TargetMeshComponent)
	{
		return false;
	}

	// 1. 世界空间 -> 局部空间
	// 使用 Actor 的变换矩阵将世界坐标转为局部坐标
	FVector LocalPos = TargetMeshComponent->GetComponentTransform().InverseTransformPosition(WorldLocation);

	// 2. 边界检查 (Broad Phase)
	// 如果点在模型的局部包围盒之外，直接认为无效
	// TargetLocalBounds 是在 InitResources 中计算的
	if (!TargetLocalBounds.IsInside(LocalPos))
	{
		return false;
	}

	// 3. 局部空间 -> 体素空间
	// 相对坐标 = 当前点 - 包围盒最小点
	FVector RelativePos = LocalPos - TargetLocalBounds.Min;

	// 体素坐标 = 相对坐标 / 单个体素大小
	OutVoxelCoord = RelativePos / VoxelSize;

	// 4. 安全性检查 (防止浮点误差导致越界)
	// 虽然 IsInside 已经检查过了，但为了保险起见，确保坐标在 [0, Dimensions] 之间
	if (OutVoxelCoord.X < 0.0f || OutVoxelCoord.X > (float)SDFDimensions.X ||
		OutVoxelCoord.Y < 0.0f || OutVoxelCoord.Y > (float)SDFDimensions.Y ||
		OutVoxelCoord.Z < 0.0f || OutVoxelCoord.Z > (float)SDFDimensions.Z)
	{
		return false;
	}

	return true;
}

float UGPUSDFCutter::SampleSDF(const FVector& VoxelCoord) const
{
	// 基础体素坐标（左下后）
	int32 X0 = FMath::FloorToInt(VoxelCoord.X);
	int32 Y0 = FMath::FloorToInt(VoxelCoord.Y);
	int32 Z0 = FMath::FloorToInt(VoxelCoord.Z);

	int32 X1 = X0 + 1;
	int32 Y1 = Y0 + 1;
	int32 Z1 = Z0 + 1;

	// 插值权重
	float Alpha = VoxelCoord.X - X0;
	float Beta  = VoxelCoord.Y - Y0;
	float Gamma = VoxelCoord.Z - Z0;

	// 采样8个角点
	float C000 = CPU_SDFData[GetVoxelIndex(X0, Y0, Z0)].R.GetFloat();
	float C100 = CPU_SDFData[GetVoxelIndex(X1, Y0, Z0)].R.GetFloat();
	float C010 = CPU_SDFData[GetVoxelIndex(X0, Y1, Z0)].R.GetFloat();
	float C110 = CPU_SDFData[GetVoxelIndex(X1, Y1, Z0)].R.GetFloat();
  
	float C001 = CPU_SDFData[GetVoxelIndex(X0, Y0, Z1)].R.GetFloat();
	float C101 = CPU_SDFData[GetVoxelIndex(X1, Y0, Z1)].R.GetFloat();
	float C011 = CPU_SDFData[GetVoxelIndex(X0, Y1, Z1)].R.GetFloat();
	float C111 = CPU_SDFData[GetVoxelIndex(X1, Y1, Z1)].R.GetFloat();

	// X轴插值
	float C00 = FMath::Lerp(C000, C100, Alpha);
	float C10 = FMath::Lerp(C010, C110, Alpha);
	float C01 = FMath::Lerp(C001, C101, Alpha);
	float C11 = FMath::Lerp(C011, C111, Alpha);

	// Y轴插值
	float C0 = FMath::Lerp(C00, C10, Beta);
	float C1 = FMath::Lerp(C01, C11, Beta);

	// Z轴插值
	return FMath::Lerp(C0, C1, Gamma);
}

int32 UGPUSDFCutter::SampleMaterialID(const FVector& VoxelCoord) const
{
	// 如果数据未初始化，返回默认ID (例如 0)
	if (CPU_SDFData.Num() == 0)
	{
		return 0;
	}
	// 1. 最近邻采样 (Nearest Neighbor)
	// 材质ID是整数，不能插值。直接四舍五入找到最近的体素中心。
	int32 X = FMath::RoundToInt(VoxelCoord.X);
	int32 Y = FMath::RoundToInt(VoxelCoord.Y);
	int32 Z = FMath::RoundToInt(VoxelCoord.Z);

	// 2. 获取数组索引 (复用现有的 GetVoxelIndex 函数，它内部包含了 Clamp 逻辑)
	int32 Index = GetVoxelIndex(X, Y, Z);

	// 3. 读取数据
	if (CPU_SDFData.IsValidIndex(Index))
	{
		// G 通道存储材质 ID (float -> int)
		return FMath::RoundToInt(CPU_SDFData[Index].G.GetFloat());
	}

	return 0; // Fallback
}

void UGPUSDFCutter::BeginPlay()
{
	Super::BeginPlay();

	if (OriginalSDFTexture && ToolSDFTexture && TargetMeshComponent)
	{
		InitResources();
	}
}

FVector UGPUSDFCutter::CalculateGradientAtVoxel(const FVector& VoxelCoord) const
{
	// 步长：1个体素
	const float E = 1.0f; 
    
	// 这里为了速度，假设 SampleSDFTrilinear 内部已经处理了 Clamp
	// 如果没有，必须在这里处理，否则数组越界会 Crash
    
	float Dx = SampleSDF(VoxelCoord + FVector(E, 0, 0)) - SampleSDF(VoxelCoord - FVector(E, 0, 0));
	float Dy = SampleSDF(VoxelCoord + FVector(0, E, 0)) - SampleSDF(VoxelCoord - FVector(0, E, 0));
	float Dz = SampleSDF(VoxelCoord + FVector(0, 0, E)) - SampleSDF(VoxelCoord - FVector(0, 0, E));

	return FVector(Dx, Dy, Dz); // 不需要归一化，外面会做
}

void UGPUSDFCutter::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	if (!bGPUResourcesInitialized || !TargetMeshComponent || !CutToolComponent)
		return;

	UpdateToolTransform();
	UpdateTargetTransform();

	// 只有工具位置变化才发送新请求
	if (bRelativeTransformDirty)
	{
		DispatchLocalUpdate();
		bRelativeTransformDirty = false;
	}
}

void UGPUSDFCutter::InitResources()
{
	if (bGPUResourcesInitialized)
	{
		UE_LOG(LogTemp, Error, TEXT("GPUResources Already Initlialized"));
		return;
	}

	// 计算SDF尺寸
	SDFDimensions = FIntVector(
		OriginalSDFTexture->GetSizeX(),
		OriginalSDFTexture->GetSizeY(),
		OriginalSDFTexture->GetSizeZ()
	);

	// 切削对象的LocalBounds
	TargetLocalBounds = TargetMeshComponent->CalcLocalBounds().GetBox();
	
	// 像素各个维度均等，选任意轴向均可
	VoxelSize = TargetLocalBounds.GetSize().X / SDFDimensions.X;

	CalculateToolDimensions();

	CurrentToolTransform = CutToolComponent->GetComponentTransform();

	// 创建VolumeRT
	VolumeRT = UKismetRenderingLibrary::CreateRenderTargetVolume(this, SDFDimensions.X, SDFDimensions.Y, SDFDimensions.Z, RTF_RGBA16f, FLinearColor::Black, false, true);
    VolumeRT->bCanCreateUAV = true;

	// 创建SDF渲染材质实例
	SDFMaterialInstanceDynamic = UMaterialInstanceDynamic::Create(SDFMaterialInstance, this);
	if (VolumeRT)
	{
		SDFMaterialInstanceDynamic->SetTextureParameterValue(FName("VolumeTexture"), VolumeRT);
	}
	// 分配材质实例给目标网格
	TargetMeshComponent->SetMaterial(0, SDFMaterialInstanceDynamic);

	InitCPUData();

	// 存储外部纹理的RHI引用(静态图片，可以直接获取RHI
	OriginalSDFRHIRef = OriginalSDFTexture->GetResource()->GetTextureRHI();	
	ToolSDFRHIRef = ToolSDFTexture->GetResource()->GetTextureRHI();

	// 刚刚创建的资源，获取资源对象的指针（CPU端的FTextureResource指针是立即有效的， RHI则不一定）
	FTextureResource* DestVolumeResource = VolumeRT->GetResource();

	ENQUEUE_RENDER_COMMAND(InitGPUSDFCutterResources)([this, OriginalTextureRHI = OriginalSDFRHIRef, DestVolumeResource](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		// 注册外部资源
		FRDGTextureRef OriginalTextureRef = RegisterExternalTexture(GraphBuilder, OriginalTextureRHI, TEXT("VolumeTexture"));

		// 此时 UpdateResource 发出的 InitRHI 命令保证已经执行完成
		FRHITexture* DestVolumeRHI = DestVolumeResource->GetTextureRHI();
		FRDGTextureRef VolumeRTTextureRef = RegisterExternalTexture(GraphBuilder, DestVolumeRHI, TEXT("DestVolumeRT"));
		
		// 复制整个纹理
		AddCopyTexturePass(GraphBuilder, OriginalTextureRef, VolumeRTTextureRef);

		GraphBuilder.Execute();
	});

	bGPUResourcesInitialized = true;
}

void UGPUSDFCutter::UpdateToolTransform()
{
	FTransform NewTransform = CutToolComponent->GetComponentTransform();
	if (!CurrentToolTransform.Equals(NewTransform))
	{
		CurrentToolTransform = NewTransform;
		bRelativeTransformDirty = true;
	}
}

void UGPUSDFCutter::UpdateTargetTransform()
{
	FTransform NewTransform = TargetMeshComponent->GetComponentTransform();
	if (!CurrentTargetTransform.Equals(NewTransform))
	{
		CurrentTargetTransform = NewTransform;
		bRelativeTransformDirty = true;
	}
}

bool UGPUSDFCutter::GetSDFValueAndNormal(FVector WorldLocation, float& OutSDFValue, FVector& OutNormal,int32& OutMaterialID)
{
	if (CPU_SDFData.Num() == 0 || !TargetMeshComponent)
	{
		OutSDFValue = 0.0f;
		OutNormal = FVector::UpVector;
		OutMaterialID = -1;//无效ID
		return false;
	}

	// Use the StaticMeshComponent's local space for sampling to match how bounds/voxels were computed

	if (!TargetMeshComponent)
	{
		OutSDFValue = 0.0f;
		OutNormal = FVector::UpVector;
		OutMaterialID = -1;
		return false;
	}

	// Convert world position to the mesh component's local space
	FVector LocalPos = TargetMeshComponent->GetComponentTransform().InverseTransformPosition(WorldLocation);

	// Compute local bounds from the component (matches CalcLocalBounds used elsewhere)
	FBox LocalBounds = TargetMeshComponent->CalcLocalBounds().GetBox();

	// If outside the target bounds, indicate failure
	if (!LocalBounds.IsInside(LocalPos))
	{
		OutSDFValue = TNumericLimits<float>::Max();
		OutNormal = (WorldLocation - TargetMeshComponent->GetComponentLocation()).GetSafeNormal();
		OutMaterialID = -1; // 无效 ID
		return false;
	}

	// Local space -> voxel space (0..Dimensions)
	FVector RelativePos = LocalPos - LocalBounds.Min;
	FVector VoxelCoord = RelativePos / VoxelSize;

	// Sample SDF value (trilinear)
	OutSDFValue = SampleSDF(VoxelCoord);
	
	// 读取材质 ID（G 通道）
	int32 VoxelIndex = GetVoxelIndex(FMath::FloorToInt(VoxelCoord.X), FMath::FloorToInt(VoxelCoord.Y), FMath::FloorToInt(VoxelCoord.Z));
	
	float RawMaterialVal = CPU_SDFData[VoxelIndex].G.GetFloat();
	OutMaterialID = FMath::RoundToInt(RawMaterialVal); 

	// Compute normal via central differences in voxel space
	const float Epsilon = 1.0f; // in voxel units
	float ValXPlus  = SampleSDF(VoxelCoord + FVector(Epsilon, 0, 0));
	float ValXMinus = SampleSDF(VoxelCoord - FVector(Epsilon, 0, 0));
	float ValYPlus  = SampleSDF(VoxelCoord + FVector(0, Epsilon, 0));
	float ValYMinus = SampleSDF(VoxelCoord - FVector(0, Epsilon, 0));
	float ValZPlus  = SampleSDF(VoxelCoord + FVector(0, 0, Epsilon));
	float ValZMinus = SampleSDF(VoxelCoord - FVector(0, 0, Epsilon));

	FVector Gradient(
		ValXPlus - ValXMinus,
		ValYPlus - ValYMinus,
		ValZPlus - ValZMinus
	);

	// Transform gradient (component local) to world space normal
	OutNormal = TargetMeshComponent->GetComponentTransform().TransformVector(Gradient);
	OutNormal.Normalize();

	return true;
}

float UGPUSDFCutter::CalculateCurrentVolume(int32 MaterialID, bool bWorldSpace)
{
	if (CPU_SDFData.Num() == 0 || !TargetMeshComponent)
	{
		return 0.0f;
	}

	// 1. 计算单个体素的体积 (Local Space)
	// VoxelSize 是在 InitResources 中根据 Bounds 和 Dimensions 计算出的单边长
	// 假设体素是完美的立方体
	const float SingleVoxelVolume = VoxelSize * VoxelSize * VoxelSize;

	// 2. 统计在表面内部的体素数量 (SDF <= 0)
	// 使用原子操作以支持并行计算
	std::atomic<int32> InsideVoxelCount(0);

	// 并行遍历整个 SDF 数组
	ParallelFor(CPU_SDFData.Num(), [&](int32 Index)
	{
		
		// 假设 SDF <= 0 表示物体内部 
		if (CPU_SDFData[Index].R <= 0.0f )
		{
			int32 MaterialIDRead = FMath::RoundToInt(CPU_SDFData[Index].G.GetFloat());
			if (MaterialIDRead == MaterialID)
			{
				++InsideVoxelCount;
			}			
		}
	});

	// 3. 计算 Local 空间总体积
	float LocalVolume = InsideVoxelCount.load() * SingleVoxelVolume;

	// 4. 如果需要世界空间体积，乘以 Actor 的缩放系数
	if (bWorldSpace)
	{
		FVector ActorScale = TargetMeshComponent->GetComponentScale();
		// 体积缩放是三个轴向缩放的乘积
		float ScaleFactor = FMath::Abs(ActorScale.X * ActorScale.Y * ActorScale.Z);
		return LocalVolume * ScaleFactor;
	}

	return LocalVolume;
}

void UGPUSDFCutter::InitCPUData()
{
    // Initialize array size
    int32 TotalVoxels = SDFDimensions.X * SDFDimensions.Y * SDFDimensions.Z;
    CPU_SDFData.SetNumUninitialized(TotalVoxels);

    // Read initial data from the original texture asset into CPU cache
    FTexturePlatformData* PlatformData = OriginalSDFTexture->GetPlatformData();
    if (PlatformData && PlatformData->Mips.Num() > 0)
    {
        const void* RawData = PlatformData->Mips[0].BulkData.LockReadOnly();

        // Assuming the source format is FFloat16Color Float
    	const FFloat16Color* SourceData = static_cast<const FFloat16Color*>(RawData);
        FMemory::Memcpy(CPU_SDFData.GetData(), SourceData, CPU_SDFData.Num() * sizeof(FFloat16Color));

        PlatformData->Mips[0].BulkData.Unlock();
    }
    else
    {
        // If no valid data, initialize to zero
        FMemory::Memzero(CPU_SDFData.GetData(), CPU_SDFData.Num() * sizeof(FFloat16Color));
    }
}

void UGPUSDFCutter::UpdateCPUDataPartial(FIntVector UpdateMin, FIntVector UpdateSize, TArray<FFloat16Color>& LocalData)
{
	// 校验数据大小是否匹配
	int32 ExpectedSize = UpdateSize.X * UpdateSize.Y * UpdateSize.Z;
	if (LocalData.Num() != ExpectedSize)
	{
		// 可能是尺寸计算有细微偏差（如边界处理），做个保护
		return;
	}

	if (CPU_SDFData.Num() != SDFDimensions.X * SDFDimensions.Y * SDFDimensions.Z)
	{
		// 如果主缓存还没初始化，无法局部更新
		return;
	}

	// 遍历局部数据，填入全局数组
	// 这是一个三重循环，但只针对切削的小区域，速度极快
	int32 LocalIndex = 0;
	for (int32 z = 0; z < UpdateSize.Z; z++)
	{
		int32 GlobalZ = UpdateMin.Z + z;
		if (GlobalZ >= SDFDimensions.Z) break;

		for (int32 y = 0; y < UpdateSize.Y; y++)
		{
			int32 GlobalY = UpdateMin.Y + y;
			if (GlobalY >= SDFDimensions.Y) break;

			// 优化：内存拷贝一行 (X轴)
			// 计算全局数组的起始索引
			int32 GlobalStartIndex = GlobalZ * (SDFDimensions.X * SDFDimensions.Y) + GlobalY * SDFDimensions.X + UpdateMin.X;
            
			// 计算当前行的有效拷贝长度 (防止X越界)
			int32 CopyCount = FMath::Min(UpdateSize.X, SDFDimensions.X - UpdateMin.X);
            
			if (CopyCount > 0)
			{
				FMemory::Memcpy(
					&CPU_SDFData[GlobalStartIndex], 
					&LocalData[LocalIndex], 
					CopyCount * sizeof(FFloat16Color)
				);
			}

			// 移动局部索引指针
			LocalIndex += UpdateSize.X; 
		}
	}
}

int32 UGPUSDFCutter::GetVoxelIndex(int32 X, int32 Y, int32 Z) const
{
	// Clamp 坐标防止越界
	X = FMath::Clamp(X, 0, SDFDimensions.X - 1);
	Y = FMath::Clamp(Y, 0, SDFDimensions.Y - 1);
	Z = FMath::Clamp(Z, 0, SDFDimensions.Z - 1);
	return Z * (SDFDimensions.X * SDFDimensions.Y) + Y * SDFDimensions.X + X;
}

void UGPUSDFCutter::CalculateToolAABBInTargetSpace(const FTransform& ToolTransform, FIntVector& OutVoxelMin,
                                                   FIntVector& OutVoxelMax)
{
	if (!CutToolComponent || !TargetMeshComponent)
	{
		return;
	}

	// Target 这里表示被切削对象的局部坐标系
	FTransform TargetToWorld = TargetMeshComponent->GetComponentTransform();
	FTransform WorldToTarget = TargetToWorld.Inverse();
	FTransform ToolToTarget = ToolTransform * WorldToTarget;

	FBox ToolBoundsInTargetSpace = ToolLocalBounds.TransformBy(ToolToTarget);

	// 求交集
	FBox IntersectionBounds = ToolBoundsInTargetSpace.Overlap(TargetLocalBounds);

	if (!IntersectionBounds.IsValid)
	{
		OutVoxelMin = FIntVector(0, 0, 0);
		OutVoxelMax = FIntVector(0, 0, 0);
		return;
	}

	// 5. 将交集区域转换为体素坐标
	FVector VoxelMin = (IntersectionBounds.Min - TargetLocalBounds.Min) / VoxelSize;
	FVector VoxelMax = (IntersectionBounds.Max - TargetLocalBounds.Min) / VoxelSize;

	OutVoxelMin = FIntVector(
		FMath::FloorToInt(VoxelMin.X),
		FMath::FloorToInt(VoxelMin.Y),
		FMath::FloorToInt(VoxelMin.Z)
	);

	OutVoxelMax = FIntVector(
		FMath::CeilToInt(VoxelMax.X),
		FMath::CeilToInt(VoxelMax.Y),
		FMath::CeilToInt(VoxelMax.Z)
	);

	// 限制在有效范围内
	OutVoxelMin = OutVoxelMin.ComponentMax(FIntVector(0, 0, 0)); // 下限不能是负数
	OutVoxelMax = OutVoxelMax.ComponentMin(SDFDimensions); // 上限不能超过尺寸
}

void UGPUSDFCutter::DispatchLocalUpdate()
{
	if (!bGPUResourcesInitialized) return;

    // 1. 计算更新区域
    FIntVector UpdateMin, UpdateMax;
    CalculateToolAABBInTargetSpace(CurrentToolTransform, UpdateMin, UpdateMax);

    // 校验区域
    if (UpdateMin.X >= UpdateMax.X || UpdateMin.Y >= UpdateMax.Y || UpdateMin.Z >= UpdateMax.Z)
    {
    	UE_LOG(LogTemp,Warning,TEXT("Invalid update region, skipping"));
        return;
    }

    // 防止重入（如果上一次回读还没完成，可以选择跳过或者排队，这里简单跳过）
    if (bIsReadingBack) return;
    bIsReadingBack = true;

    // 2. 准备Shader参数
    FTransform TargetSpaceToToolSpaceTransform = CurrentTargetTransform * CurrentToolTransform.Inverse();
    FBox CapturedTargetBounds = TargetLocalBounds;
    FBox CapturedToolBounds = ToolLocalBounds;
    FIntVector CapturedSDFDimensions = SDFDimensions;

    FTextureResource* ToolResource = ToolSDFTexture->GetResource();
    FTextureResource* RenderTargetResource = VolumeRT->GetResource();

    // 3. 发送渲染命令
    ENQUEUE_RENDER_COMMAND(GPUSDFCutter_LocalUpdate)(
        [this, UpdateMin, UpdateMax,
         CapturedTargetBounds, CapturedToolBounds,
         TargetSpaceToToolSpaceTransform,
         CapturedSDFDimensions,
         ToolResource, RenderTargetResource]
        (FRHICommandListImmediate& RHICmdList)
        {
            FRHITexture* ToolRHI = ToolResource ? ToolResource->GetTextureRHI() : nullptr;
            FRHITexture* VolumeRHI = RenderTargetResource ? RenderTargetResource->GetTextureRHI() : nullptr;

            if (!ToolRHI || !VolumeRHI)
            {
                bIsReadingBack = false;
                return;
            }

            FRDGBuilder GraphBuilder(RHICmdList);

            // --- A. 执行 Compute Shader 切削 (保持不变) ---
            FRDGTextureRef ToolTexture = RegisterExternalTexture(GraphBuilder, ToolRHI, TEXT("ToolSDF"));
            FRDGTextureRef VolumeRTTexture = RegisterExternalTexture(GraphBuilder, VolumeRHI, TEXT("VolumeRT"));

            auto* CutUBParams = GraphBuilder.AllocParameters<FCutUB>();
            CutUBParams->TargetLocalBoundsMin = FVector3f(CapturedTargetBounds.Min);
            CutUBParams->TargetLocalBoundsMax = FVector3f(CapturedTargetBounds.Max);
            CutUBParams->ToolLocalBoundsMin = FVector3f(CapturedToolBounds.Min);
            CutUBParams->ToolLocalBoundsMax = FVector3f(CapturedToolBounds.Max);
            CutUBParams->TargetToToolTransform = FMatrix44f(TargetSpaceToToolSpaceTransform.ToMatrixWithScale());
            CutUBParams->SDFDimensions = CapturedSDFDimensions;
            CutUBParams->UpdateRegionMin = UpdateMin;
            CutUBParams->UpdateRegionMax = UpdateMax;

            auto* PassParams = GraphBuilder.AllocParameters<FUpdateSDFCS::FParameters>();
            PassParams->Params = GraphBuilder.CreateUniformBuffer(CutUBParams);
            PassParams->InputSDF = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(VolumeRTTexture));
            PassParams->ToolSDF = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ToolTexture));
            PassParams->OutputSDF = GraphBuilder.CreateUAV(VolumeRTTexture);
            PassParams->InputSDFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
            PassParams->ToolSDFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

            FIntVector RegionSize = UpdateMax - UpdateMin; // 注意：这里不要加1，作为Size使用
            // 修正GroupCount计算，确保覆盖所有体素
            FIntVector DispatchSize = RegionSize + FIntVector(1, 1, 1); 
            FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(DispatchSize, FIntVector(4, 4, 4));

            TShaderMapRef<FUpdateSDFCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("LocalSDFUpdate"),
                ComputeShader,
                PassParams,
                GroupCount
            );

            // --- B. 局部回读准备 (新增核心逻辑) ---

            // 1. 创建一个临时的 RHI 纹理作为 Staging Buffer (大小等于切削区域)
            // 我们需要显式创建 RHI 资源以便在 GraphBuilder 执行后依然能访问它进行读取
            const FRDGTextureDesc  StagingDesc =
                FRDGTextureDesc::Create3D(RegionSize, PF_FloatRGBA,FClearValueBinding::None,TexCreate_ShaderResource);

			// 2. 直接在 RDG 中创建纹理
			FRDGTextureRef StagingTextureRDG = GraphBuilder.CreateTexture(StagingDesc, TEXT("SDFStagingRDG"));
        	
            // 3. 添加 Copy Pass：只拷贝切削区域
            FRHICopyTextureInfo CopyInfo;
            CopyInfo.Size = RegionSize;
            CopyInfo.SourcePosition = UpdateMin; // 从大图的这个位置开始
            CopyInfo.DestPosition = FIntVector::ZeroValue; // 拷贝到小图的 (0,0,0)

            AddCopyTexturePass(GraphBuilder, VolumeRTTexture, StagingTextureRDG, CopyInfo);

			// 4. 【关键步骤】提取资源
			// 我们需要一个 TRefCountPtr<IPooledRenderTarget> 来承接 RDG 分配的资源
			TRefCountPtr<IPooledRenderTarget> StagingPooledRenderTarget;
			GraphBuilder.QueueTextureExtraction(StagingTextureRDG, &StagingPooledRenderTarget);

            // 4. 执行 RDG
            // GraphBuilder.Execute() 会执行 CS 和 Copy，此时 StagingTextureRHI 里就有了最新的局部数据
            GraphBuilder.Execute();

            // --- C. 执行回读 ---
            
			// 获取底层的 RHI 纹理
    		FRHITexture* StagingRHI = StagingPooledRenderTarget->GetRHI();

            // 准备接收数据的数组
        	TArray<FFloat16Color> TempPixels;
            // 读取 Staging Texture (它很小，所以很快)
            // 注意：Read3DSurfaceFloatData 会导致 CPU 等待 GPU 完成 Copy 操作
            RHICmdList.Read3DSurfaceFloatData(StagingRHI, FIntRect(0, 0, RegionSize.X, RegionSize.Y), FIntPoint(0, RegionSize.Z), TempPixels);

        	// 3. 转换为 float 数组
			TArray<FFloat16Color> LocalFloatPixels;
			LocalFloatPixels.SetNumUninitialized(TempPixels.Num());
        	
        	// 并行转换数据 (SDF通常存储在R通道)
        	ParallelFor(TempPixels.Num(), [&](int32 i)
			{
				LocalFloatPixels[i] = TempPixels[i];
			}, EParallelForFlags::None);
        				
            // --- D. 返回 GameThread 更新数据 ---            
            // 使用 MoveTemp 转移数据所有权，避免拷贝
            AsyncTask(ENamedThreads::GameThread, [this, LocalData = MoveTemp(LocalFloatPixels), UpdateMin, RegionSize]() mutable
            {
                UpdateCPUDataPartial(UpdateMin, RegionSize, LocalData);
                bIsReadingBack = false;
            });
        });
}

void UGPUSDFCutter::CalculateToolDimensions()
{

	ToolLocalBounds = CutToolComponent->CalcLocalBounds().GetBox();
	// 计算工具尺寸
	ToolOriginalSize = ToolLocalBounds.GetSize();
}