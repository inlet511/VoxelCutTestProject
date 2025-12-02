// Fill out your copyright notice in the Description page of Project Settings.


#include "GPUSDFCutter.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Components/DynamicMeshComponent.h"
#include "Engine/VolumeTexture.h"


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

	if (OriginalSDFTexture && ToolSDFTexture)
	{
		InitGPUResources();
	}
	
}


// Called every frame
void UGPUSDFCutter::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bGPUResourcesInitialized || !TargetMeshComponent)
		return;

	// 只有Transform更新时才调度GPU计算
	if (bToolTransformUpdated)
	{
		DispatchRenderGraph();
		bToolTransformUpdated = false;
	}

	// 每帧更新网格（使用双缓冲的上一帧数据）
	if (bHasValidMeshData)
	{
		int32 RenderBufferIndex = (CurrentBufferIndex + 1) % 2;
		UpdateDynamicMesh(ReadbackVertices[RenderBufferIndex], ReadbackTriangles[RenderBufferIndex]);
	}
}


void UGPUSDFCutter::InitSDFTextures(UVolumeTexture* InOriginalSDF, UVolumeTexture* InToolSDF, const UE::Geometry::FAxisAlignedBox3d& InOriginalBounds, float InCubeSize)
{
    check(InOriginalSDF && InToolSDF);
    OriginalSDFTexture = InOriginalSDF;
    ToolSDFTexture = InToolSDF;
    OriginalBounds = InOriginalBounds;
    CubeSize = InCubeSize;

    // 计算动态SDF分辨率（根据边界和体素尺寸）
    DynamicSDFDimensions.X = FMath::Max(32, FMath::RoundToInt((OriginalBounds.Max.X - OriginalBounds.Min.X) / CubeSize));
    DynamicSDFDimensions.Y = FMath::Max(32, FMath::RoundToInt((OriginalBounds.Max.Y - OriginalBounds.Min.Y) / CubeSize));
    DynamicSDFDimensions.Z = FMath::Max(32, FMath::RoundToInt((OriginalBounds.Max.Z - OriginalBounds.Min.Z) / CubeSize));

    if (IsComponentTickEnabled())
    {
        InitGPUResources();
    }
}

void UGPUSDFCutter::UpdateToolTransform(const FTransform& InToolTransform)
{
    CurrentToolTransform = InToolTransform;
    bToolTransformUpdated = true;
}

void UGPUSDFCutter::SetTargetMeshComponent(UDynamicMeshComponent* InTargetMeshComponent)
{
    TargetMeshComponent = InTargetMeshComponent;
    if (TargetMeshComponent)
    {
    	TargetMeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    	TargetMeshComponent->SetGenerateOverlapEvents(true);
    	TargetMeshComponent->SetDynamicMesh(MakeShared<UE::Geometry::FDynamicMesh3>());
    }
}

void UGPUSDFCutter::InitGPUResources()
{
    if (bGPUResourcesInitialized)
        return;

    FRHICommandListImmediate& RHICmdList = GRHICommandList.GetImmediateCommandList();

	// 1. 获取原始物体和工具SDF的RHI引用（确保纹理格式为PF_R32_FLOAT）
	OriginalSDFRHIRef = OriginalSDFTexture->GetResource()->GetTextureRHI();
	ToolSDFRHIRef = ToolSDFTexture->GetResource()->GetTextureRHI();
	check(OriginalSDFRHIRef.IsValid() && ToolSDFRHIRef.IsValid());
	check(OriginalSDFTexture->GetPixelFormat() == PF_R32_FLOAT && ToolSDFTexture->GetPixelFormat() == PF_R32_FLOAT);

	// 2. 创建动态SDF纹理（RWTexture3D，可写+可读）
	FTexture3DDesc DynamicSDFDesc;
	DynamicSDFDesc.SizeX = DynamicSDFDimensions.X;
	DynamicSDFDesc.SizeY = DynamicSDFDimensions.Y;
	DynamicSDFDesc.SizeZ = DynamicSDFDimensions.Z;
	DynamicSDFDesc.Format = PF_R32_FLOAT; // 单通道浮点，存储SDF值
	DynamicSDFDesc.Usage = TexUsage_GPUCompute | TexUsage_ShaderResource;
	DynamicSDFDesc.Flags = TexCreate_UnorderedAccess | TexCreate_CPUReadback;
	DynamicSDFRHIRef = RHICreateTexture3D(DynamicSDFDesc, TEXT("DynamicSDFTexture"), nullptr);
	check(DynamicSDFRHIRef.IsValid());

	// 3. 初始化动态SDF为原始SDF值（首次启动时拷贝）
	FComputeShaderUtils::CopyTexture(RHICmdList, OriginalSDFRHIRef, DynamicSDFRHIRef);

	bGPUResourcesInitialized = true;
	UE_LOG(LogTemp, Log, TEXT("GPU SDF Cutter Resources Initialized. Dynamic SDF Dimensions: %d x %d x %d"),
		DynamicSDFDimensions.X, DynamicSDFDimensions.Y, DynamicSDFDimensions.Z);
}

void UGPUSDFCutter::DispatchRenderGraph()
{
	if (!bGPUResourcesInitialized)
        return;

    // 1. 构建Render Graph（RDG核心）
    FRDGBuilder GraphBuilder(RHIVendor::GetDefaultFeatureLevel());

    // 2. 准备全局参数（仅传递Transform等轻量数据）
    FCutterGlobalParams GlobalParams;
    GlobalParams.IsoValue = 0.0f;
    GlobalParams.CubeSize = CubeSize;
    GlobalParams.ToolTransform = CurrentToolTransform.ToMatrixWithScale(); // 工具→原始物体空间变换
    GlobalParams.ToolInverseTransform = GlobalParams.ToolTransform.Inverse();
    GlobalParams.ToolAABB = ComputeToolAABBInOriginalSpace(CurrentToolTransform); // 优化更新范围

    // 3. 绑定SDF纹理参数
    FSDFTextureParams SDFParams;
    SDFParams.OriginalSDF = GraphBuilder.RegisterExternalTexture(OriginalSDFRHIRef, TEXT("OriginalSDF"));
    SDFParams.OriginalSDFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
    SDFParams.ToolSDF = GraphBuilder.RegisterExternalTexture(ToolSDFRHIRef, TEXT("ToolSDF"));
    SDFParams.ToolSDFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
    SDFParams.DynamicSDF = GraphBuilder.RegisterExternalTexture(DynamicSDFRHIRef, TEXT("DynamicSDF"));

    // 4. 创建DynamicSDF更新Shader的参数缓冲区
    auto DynamicSDFUpdateParams = GraphBuilder.AllocParameters<FDynamicSDFUpdateCS::Parameters>();
    DynamicSDFUpdateParams->GlobalParams = GlobalParams;
    DynamicSDFUpdateParams->SDFParams = SDFParams;
    DynamicSDFUpdateParams->DynamicSDFBounds = OriginalBounds;
    DynamicSDFUpdateParams->DynamicSDFDimensions = DynamicSDFDimensions;

    // 5. 调度DynamicSDF更新Compute Shader（3D线程组：适配体素网格）
    TShaderMapRef<FDynamicSDFUpdateCS> DynamicSDFUpdateShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    FIntVector DynamicSDFThreadGroupCount = FIntVector(
        FMath::DivideAndRoundUp(DynamicSDFDimensions.X, 8),
        FMath::DivideAndRoundUp(DynamicSDFDimensions.Y, 8),
        FMath::DivideAndRoundUp(DynamicSDFDimensions.Z, 8)
    );
    GraphBuilder.AddPass(
        RDG_EVENT_NAME("DynamicSDFUpdate"),
        DynamicSDFUpdateParams,
        ERDGPassFlags::Compute,
        [&](FRHICommandList& RHICmdList)
        {
            RHICmdList.SetComputeShader(DynamicSDFUpdateShader.GetComputeShader());
            DynamicSDFUpdateShader->SetParameters(RHICmdList, *DynamicSDFUpdateParams);
            RHICmdList.DispatchComputeShader(DynamicSDFThreadGroupCount.X, DynamicSDFThreadGroupCount.Y, DynamicSDFThreadGroupCount.Z);
            DynamicSDFUpdateShader->UnsetParameters(RHICmdList, *DynamicSDFUpdateParams);
        }
    );

    // 6. 创建Marching Cubes输出缓冲区（通过RDG创建TRDGBufferRef）
    const int32 MaxVertices = DynamicSDFDimensions.X * DynamicSDFDimensions.Y * DynamicSDFDimensions.Z * 12;
    const int32 MaxTriangles = DynamicSDFDimensions.X * DynamicSDFDimensions.Y * DynamicSDFDimensions.Z * 5;
    
    TRDGBufferRef<FVector> VertexBuffer = GraphBuilder.CreateStructuredBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector), MaxVertices),
        TEXT("MCVerticesBuffer")
    );
    TRDGBufferRef<FIntVector> TriangleBuffer = GraphBuilder.CreateStructuredBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(FIntVector), MaxTriangles),
        TEXT("MCTrianglesBuffer")
    );
    TRDGBufferRef<int32> VertexCounterBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(int32), 1),
        TEXT("VertexCounterBuffer"),
        ERDGResourceType::UAV
    );
    TRDGBufferRef<int32> TriangleCounterBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(int32), 1),
        TEXT("TriangleCounterBuffer"),
        ERDGResourceType::UAV
    );

    // 7. 清空计数器（每帧重置顶点/三角形索引）
    GraphBuilder.AddPass(
        RDG_EVENT_NAME("ClearMCounters"),
        ERDGPassFlags::Compute,
        [&](FRHICommandList& RHICmdList)
        {
            RHICmdList.ClearUAV(VertexCounterBuffer->UAV, 0);
            RHICmdList.ClearUAV(TriangleCounterBuffer->UAV, 0);
        }
    );

    // 8. 创建Marching Cubes Shader的参数缓冲区
    auto MCParams = GraphBuilder.AllocParameters<FMarchingCubesCS::Parameters>();
    MCParams->GlobalParams = GlobalParams;
    MCParams->DynamicSDFBounds = OriginalBounds;
    MCParams->DynamicSDFDimensions = DynamicSDFDimensions;
    MCParams->DynamicSDF = SDFParams.DynamicSDF;
    MCParams->MCOutputParams.OutVertices = VertexBuffer->UAV;
    MCParams->MCOutputParams.OutTriangles = TriangleBuffer->UAV;
    MCParams->MCOutputParams.VertexCounter = VertexCounterBuffer->UAV;
    MCParams->MCOutputParams.TriangleCounter = TriangleCounterBuffer->UAV;

    // 9. 调度Marching Cubes Compute Shader（3D线程组：适配体素网格）
    TShaderMapRef<FMarchingCubesCS> MCShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    FIntVector MCThreadGroupCount = FIntVector(
        FMath::DivideAndRoundUp(DynamicSDFDimensions.X - 1, 8), // 立方体数量=体素数量-1
        FMath::DivideAndRoundUp(DynamicSDFDimensions.Y - 1, 8),
        FMath::DivideAndRoundUp(DynamicSDFDimensions.Z - 1, 8)
    );
    GraphBuilder.AddPass(
        RDG_EVENT_NAME("MarchingCubesGenerate"),
        MCParams,
        ERDGPassFlags::Compute,
        [&](FRHICommandList& RHICmdList)
        {
            RHICmdList.SetComputeShader(MCShader.GetComputeShader());
            MCShader->SetParameters(RHICmdList, *MCParams);
            RHICmdList.DispatchComputeShader(MCThreadGroupCount.X, MCThreadGroupCount.Y, MCThreadGroupCount.Z);
            MCShader->UnsetParameters(RHICmdList, *MCParams);
        }
    );

    // 10. 回读计数器（获取实际生成的顶点/三角形数量）
    TArray<int32> CounterReadbackData;
    GraphBuilder.EnqueueReadback(VertexCounterBuffer, CounterReadbackData, 0, 1);
    GraphBuilder.EnqueueReadback(TriangleCounterBuffer, CounterReadbackData, 1, 1);

    // 11. 执行Render Graph并设置完成回调（异步回读网格数据）
    LastRenderGraphEvent = GraphBuilder.Execute();
    ENQUEUE_RENDER_COMMAND(RDG_MeshReadback)(
        [this, VertexBuffer, TriangleBuffer](FRHICommandListImmediate& RHICmdList)
        {
            // 等待RDG执行完成
            if (LastRenderGraphEvent.IsValid())
            {
                LastRenderGraphEvent->Wait();
            }

            // 读取计数器数据
            int32 NumVertices = 0;
            int32 NumTriangles = 0;
            RHICmdList.ReadBufferReference(VertexCounterBuffer->Buffer, &NumVertices);
            RHICmdList.ReadBufferReference(TriangleCounterBuffer->Buffer, &NumTriangles);

            // 回读网格数据（仅当有有效数据时）
            if (NumVertices > 0 && NumTriangles > 0)
            {
                ReadbackMeshData(VertexBuffer, TriangleBuffer, NumVertices, NumTriangles);
            }
        }
    );
}

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

// 更新DynamicMeshComponent（渲染生成的网格）
void UGPUSDFCutter::UpdateDynamicMesh(const TArray<FVector>& Vertices, const TArray<FIntVector>& Triangles)
{
    if (!TargetMeshComponent || Vertices.Num() == 0 || Triangles.Num() == 0)
        return;

    // 获取DynamicMesh并清空旧数据
    TSharedPtr<FDynamicMesh3> DynamicMesh = TargetMeshComponent->GetDynamicMesh();
    if (!DynamicMesh)
    {
        DynamicMesh = MakeShared<FDynamicMesh3>();
        TargetMeshComponent->SetDynamicMesh(DynamicMesh);
    }
    DynamicMesh->Clear();

    // 添加顶点
    for (const FVector& Vert : Vertices)
    {
        DynamicMesh->AddVertex(Vert);
    }

    // 添加三角形（确保索引有效）
    for (const FIntVector& Tri : Triangles)
    {
        if (Tri.X >= 0 && Tri.X < Vertices.Num() &&
            Tri.Y >= 0 && Tri.Y < Vertices.Num() &&
            Tri.Z >= 0 && Tri.Z < Vertices.Num())
        {
            DynamicMesh->AddTriangle(Tri.X, Tri.Y, Tri.Z);
        }
    }

    // 计算法线（优化渲染效果）
    DynamicMesh->ComputeNormals();
    DynamicMesh->ReverseOrientation(); // 确保法线方向正确

    // 更新网格组件（触发渲染）
    TargetMeshComponent->NotifyMeshUpdated();

    // 触发回调
    OnMeshGenerated.Broadcast(TargetMeshComponent);
}

// 计算工具在原始物体空间的AABB（用于优化SDF更新范围，只更新工具影响的体素）
FAxisAlignedBox UGPUSDFCutter::ComputeToolAABBInOriginalSpace(const FTransform& ToolTransform)
{
    // 假设工具的本地AABB是[-ToolHalfSize, ToolHalfSize]（需与工具SDF预生成范围一致）
    float ToolHalfSize = 50.0f; // 工具SDF预生成范围是[-50,50]，需手动匹配
    TArray<FVector> ToolLocalCorners = {
        FVector(-ToolHalfSize, -ToolHalfSize, -ToolHalfSize),
        FVector(ToolHalfSize, -ToolHalfSize, -ToolHalfSize),
        FVector(ToolHalfSize, ToolHalfSize, -ToolHalfSize),
        FVector(-ToolHalfSize, ToolHalfSize, -ToolHalfSize),
        FVector(-ToolHalfSize, -ToolHalfSize, ToolHalfSize),
        FVector(ToolHalfSize, -ToolHalfSize, ToolHalfSize),
        FVector(ToolHalfSize, ToolHalfSize, ToolHalfSize),
        FVector(-ToolHalfSize, ToolHalfSize, ToolHalfSize)
    };

    // 转换到原始物体空间
    FAxisAlignedBox ToolAABB;
    for (const FVector& LocalCorner : ToolLocalCorners)
    {
        FVector WorldCorner = ToolTransform.TransformPosition(LocalCorner);
        ToolAABB += WorldCorner;
    }

    // 与原始物体边界交集（只保留重叠部分）
    ToolAABB = ToolAABB.Intersect(OriginalBounds);
    return ToolAABB;
}