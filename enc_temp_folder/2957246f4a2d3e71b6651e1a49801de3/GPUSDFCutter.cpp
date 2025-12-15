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
		InitResources(
			OriginalSDFTexture,
			ToolSDFTexture,
			TargetMeshActor->GetStaticMeshComponent()->Bounds.GetBox(),
			VoxelSize);
	}

	// 创建一个 Lambda 延迟执行
	// FTimerHandle UnusedHandle;
	// GetWorld()->GetTimerManager().SetTimer(UnusedHandle, [this]()
	// {
	// 	if (OriginalSDFTexture && ToolSDFTexture && TargetMeshActor)
	// 	{
	// 		UE_LOG(LogTemp, Warning, TEXT("Triggering InitResources NOW..."));
	// 		InitResources(
	// 			OriginalSDFTexture,
	// 			ToolSDFTexture,
	// 			TargetMeshActor->GetStaticMeshComponent()->Bounds.GetBox(),
	// 			VoxelSize);
	// 	}
	// }, 3.0f, false); // 延迟 3.0 秒
}


// Called every frame
void UGPUSDFCutter::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	if (!bGPUResourcesInitialized || !TargetMeshActor || !CutToolActor)
		return;

	UpdateToolTransform(CutToolActor->GetActorTransform());
	
	InitResources(
				OriginalSDFTexture,
				ToolSDFTexture,
				TargetMeshActor->GetStaticMeshComponent()->Bounds.GetBox(),
				VoxelSize);
	// 只有工具位置变化,并且数据已经处理完成才发送新请求
	if (bToolTransformDirty)
	{
		UE_LOG(LogTemp, Warning, TEXT("Transform Updated, Dispatching Work"));

		bToolTransformDirty = false;
	}
}

/*
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

	// 创建VolumeRT
	VolumeRT = NewObject<UTextureRenderTargetVolume>(GetTransientPackage(),NAME_None);
	VolumeRT->OverrideFormat = OriginalSDFTexture->GetPixelFormat();	
	VolumeRT->SizeX = InOriginalSDF->GetSizeX();
	VolumeRT->SizeY = InOriginalSDF->GetSizeY();
	VolumeRT->SizeZ = InOriginalSDF->GetSizeZ();
	VolumeRT->UpdateResource();

	// 创建SDF渲染材质实例
	SDFMaterialInstanceDynamic = UMaterialInstanceDynamic::Create(SDFMaterialInstance, this);
	if (VolumeRT)
	{
		SDFMaterialInstanceDynamic->SetTextureParameterValue(FName("VolumeTexture"), VolumeRT);
		SDFMaterialInstanceDynamic->SetVectorParameterValue(FName("AmbientColor"), FVector(1, 0, 0));
	}
	// 分配材质实例给目标网格
	TargetMeshActor->GetStaticMeshComponent()->SetMaterial(0, SDFMaterialInstanceDynamic);



	// 初始化GPU资源
	if (bGPUResourcesInitialized || !VolumeRT)
	{
		UE_LOG(LogTemp, Error, TEXT("GPUResources Not Initlialized , or VolumeRT is null!"));
		return;
	}

	// 1. 存储外部纹理的RHI引用
	OriginalSDFRHIRef = OriginalSDFTexture->GetResource()->GetTextureRHI();
	
	if (!OriginalSDFRHIRef.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to get OriginalSDF RHI"));
		return;
	}
	
	ToolSDFRHIRef = ToolSDFTexture->GetResource()->GetTextureRHI();

	if (!ToolSDFRHIRef.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to get ToolSDF RHI"));
		return;
	}
	
	//VolumeRTRHIRef = VolumeRT->GetResource()->GetTextureRHI();

	// 获取资源对象的指针（CPU端的FTextureResource指针是立即有效的）
	FTextureResource* DestVolumeResource = VolumeRT->GetResource();

	SDFDimensions = FIntVector(
		OriginalSDFTexture->GetSizeX(),
		OriginalSDFTexture->GetSizeY(),
		OriginalSDFTexture->GetSizeZ()
	);

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
*/

void UGPUSDFCutter::InitResources(
	UVolumeTexture* InOriginalSDF,
	UVolumeTexture* InToolSDF,
	const FBox& TargetLocalBox,
	float InVoxelSize)
{
	// 1. 基础资源设置
	OriginalSDFTexture = InOriginalSDF;
	ToolSDFTexture = InToolSDF;
	VoxelSize = InVoxelSize;

	// 计算尺寸
	SDFDimensions.X = FMath::RoundToInt(TargetLocalBox.GetSize().X / VoxelSize);
	SDFDimensions.Y = FMath::RoundToInt(TargetLocalBox.GetSize().Y / VoxelSize);
	SDFDimensions.Z = FMath::RoundToInt(TargetLocalBox.GetSize().Z / VoxelSize);
	TargetLocalBounds = TargetLocalBox;

	CalculateToolDimensions();
	CurrentToolTransform = GetToolWorldTransform();

	// 2. 创建 VolumeRT
	// 确保使用 this 作为 Outer，防止被意外 GC
	if (!VolumeRT->IsValidLowLevel())
	{
		//VolumeRT = NewObject<UTextureRenderTargetVolume>(this, NAME_None);
		int32 TextureSizeX = OriginalSDFTexture->GetSizeX();
		int32 TextureSizeY = OriginalSDFTexture->GetSizeY();
		int32 TextureSizeZ = OriginalSDFTexture->GetSizeZ();
		VolumeRT = UKismetRenderingLibrary::CreateRenderTargetVolume(this, TextureSizeX, TextureSizeY, TextureSizeZ, RTF_R32f);

		// 关键：如果你后续要用 Compute Shader 写它，必须开启 UAV
		VolumeRT->bCanCreateUAV = true;
	}


	// 3. 材质设置 (立即绑定)
	SDFMaterialInstanceDynamic = UMaterialInstanceDynamic::Create(SDFMaterialInstance, this);
	if (VolumeRT)
	{
		SDFMaterialInstanceDynamic->SetTextureParameterValue(FName("VolumeTexture"), VolumeRT);
	}
	TargetMeshActor->GetStaticMeshComponent()->SetMaterial(0, SDFMaterialInstanceDynamic);

	// 4. 执行 GPU 复制 (Direct RHI)
	FTextureResource* DestResource = VolumeRT->GetResource();
	FTextureResource* SrcResource = OriginalSDFTexture->GetResource();

	ENQUEUE_RENDER_COMMAND(InitGPUSDFCutterResources)(
		[DestResource, SrcResource](FRHICommandListImmediate& RHICmdList)
		{

			SCOPED_DRAW_EVENTF(RHICmdList, TextureCopyEvent, TEXT("SDFCopyTexture!"));
			// 安全检查
			if (!SrcResource || !SrcResource->GetTextureRHI() || !DestResource || !DestResource->GetTextureRHI())
			{
				// 如果此时资源还没准备好（比如还在流送），这可能会失败
				// 生产环境中可能需要重试机制，但 BeginPlay 通常没问题
				return;
			}

			FRHITexture* SrcRHI = SrcResource->GetTextureRHI();
			FRHITexture* DstRHI = DestResource->GetTextureRHI();

			// 状态转换：准备复制
			// Src -> CopySrc
			// Dst -> CopyDest
			RHICmdList.Transition(FRHITransitionInfo(SrcRHI, ERHIAccess::Unknown, ERHIAccess::CopySrc));
			RHICmdList.Transition(FRHITransitionInfo(DstRHI, ERHIAccess::Unknown, ERHIAccess::CopyDest));

			// 执行直接复制
			FRHICopyTextureInfo CopyInfo;
			CopyInfo.Size = FIntVector(SrcRHI->GetSizeX(), SrcRHI->GetSizeY(), SrcRHI->GetSizeZ());
			RHICmdList.CopyTexture(SrcRHI, DstRHI, CopyInfo);

			// 状态转换：准备渲染
			// Dst -> SRV (Shader Resource View) 供 Pixel Shader 读取
			// Src -> SRV (恢复原状)
			RHICmdList.Transition(FRHITransitionInfo(DstRHI, ERHIAccess::CopyDest, ERHIAccess::SRVGraphics));
			RHICmdList.Transition(FRHITransitionInfo(SrcRHI, ERHIAccess::CopySrc, ERHIAccess::SRVGraphics));
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

	// 切削对象的LocalBounds
	TargetLocalBounds = TargetMeshActor->GetStaticMeshComponent()->Bounds.GetBox();

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


void UGPUSDFCutter::DispatchLocalUpdate(
	TFunction<void(const TArray<FVector>& Vertices, const TArray<FIntVector>& Triangles)> AsyncCallback)
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


	// 获取资源指针
	FTextureResource* ToolResource = ToolSDFTexture->GetResource();
	FTextureResource* OriginalResource = OriginalSDFTexture->GetResource();
	FTextureResource* VolumeResource = VolumeRT->GetResource();

	ENQUEUE_RENDER_COMMAND(GPUSDFCutter_LocalUpdate)(
		[this,UpdateMin, UpdateMax,
			CapturedToolTransform, CapturedObjectTransform,
			CapturedTargetBounds, CapturedToolBounds,
			CapturedSDFDimensions, CapturedVoxelSize,
			ToolResource, OriginalResource, VolumeResource]
	(FRHICommandListImmediate& RHICmdList)
		{
			// 在渲染线程获取 RHI
			FRHITexture* ToolRHI = ToolResource ? ToolResource->GetTextureRHI() : nullptr;
			FRHITexture* OriginalRHI = OriginalResource ? OriginalResource->GetTextureRHI() : nullptr;
			FRHITexture* VolumeRHI = VolumeResource ? VolumeResource->GetTextureRHI() : nullptr;

			if (!ToolRHI || !OriginalRHI || !VolumeRHI) return;

			FRDGBuilder GraphBuilder(RHICmdList);

			// 注册纹理资源
			FRDGTextureRef OriginalTexture = RegisterExternalTexture(
				GraphBuilder, OriginalRHI, TEXT("OriginalSDF"));
			FRDGTextureRef ToolTexture = RegisterExternalTexture(
				GraphBuilder, ToolRHI, TEXT("ToolSDF"));
			FRDGTextureRef VolumeRTTexture = RegisterExternalTexture(
				GraphBuilder, VolumeRHI, TEXT("VolumeRT"));


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
