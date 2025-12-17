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

IMPLEMENT_UNIFORM_BUFFER_STRUCT(FCutUB, "CutUB");
IMPLEMENT_GLOBAL_SHADER(FUpdateSDFCS, "/SDF/Shaders/DynamicSDFUpdateCS.usf", "LocalSDFUpdateKernel", SF_Compute);

UGPUSDFCutter::UGPUSDFCutter()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UGPUSDFCutter::BeginPlay()
{
	Super::BeginPlay();

	if (OriginalSDFTexture && ToolSDFTexture && TargetMeshActor)
	{
		InitResources();
	}
}


void UGPUSDFCutter::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	if (!bGPUResourcesInitialized || !TargetMeshActor || !CutToolActor)
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
	TargetLocalBounds = TargetMeshActor->GetStaticMeshComponent()->CalcLocalBounds().GetBox();
	
	// 像素各个维度均等，选任意轴向均可
	VoxelSize = TargetLocalBounds.GetSize().X / SDFDimensions.X;

	CalculateToolDimensions();

	CurrentToolTransform = CutToolActor->GetTransform();

	// 创建VolumeRT
	int32 TextureSizeX = SDFDimensions.X;
	int32 TextureSizeY = SDFDimensions.Y;
	int32 TextureSizeZ = SDFDimensions.Z;
	VolumeRT = UKismetRenderingLibrary::CreateRenderTargetVolume(
		this,
		TextureSizeX,
		TextureSizeY,
		TextureSizeZ,
		RTF_R32f,
		FLinearColor::Black,
		false,
		true);
	VolumeRT->bCanCreateUAV = true;

	// 创建SDF渲染材质实例
	SDFMaterialInstanceDynamic = UMaterialInstanceDynamic::Create(SDFMaterialInstance, this);
	if (VolumeRT)
	{
		SDFMaterialInstanceDynamic->SetTextureParameterValue(FName("VolumeTexture"), VolumeRT);
	}
	// 分配材质实例给目标网格
	TargetMeshActor->GetStaticMeshComponent()->SetMaterial(0, SDFMaterialInstanceDynamic);


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
	FTransform NewTransform = CutToolActor->GetActorTransform();
	if (!CurrentToolTransform.Equals(NewTransform))
	{
		CurrentToolTransform = NewTransform;
		bRelativeTransformDirty = true;
	}
}

void UGPUSDFCutter::UpdateTargetTransform()
{
	FTransform NewTransform = TargetMeshActor->GetActorTransform();
	if (!CurrentTargetTransform.Equals(NewTransform))
	{
		CurrentTargetTransform = NewTransform;
		bRelativeTransformDirty = true;
	}
}


void UGPUSDFCutter::CalculateToolAABBInTargetSpace(const FTransform& ToolTransform, FIntVector& OutVoxelMin,
                                                   FIntVector& OutVoxelMax)
{
	if (!CutToolActor || !TargetMeshActor)
	{
		return;
	}

	// Target 这里表示被切削对象的局部坐标系
	FTransform TargetToWorld = TargetMeshActor->GetActorTransform();
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
	if (!bGPUResourcesInitialized)
		return;

	// 计算当前工具影响的体素区域
	FIntVector UpdateMin, UpdateMax;
	CalculateToolAABBInTargetSpace(CurrentToolTransform, UpdateMin, UpdateMax);

	// 验证区域有效性
	if (UpdateMin.X > UpdateMax.X || UpdateMin.Y > UpdateMax.Y || UpdateMin.Z > UpdateMax.Z)
	{
		UE_LOG(LogTemp, Warning, TEXT("Invalid update region, skipping"));
		return;
	}
	
	// 捕获必要的参数
	FTransform TargetSpaceToToolSpaceTransform = CurrentTargetTransform * CurrentToolTransform.Inverse();
	FBox CapturedTargetBounds = TargetLocalBounds;
	FBox CapturedToolBounds = ToolLocalBounds;
	FIntVector CapturedSDFDimensions = SDFDimensions;

	// 获取资源指针
	FTextureResource* ToolResource = ToolSDFTexture->GetResource();
	FTextureResource* RenderTargetResource = VolumeRT->GetResource();

	ENQUEUE_RENDER_COMMAND(GPUSDFCutter_LocalUpdate)(
		[this,UpdateMin, UpdateMax,
			CapturedTargetBounds, CapturedToolBounds,
			TargetSpaceToToolSpaceTransform,
			CapturedSDFDimensions,
			ToolResource, RenderTargetResource]
	(FRHICommandListImmediate& RHICmdList)
		{
		
			// 在渲染线程获取 RHI
			FRHITexture* ToolRHI = ToolResource ? ToolResource->GetTextureRHI() : nullptr;
			FRHITexture* VolumeRHI = RenderTargetResource ? RenderTargetResource->GetTextureRHI() : nullptr;

			if (!ToolRHI || !VolumeRHI) return;

			FRDGBuilder GraphBuilder(RHICmdList);

			// 注册纹理资源
			FRDGTextureRef ToolTexture = RegisterExternalTexture(
				GraphBuilder, ToolRHI, TEXT("ToolSDF"));
			FRDGTextureRef VolumeRTTexture = RegisterExternalTexture(
				GraphBuilder, VolumeRHI, TEXT("VolumeRT"));

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

			// 计算线程组数量（只处理更新区域）
			FIntVector RegionSize = UpdateMax - UpdateMin + FIntVector(1, 1, 1);
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(RegionSize, FIntVector(4, 4, 4));

			TShaderMapRef<FUpdateSDFCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("LocalSDFUpdate"),
				ComputeShader,
				PassParams,
				GroupCount
			);

			GraphBuilder.Execute();
		});
}


void UGPUSDFCutter::CalculateToolDimensions()
{
	// 从CutToolActor的网格组件获取边界
	UStaticMeshComponent* ToolMeshComponent = CutToolActor->GetStaticMeshComponent();

	ToolLocalBounds = ToolMeshComponent->CalcLocalBounds().GetBox();
	// 计算工具尺寸
	ToolOriginalSize = ToolLocalBounds.GetSize();
}

