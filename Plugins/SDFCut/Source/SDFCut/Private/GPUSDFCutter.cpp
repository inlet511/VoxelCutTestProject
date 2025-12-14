// Fill out your copyright notice in the Description page of Project Settings.


#include "GPUSDFCutter.h"

#include "DynamicMeshEditor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Components/DynamicMeshComponent.h"
#include "Engine/VolumeTexture.h"
#include "RenderGraphUtils.h"
#include "UpdateSDFShader.h"
#include "DynamicMesh/MeshNormals.h"

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
		InitResources(
			OriginalSDFTexture,
			ToolSDFTexture,
			TargetMeshActor->GetDynamicMeshComponent()->GetLocalBounds().GetBox(),
			VoxelSize);
	}
}


// Called every frame
void UGPUSDFCutter::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	if (!bGPUResourcesInitialized || !TargetMeshActor || !CutToolActor)
		return;

	UpdateToolTransform(CutToolActor->GetActorTransform());	

	// 只有工具位置变化,并且数据已经处理完成才发送新请求
	if (bToolTransformDirty)
	{
		UE_LOG(LogTemp, Warning, TEXT("Transform Updated, Dispatching Work"));

		bToolTransformDirty = false;
	}
}


void UGPUSDFCutter::InitResources(
	UVolumeTexture* InOriginalSDF,
	UVolumeTexture* InToolSDF,
	const FBox& TargetLocalBox,
	float InVoxelSize)
{
	OriginalSDFTexture = InOriginalSDF;
	ToolSDFTexture = InToolSDF;
	VoxelSize = InVoxelSize;

	// 计算SDF尺寸
	SDFDimensions.X = FMath::RoundToInt(TargetLocalBox.GetSize().X / VoxelSize);
	SDFDimensions.Y = FMath::RoundToInt(TargetLocalBox.GetSize().Y / VoxelSize);
	SDFDimensions.Z = FMath::RoundToInt(TargetLocalBox.GetSize().Z / VoxelSize);

	// 设置原始边界
	TargetLocalBounds = TargetLocalBox;

	CalculateToolDimensions();
	CurrentToolTransform = GetToolWorldTransform();

	// 初始化GPU资源
	if (bGPUResourcesInitialized || !VolumeRT)
	{
		UE_LOG(LogTemp, Error, TEXT("GPUResources Not Initlialized , or VolumeRT is null!"));
		return;
	}

	// 1. 存储外部纹理的RHI引用
	OriginalSDFRHIRef = OriginalSDFTexture->GetResource()->GetTextureRHI();
	ToolSDFRHIRef = ToolSDFTexture->GetResource()->GetTextureRHI();
	VolumeRTRHIRef = VolumeRT->GetResource()->GetTextureRHI();

	SDFDimensions = FIntVector(
		OriginalSDFTexture->GetSizeX(),
		OriginalSDFTexture->GetSizeY(),
		OriginalSDFTexture->GetSizeZ()
	);

	ENQUEUE_RENDER_COMMAND(InitGPUSDFCutterResources)([this, OriginalTextureRHI = OriginalSDFRHIRef](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		// 注册外部资源
		FRDGTextureRef RDGOriginalSDFTexture = RegisterExternalTexture(GraphBuilder, OriginalTextureRHI, TEXT("VolumeTexture"));
		FRDGTextureRef DestTexture = RegisterExternalTexture(GraphBuilder, VolumeRTRHIRef, TEXT("DestVolumeRT"));


		// 复制整个纹理
		AddCopyTexturePass(GraphBuilder, RDGOriginalSDFTexture, DestTexture);

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


void UGPUSDFCutter::CalculateToolAABBInTargetSpace(const FTransform& ToolTransform, FIntVector& OutVoxelMin, FIntVector& OutVoxelMax)
{
	if (!CutToolActor || !TargetMeshActor)
	{
		return;
	}

	// 切削对象的LocalBounds
	TargetLocalBounds = TargetMeshActor->GetDynamicMeshComponent()->GetLocalBounds().GetBox();

	// 工具本地AABB
	FBox ToolLocalBox = CutToolActor->GetDynamicMeshComponent()->GetLocalBounds().GetBox();


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


void UGPUSDFCutter::DispatchLocalUpdate(TFunction<void(const TArray<FVector>& Vertices, const TArray<FIntVector>& Triangles)> AsyncCallback)
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

	// 如果有上次更新区域，合并以覆盖"擦除痕迹"
	if (bHasLastUpdateRegion)
	{
		UpdateMin = UpdateMin.ComponentMin(LastUpdateMin);
		UpdateMax = UpdateMax.ComponentMax(LastUpdateMax);
	}

	// 记录本次更新区域
	LastUpdateMin = UpdateMin;
	LastUpdateMax = UpdateMax;
	bHasLastUpdateRegion = true;

	// 捕获必要的参数
	FTransform CapturedToolTransform = CurrentToolTransform;
	FTransform CapturedObjectTransform = CurrentObjectTransform;
	FBox CapturedTargetBounds = TargetLocalBounds;
	FBox CapturedToolBounds = ToolLocalBounds;
	FIntVector CapturedSDFDimensions = SDFDimensions;
	float CapturedVoxelSize = VoxelSize;


	ENQUEUE_RENDER_COMMAND(GPUSDFCutter_LocalUpdate)(
		[this,UpdateMin, UpdateMax,
		CapturedToolTransform, CapturedObjectTransform,
		CapturedTargetBounds, CapturedToolBounds,
		CapturedSDFDimensions, CapturedVoxelSize]
		(FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		// 注册纹理资源
		FRDGTextureRef OriginalTexture = RegisterExternalTexture(
			GraphBuilder, OriginalSDFRHIRef, TEXT("OriginalSDF"));
		FRDGTextureRef ToolTexture = RegisterExternalTexture(
			GraphBuilder, ToolSDFRHIRef, TEXT("ToolSDF"));
		FRDGTextureRef VolumeRTTexture = RegisterExternalTexture(
			GraphBuilder, VolumeRTRHIRef, TEXT("VolumeRT"));


		auto* CutUBParams = GraphBuilder.AllocParameters<FCutUB>();
		CutUBParams->TargetLocalBoundsMin = FVector3f(CapturedTargetBounds.Min);
		CutUBParams->TargetLocalBoundsMax = FVector3f(CapturedTargetBounds.Max);
		CutUBParams->ToolLocalBoundsMin = FVector3f(CapturedToolBounds.Min);
		CutUBParams->ToolLocalBoundsMax = FVector3f(CapturedToolBounds.Max);

		CutUBParams->TargetToToolTransform = FMatrix44f(CurrentObjectTransform.ToMatrixWithScale());
		CutUBParams->ToolToTargetTransform = FMatrix44f(CurrentObjectTransform.Inverse().ToMatrixWithScale());
		CutUBParams->SDFDimensions = CapturedSDFDimensions;
		CutUBParams->VoxelSize = CapturedVoxelSize;
		CutUBParams->IsoValue = 0.0f;
		CutUBParams->UpdateRegionMin = UpdateMin;
		CutUBParams->UpdateRegionMax = UpdateMax;


		auto* PassParams = GraphBuilder.AllocParameters<FUpdateSDFCS::FParameters>();
		PassParams->Params = GraphBuilder.CreateUniformBuffer(CutUBParams);
		PassParams->OriginalSDF = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(OriginalTexture));
		PassParams->ToolSDF = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ToolTexture));
		PassParams->OutputSDF = GraphBuilder.CreateUAV(VolumeRTTexture);

		PassParams->OriginalSDFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
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
