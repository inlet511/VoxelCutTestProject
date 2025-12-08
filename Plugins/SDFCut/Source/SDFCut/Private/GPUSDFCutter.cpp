// Fill out your copyright notice in the Description page of Project Settings.


#include "GPUSDFCutter.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Components/DynamicMeshComponent.h"
#include "Engine/VolumeTexture.h"
#include "RenderGraphUtils.h"
#include "SDFShaderParameters.h"


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
		InitSDFTextures(
			OriginalSDFTexture,
			ToolSDFTexture,
			TargetMeshActor->GetDynamicMeshComponent()->GetLocalBounds().GetBox(),
			VoxelSize);
	}

}


// Called every frame
void UGPUSDFCutter::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	if (!bGPUResourcesInitialized || !TargetMeshActor)
		return;

	// 只有工具位置变化时才更新
	if (bToolTransformDirty)
	{
		DispatchLocalUpdate();
		bToolTransformDirty = false;
	}

	// 更新网格（使用双缓冲的上一帧数据）
	if (bHasPendingMeshData)
	{
		int32 ReadBufferIndex = (CurrentMeshBuffer + 1) % 2;
		UpdateDynamicMesh(MeshVertices[ReadBufferIndex], MeshTriangles[ReadBufferIndex]);
		bHasPendingMeshData = false;
	}
}


void UGPUSDFCutter::InitSDFTextures(
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
	LastToolTransform = CurrentToolTransform;

	// 初始化GPU资源
	InitGPUResources();
}

void UGPUSDFCutter::UpdateToolTransform(const FTransform& InToolTransform)
{
	LastToolTransform = CurrentToolTransform;
	CurrentToolTransform = InToolTransform;
	bToolTransformDirty = true;
}

void UGPUSDFCutter::UpdateObjectTransform(const FTransform& InObjectTransform)
{
	CurrentObjectTransform = InObjectTransform;
	bToolTransformDirty = true; // 物体移动也需要更新
}

void UGPUSDFCutter::InitGPUResources()
 {
    if (bGPUResourcesInitialized)
     	return;
 
    ENQUEUE_RENDER_COMMAND(InitGPUSDFCutterResources)([this](FRHICommandListImmediate& RHICmdList)
    {
    	
     	int32 SourceSizeX = OriginalSDFTexture->GetSizeX();
     	int32 SourceSizeY = OriginalSDFTexture->GetSizeY();
     	int32 SourceSizeZ = OriginalSDFTexture->GetSizeZ();
 
     	// 1. 存储外部纹理的RHI引用（不要ConvertToExternalTexture）
     	OriginalSDFRHI = OriginalSDFTexture->GetResource()->GetTexture3DRHI();
     	ToolSDFRHI = ToolSDFTexture->GetResource()->GetTextureRHI();
 
     	FRDGBuilder GraphBuilder(RHICmdList);
 
     	// 2. 注册外部纹理到Render Graph
     	FRDGTextureRef OriginalTextureRef = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(OriginalSDFRHI,TEXT("OrigianlTexture")));
     	FRDGTextureRef ToolTextureRef = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(ToolSDFRHI,TEXT("ToolSDFTexture")));
 
     	// 3. 【关键修改】创建动态SDF纹理 - 移除不允许的标志
     	// 移除了 TexCreate_RenderTargetable 和 TexCreate_DepthStencilTargetable 等标志
     	// 仅保留UAV（用于写入）和ShaderResource（用于读取）标志
     	FRDGTextureDesc DynamicSDFDesc = FRDGTextureDesc::Create3D(
     		SDFDimensions,
     		PF_R32_FLOAT,
     		FClearValueBinding::None,
     		TexCreate_UAV | TexCreate_ShaderResource // 只保留必要的标志

	);
		FRDGTextureRef DynamicSDFTextureRef = GraphBuilder.CreateTexture(DynamicSDFDesc, TEXT("DynamicSDF"));

    	// 5. 初始化动态SDF为原始SDF值
    	AddCopyTexturePass(GraphBuilder, OriginalTextureRef, DynamicSDFTextureRef);
    	
		DynamicSDFTexturePooled = GraphBuilder.ConvertToExternalTexture(DynamicSDFTextureRef);
		GraphBuilder.Execute();
	

		bGPUResourcesInitialized = true;
    	UE_LOG(LogTemp, Log, TEXT("GPU SDF Cutter Local-Update Mode Initialized"));
	});	
}

void UGPUSDFCutter::CalculateToolAABBInTargetSpace(const FTransform& ToolTransform, FIntVector& OutVoxelMin, FIntVector& OutVoxelMax)
{
	if (!CutToolActor||!TargetMeshActor)
	{
		return;
	}

	// 切削对象的LocalBounds
	FBox TargetLocalBounds = TargetMeshActor->GetDynamicMeshComponent()->GetLocalBounds().GetBox();
	
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


void UGPUSDFCutter::DispatchLocalUpdate()
{
    ENQUEUE_RENDER_COMMAND(GPUSDFCutter_LocalUpdate)([this](FRHICommandListImmediate& RHICmdList)
    {
        FRDGBuilder GraphBuilder(RHICmdList);

        // 计算更新区域
        FIntVector UpdateMin, UpdateMax;
        CalculateToolAABBInTargetSpace(CurrentToolTransform, UpdateMin, UpdateMax);
        
        // 如果区域无效，跳过
        if (UpdateMin.X > UpdateMax.X || UpdateMin.Y > UpdateMax.Y || UpdateMin.Z > UpdateMax.Z)
            return;

        // 注册纹理
        FRDGTextureRef OriginalTexture = GraphBuilder.RegisterExternalTexture(
            CreateRenderTarget(OriginalSDFRHI, TEXT("OriginalSDFTexture")));
        FRDGTextureRef ToolTexture = GraphBuilder.RegisterExternalTexture(
            CreateRenderTarget(ToolSDFRHI, TEXT("ToolSDFTexture")));
        FRDGTextureRef DynamicSDFTexture = GraphBuilder.RegisterExternalTexture(DynamicSDFTexturePooled, TEXT("DynamicSDFTexture"));

        // 1. 局部SDF更新
        {
            FLocalUpdateParams LocalParams;
        	LocalParams.ObjectLocalBoundsMin = FVector3f(TargetLocalBounds.Min);
			LocalParams.ObjectLocalBoundsMax = FVector3f(TargetLocalBounds.Max);
			LocalParams.ToolLocalBoundsMin = FVector3f(ToolLocalBounds.Min);
			LocalParams.ToolLocalBoundsMax = FVector3f(ToolLocalBounds.Max);
			LocalParams.ObjectToToolTransform = FMatrix44f(ObjectToTool.ToMatrixWithScale());
			LocalParams.ToolToObjectTransform = FMatrix44f(ObjectToTool.Inverse().ToMatrixWithScale());
            LocalParams.SDFDimensions = SDFDimensions;
            LocalParams.VoxelSize = VoxelSize;
            LocalParams.IsoValue = 0.0f;
            LocalParams.UpdateRegionMin = UpdateMin;
            LocalParams.UpdateRegionMax = UpdateMax;

            auto* PassParams = GraphBuilder.AllocParameters<FLocalSDFUpdateCS::FParameters>();
            PassParams->Params = LocalParams;
            PassParams->OriginalSDF = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(OriginalTexture));
            PassParams->ToolSDF = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ToolTexture));
            PassParams->DynamicSDF = GraphBuilder.CreateUAV(DynamicSDFTexture);
            PassParams->OriginalSDFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
            PassParams->ToolSDFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

            // 只调度更新区域
            FIntVector RegionSize = UpdateMax - UpdateMin + FIntVector(1);
            FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(RegionSize, FIntVector(4, 4, 4));

            TShaderMapRef<FLocalSDFUpdateCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("LocalSDFUpdate"),
                ComputeShader,
                PassParams,
                GroupCount
            );
        }

        // 2. 局部Marching Cubes（只生成变化区域的网格）
        {
            // 网格生成区域比更新区域大1个体素（因为Marching Cubes需要相邻体素）
            FIntVector GenerateMin = FIntVector::Max(UpdateMin - FIntVector(1), FIntVector(0));
            FIntVector GenerateMax = FIntVector::Min(UpdateMax + FIntVector(1), SDFDimensions - FIntVector(2));

            // 创建输出缓冲区
            const int32 MaxVertices = RegionSize.X * RegionSize.Y * RegionSize.Z * 5; // 保守估计
            const int32 MaxTriangles = MaxVertices;

            FRDGBufferRef VertexBuffer = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), MaxVertices), TEXT("Vertices"));
            FRDGBufferRef TriangleBuffer = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateStructuredDesc(sizeof(FIntVector), MaxTriangles), TEXT("Triangles"));
            FRDGBufferRef VertexCounter = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateBufferDesc(sizeof(int32), 1), TEXT("VertexCounter"));
            FRDGBufferRef TriangleCounter = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateBufferDesc(sizeof(int32), 1), TEXT("TriangleCounter"));

            // 清空计数器
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(VertexCounter, PF_R32_SINT), 0);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(TriangleCounter, PF_R32_SINT), 0);

            FMCParams MCParams;
            MCParams.SDFBoundsMin = FVector3f(TargetLocalBounds.Min);
            MCParams.SDFBoundsMax = FVector3f(TargetLocalBounds.Max);
            MCParams.SDFDimensions = SDFDimensions;
            MCParams.IsoValue = 0.0f;
            MCParams.GenerateRegionMin = GenerateMin;
            MCParams.GenerateRegionMax = GenerateMax;

            auto* PassParams = GraphBuilder.AllocParameters<FLocalMarchingCubesCS::FParameters>();
            PassParams->Params = MCParams;
            PassParams->DynamicSDF = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DynamicSDFTexture));
            PassParams->OutVertices = GraphBuilder.CreateUAV(VertexBuffer);
            PassParams->OutTriangles = GraphBuilder.CreateUAV(TriangleBuffer);
            PassParams->VertexCounter = GraphBuilder.CreateUAV(VertexCounter);
            PassParams->TriangleCounter = GraphBuilder.CreateUAV(TriangleCounter);

            FIntVector MCGroupCount = FComputeShaderUtils::GetGroupCount(
                GenerateMax - GenerateMin + FIntVector(1), FIntVector(4, 4, 4));

            TShaderMapRef<FLocalMarchingCubesCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("LocalMarchingCubes"),
                ComputeShader,
                PassParams,
                MCGroupCount
            );

            // 设置回读
            TRefCountPtr<FRDGPooledBuffer> PooledVertexBuffer, PooledTriangleBuffer;
            GraphBuilder.QueueBufferExtraction(VertexBuffer, &PooledVertexBuffer);
            GraphBuilder.QueueBufferExtraction(TriangleBuffer, &PooledTriangleBuffer);
            GraphBuilder.QueueBufferExtraction(VertexCounter, &PooledVertexBuffer);
            GraphBuilder.QueueBufferExtraction(TriangleCounter, &PooledTriangleBuffer);

            // 异步回读
            RenderGraphCompletionEvent = GraphBuilder.Execute();
            ENQUEUE_RENDER_COMMAND(ReadbackMeshData)([this, PooledVertexBuffer, PooledTriangleBuffer](FRHICommandListImmediate& RHICmdList)
            {
                if (RenderGraphCompletionEvent.IsValid())
                    RenderGraphCompletionEvent->Wait();

                // 这里实现数据回读
                // ReadbackAndUpdateMesh(PooledVertexBuffer, PooledTriangleBuffer, ...);
            });
        }
    });
}

/*
void UGPUSDFCutter::ReadbackMeshData(TRDGBufferRef<FVector> VertexBuffer, TRDGBufferRef<FIntVector> TriangleBuffer, int32 NumVertices, int32 NumTriangles)
{
	
	FRHICommandListImmediate& RHICmdList = GRHICommandList.GetImmediateCommandList();

	// 切换双缓冲（当前缓冲区用于存储新数据）
	int32 WriteBufferIndex = CurrentBufferIndex;
	CurrentBufferIndex = (CurrentBufferIndex + 1) % 2;

	// 回读顶点数据
	TArray<FVector>& DestVertices = ReadbackVertices[WriteBufferIndex];
	DestVertices.SetNumUninitialized(NumVertices);
	RHICmdList.ReadSurfaceData(
		VertexBuffer->Buffer,
		FIntVector(0, 0, 0),
		FIntVector(NumVertices, 1, 1),
		PF_R32G32B32A32F,
		DestVertices.GetData()
	);

	// 回读三角形数据
	TArray<FIntVector>& DestTriangles = ReadbackTriangles[WriteBufferIndex];
	DestTriangles.SetNumUninitialized(NumTriangles);
	RHICmdList.ReadSurfaceData(
		TriangleBuffer->Buffer,
		FIntVector(0, 0, 0),
		FIntVector(NumTriangles, 1, 1),
		PF_R32G32B32A32_SINT,
		DestTriangles.GetData()
	);

	// 确保数据回读完成
	RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);

	bHasValidMeshData = true;
	
}
*/

void UGPUSDFCutter::UpdateDynamicMesh(const TArray<FVector>& Vertices, const TArray<FIntVector>& Triangles)
{
	if (!TargetMeshActor || Vertices.Num() == 0 || Triangles.Num() == 0)
		return;

	UDynamicMeshComponent* DynamicMeshComp = TargetMeshActor->GetDynamicMeshComponent();
	if (!DynamicMeshComp) return;

	// 使用DynamicMeshComponent的API更新网格
	// 这里需要根据您的DynamicMesh组件API进行调整
	// ...

	if (OnMeshGenerated.IsBound())
	{
		OnMeshGenerated.Broadcast(DynamicMeshComp);
	}
}

void UGPUSDFCutter::CalculateToolDimensions()
{
	// 从CutToolActor的网格组件获取边界
	UDynamicMeshComponent* ToolMeshComponent = CutToolActor->GetDynamicMeshComponent();

	ToolLocalBounds  = ToolMeshComponent->GetLocalBounds().GetBox();
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

