// Fill out your copyright notice in the Description page of Project Settings.


#include "GPUSDFCutter.h"

#include "DynamicMeshEditor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Components/DynamicMeshComponent.h"
#include "Engine/VolumeTexture.h"
#include "RenderGraphUtils.h"
#include "UpdateSDFShader.h"
#include "DynamicMesh/MeshNormals.h"
#include "Engine/TextureRenderTargetVolume.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet/KismetRenderingLibrary.h"


UE_DISABLE_OPTIMIZATION

IMPLEMENT_UNIFORM_BUFFER_STRUCT(FCutUB, "CutUB");
IMPLEMENT_GLOBAL_SHADER(FUpdateSDFCS, "/SDF/Shaders/DynamicSDFUpdateCS.usf", "LocalSDFUpdateKernel", SF_Compute);

// Sets default values for this component's properties
UGPUSDFCutter::UGPUSDFCutter()
{
	// Set this component to be initialized when the game starts, and to be ticked every frame.  You can turn these features
	// off to improve performance if you don't need them.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	// ...
}


// Called when the game starts
void UGPUSDFCutter::BeginPlay()
{
	Super::BeginPlay();

	if (OriginalSDFTexture && ToolSDFTexture && TargetMeshActor)
	{
		InitResources();
	}

}


// Called every frame
void UGPUSDFCutter::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	if (!bGPUResourcesInitialized || !TargetMeshActor || !CutToolActor)
		return;

	UpdateToolTransform(CutToolActor->GetActorTransform());

	// 只有工具位置变化才发送新请求
	if (bToolTransformDirty)
	{
		DispatchLocalUpdate();

		bToolTransformDirty = false;
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

	CurrentToolTransform = GetToolWorldTransform();

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


void UGPUSDFCutter::UpdateToolTransform(const FTransform& InToolTransform)
{
	if (!CurrentToolTransform.Equals(InToolTransform))
	{
		CurrentToolTransform = InToolTransform;
		bToolTransformDirty = true;
	}
}

void UGPUSDFCutter::UpdateObjectTransform(const FTransform& InObjectTransform)
{
	CurrentObjectTransform = InObjectTransform;
	bToolTransformDirty = true; // 物体移动也需要更新
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
	FTransform ToolToTarget = WorldToTarget * ToolTransform;

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
	OutVoxelMax = OutVoxelMax.ComponentMin(SDFDimensions - FIntVector(1, 1, 1)); // 上限不能超过尺寸
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
	FTransform TargetSpaceToToolSpaceTransform = CurrentObjectTransform.Inverse() * CurrentToolTransform;
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
	UDynamicMeshComponent* ToolMeshComponent = CutToolActor->GetDynamicMeshComponent();

	ToolLocalBounds = ToolMeshComponent->GetLocalBounds().GetBox();
	// 计算工具尺寸
	ToolOriginalSize = ToolLocalBounds.GetSize();
}

FTransform UGPUSDFCutter::GetToolWorldTransform() const
{
	if (CutToolActor)
	{
		return CutToolActor->GetActorTransform();
	}
	return CurrentToolTransform;
}

UE_ENABLE_OPTIMIZATION
