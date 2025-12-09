// Fill out your copyright notice in the Description page of Project Settings.


#include "GPUSDFCutter.h"

#include "DynamicMeshEditor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Components/DynamicMeshComponent.h"
#include "Engine/VolumeTexture.h"
#include "RenderGraphUtils.h"
#include "UpdateSDFShader.h"
#include "MarchingCubesShader.h"
#include "DynamicMesh/MeshNormals.h"

IMPLEMENT_UNIFORM_BUFFER_STRUCT(FCutUB,"CutUB");
IMPLEMENT_GLOBAL_SHADER(FUpdateSDFCS, "/SDF/Shaders/DynamicSDFUpdateCS.usf", "LocalSDFUpdateKernel", SF_Compute);

IMPLEMENT_UNIFORM_BUFFER_STRUCT(FMCUB,"MCUB");
IMPLEMENT_GLOBAL_SHADER(FLocalMarchingCubesCS, "/SDF/Shaders/MarchingCubesCS.usf", "MarchingCubesKernel", SF_Compute);

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
	
	UpdateToolTransform(CutToolActor->GetActorTransform());
	
	if (!bGPUResourcesInitialized || !TargetMeshActor)
		return;

	// 只有工具位置变化时才更新
	if (bToolTransformDirty)
	{
		UE_LOG(LogTemp, Warning, TEXT("Transform Updated, Dispatching Work"));
		DispatchLocalUpdate([this](const TArray<FVector>& Vertices, const TArray<FIntVector>& Triangles) {
			this->ReadbackMeshData(Vertices, Triangles);
		});
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

	// 初始化GPU资源
	InitGPUResources();
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

void UGPUSDFCutter::InitGPUResources()
 {
    if (bGPUResourcesInitialized)
     	return;

	// 1. 存储外部纹理的RHI引用
	OriginalSDFRHIRef = OriginalSDFTexture->GetResource()->TextureRHI;
	ToolSDFRHIRef = ToolSDFTexture->GetResource()->TextureRHI;

	SDFDimensions = FIntVector(
		OriginalSDFTexture->GetSizeX(),
		OriginalSDFTexture->GetSizeY(),
		OriginalSDFTexture->GetSizeZ()
	);
	
    ENQUEUE_RENDER_COMMAND(InitGPUSDFCutterResources)([this,OriginalTextureRHI = OriginalSDFRHIRef](FRHICommandListImmediate& RHICmdList)
    {
 
     	FRDGBuilder GraphBuilder(RHICmdList); 

     	FRDGTextureDesc DynamicSDFDesc = FRDGTextureDesc::Create3D(
			SDFDimensions,
     		PF_R32_FLOAT,
     		FClearValueBinding::None,
     		TexCreate_UAV | TexCreate_RenderTargetable // 只保留必要的标志
		);
		FRDGTextureRef OutTexture = GraphBuilder.CreateTexture(DynamicSDFDesc, TEXT("DynamicSDF"));
		DynamicSDFTexturePooled = GraphBuilder.ConvertToExternalTexture(OutTexture);

    	// 注册外部资源
    	FRDGTextureRef RDGOriginalSDFTexture  = RegisterExternalTexture(GraphBuilder, OriginalTextureRHI, TEXT("VolumeTexture"));

    	FRHICopyTextureInfo CopyInfo;
		CopyInfo.SourceMipIndex = 0;
		CopyInfo.DestMipIndex = 0;
    	// 5. 初始化动态SDF为原始SDF值
    	AddCopyTexturePass(GraphBuilder, RDGOriginalSDFTexture , OutTexture, CopyInfo); 
    	
		GraphBuilder.Execute();	

		bGPUResourcesInitialized = true;

	});
	
}

void UGPUSDFCutter::CalculateToolAABBInTargetSpace(const FTransform& ToolTransform, FIntVector& OutVoxelMin, FIntVector& OutVoxelMax)
{
	if (!CutToolActor||!TargetMeshActor)
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
    ENQUEUE_RENDER_COMMAND(GPUSDFCutter_LocalUpdate)([this,AsyncCallback](FRHICommandListImmediate& RHICmdList)
    {
        FRDGBuilder GraphBuilder(RHICmdList);

        // 计算更新区域
        FIntVector UpdateMin, UpdateMax;
        CalculateToolAABBInTargetSpace(CurrentToolTransform, UpdateMin, UpdateMax);
        
        // 如果区域无效，跳过
        if (UpdateMin.X > UpdateMax.X || UpdateMin.Y > UpdateMax.Y || UpdateMin.Z > UpdateMax.Z)
            return;

        // 注册纹理
        FRDGTextureRef OriginalTexture = RegisterExternalTexture(GraphBuilder, OriginalSDFRHIRef,TEXT("OriginalSDFTexture"));
        FRDGTextureRef ToolTexture = RegisterExternalTexture(GraphBuilder, ToolSDFRHIRef, TEXT("ToolSDFTexture"));
        FRDGTextureRef DynamicSDFTexture = GraphBuilder.RegisterExternalTexture(
        	DynamicSDFTexturePooled, TEXT("DynamicSDFTexture"));

    	FRDGTextureRef UpdatedSDFTexture = nullptr;
    	
        // 1. 局部SDF更新
        {
        	// 创建临时纹理用于更新结果
			FRDGTextureDesc TempSDFDesc = FRDGTextureDesc::Create3D(
				SDFDimensions, PF_R32_FLOAT, FClearValueBinding::None, 
				TexCreate_ShaderResource | TexCreate_UAV);
                
			UpdatedSDFTexture = GraphBuilder.CreateTexture(TempSDFDesc, TEXT("TempSDF"));
            
			// 首先复制当前动态SDF到临时纹理
			AddCopyTexturePass(GraphBuilder, DynamicSDFTexture, UpdatedSDFTexture);
        	
        	auto* CutUBParams = GraphBuilder.AllocParameters<FCutUB>();        	
        	CutUBParams->ObjectLocalBoundsMin = FVector3f(TargetLocalBounds.Min);
			CutUBParams->ObjectLocalBoundsMax = FVector3f(TargetLocalBounds.Max);
			CutUBParams->ToolLocalBoundsMin = FVector3f(ToolLocalBounds.Min);
			CutUBParams->ToolLocalBoundsMax = FVector3f(ToolLocalBounds.Max);
			CutUBParams->ObjectToToolTransform = FMatrix44f(CurrentObjectTransform.ToMatrixWithScale());
			CutUBParams->ToolToObjectTransform = FMatrix44f(CurrentObjectTransform.Inverse().ToMatrixWithScale());
            CutUBParams->SDFDimensions = SDFDimensions;
            CutUBParams->VoxelSize = VoxelSize;
            CutUBParams->IsoValue = 0.0f;
            CutUBParams->UpdateRegionMin = UpdateMin;
            CutUBParams->UpdateRegionMax = UpdateMax;      

            auto* PassParams = GraphBuilder.AllocParameters<FUpdateSDFCS::FParameters>();
            PassParams->Params = GraphBuilder.CreateUniformBuffer(CutUBParams);
            PassParams->OriginalSDF = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(OriginalTexture));
            PassParams->ToolSDF = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ToolTexture));
            PassParams->DynamicSDF = GraphBuilder.CreateUAV(UpdatedSDFTexture);
            PassParams->OriginalSDFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
            PassParams->ToolSDFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

            // 只调度更新区域
            FIntVector RegionSize = UpdateMax - UpdateMin + FIntVector(1);
            FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(RegionSize, FIntVector(4, 4, 4));

            TShaderMapRef<FUpdateSDFCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("LocalSDFUpdate"),
                ComputeShader,
                PassParams,
                GroupCount
            );
        }

        // ========== 2. 全量Marching Cubes（遍历整个SDF） ==========
		{
            // 全量生成：覆盖整个SDF体素范围（不再依赖工具影响区）
            FIntVector GenerateMin(0, 0, 0);
            FIntVector GenerateMax = SDFDimensions - FIntVector(2, 2, 2); // 避免体素越界

            // 全量MC需扩容缓冲区（按总体现素估算最大顶点/三角形数）
            const int32 TotalVoxels = SDFDimensions.X * SDFDimensions.Y * SDFDimensions.Z;
            const int32 MaxVertices = TotalVoxels * 5;  // 每个体素最多5个顶点
            const int32 MaxTriangles = TotalVoxels * 2; // 每个体素最多2个三角形

            // 创建全量缓冲区
            FRDGBufferRef VertexBuffer = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), MaxVertices), TEXT("FullMCVertices"));
            FRDGBufferRef TriangleBuffer = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateStructuredDesc(sizeof(FIntVector), MaxTriangles), TEXT("FullMCTriangles"));
            FRDGBufferRef VertexCounter = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateBufferDesc(sizeof(int32), 1), TEXT("FullMCVertexCounter"));
            FRDGBufferRef TriangleCounter = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateBufferDesc(sizeof(int32), 1), TEXT("FullMCTriangleCounter"));

            // 清空计数器
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(VertexCounter, PF_R32_SINT), 0);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(TriangleCounter, PF_R32_SINT), 0);
			
            // MC参数：生成区域设为全量
            auto* MCUBParams = GraphBuilder.AllocParameters<FMCUB>();
            MCUBParams->SDFBoundsMin = FVector3f(TargetLocalBounds.Min);
            MCUBParams->SDFBoundsMax = FVector3f(TargetLocalBounds.Max);
            MCUBParams->SDFDimensions = SDFDimensions;
            MCUBParams->IsoValue = 0.0f;
        	MCUBParams->CubeSize = VoxelSize;
            MCUBParams->GenerateRegionMin = GenerateMin; // 全量：从(0,0,0)开始
            MCUBParams->GenerateRegionMax = GenerateMax; // 全量：覆盖整个SDF

            auto* PassParams = GraphBuilder.AllocParameters<FLocalMarchingCubesCS::FParameters>();
            PassParams->Params = GraphBuilder.CreateUniformBuffer(MCUBParams);
            PassParams->DynamicSDF = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(UpdatedSDFTexture));
            PassParams->OutVertices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VertexBuffer, PF_R32G32B32F));
            PassParams->OutTriangles = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TriangleBuffer, PF_R8G8B8A8_UINT));
            PassParams->VertexCounter = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VertexCounter, PF_R32_SINT));
            PassParams->TriangleCounter = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TriangleCounter, PF_R32_SINT));
			
            // 全量MC的线程调度（按整个SDF范围计算）
            FIntVector MCRegionSize = GenerateMax - GenerateMin + FIntVector(1);
            FIntVector MCGroupCount = FComputeShaderUtils::GetGroupCount(MCRegionSize, FIntVector(4, 4, 4));

            TShaderMapRef<FLocalMarchingCubesCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("FullMarchingCubes"),
                ComputeShader,
                PassParams,
                MCGroupCount
            );

        	// 将更新后的SDF复制回持久化纹理
        	AddCopyTexturePass(GraphBuilder, UpdatedSDFTexture, DynamicSDFTexture);

			
        	FRHIGPUBufferReadback* VertexReadback = new FRHIGPUBufferReadback(TEXT("VerticesReadback"));
        	FRHIGPUBufferReadback* TriangleReadback = new FRHIGPUBufferReadback(TEXT("TrianglesReadback"));
        	FRHIGPUBufferReadback* VertexCounterReadback = new FRHIGPUBufferReadback(TEXT("VertexCounterReadback"));
        	FRHIGPUBufferReadback* TriangleCounterReadback = new FRHIGPUBufferReadback(TEXT("TriangleCounterReadback"));

        	// 5. 添加拷贝Pass：将RDG缓冲区数据拷贝到回读对象（AddEnqueueCopyPass）
			AddEnqueueCopyPass(GraphBuilder,VertexReadback,VertexBuffer,0u);
        	AddEnqueueCopyPass(GraphBuilder,TriangleReadback,TriangleBuffer,0u);
        	AddEnqueueCopyPass(GraphBuilder,VertexCounterReadback,VertexCounter,0u);
        	AddEnqueueCopyPass(GraphBuilder,TriangleCounterReadback,TriangleCounter,0u);

        	auto RunnerFunc = [VertexReadback,TriangleReadback,VertexCounterReadback,TriangleCounterReadback, AsyncCallback](auto&& RunnerFunc)->void
        	{
        		if (VertexReadback->IsReady() && TriangleReadback->IsReady() && VertexCounterReadback->IsReady() && TriangleCounterReadback->IsReady())
        		{
        			// 读取顶点数量
        			int32 VertexCount = 0;
        			void* LockedData = VertexCounterReadback->Lock(sizeof(int32));
					if (LockedData)
					{
						VertexCount = *(int32*)LockedData;
					}
        			VertexCounterReadback->Unlock();        			

        			// 读取顶点
        			TArray<FVector> Vertices;
        			Vertices.SetNumUninitialized(VertexCount);
        			LockedData = nullptr;
        			LockedData = VertexReadback->Lock(sizeof(FVector3f) * VertexCount);
        			if (LockedData)
        			{
        				// 将void*转换为FVector3f*指针
						FVector3f* VertexData = static_cast<FVector3f*>(LockedData);
						    
						for (int32 i = 0; i < VertexCount; i++)
						{
							// 直接通过指针索引访问，不再需要手动计算字节偏移
							Vertices[i] = FVector(VertexData[i]); // 如果需要转换为FVector
						}        				
        			}
        			VertexReadback->Unlock();

        			// 读取三角形数量
					int32 TriangleCount = 0;
					LockedData = nullptr;
					LockedData = TriangleCounterReadback->Lock(sizeof(int32));
					if (LockedData)
					{
						TriangleCount = *(int32*)LockedData;
					}
					TriangleCounterReadback->Unlock();

        			// 读取三角形数据
        			TArray<FIntVector> Triangles;
        			Triangles.SetNumUninitialized(TriangleCount);
        			LockedData = nullptr;
        			LockedData = TriangleCounterReadback->Lock(sizeof(FIntVector) * TriangleCount);
        			if (LockedData)
        			{
        				FIntVector* TriangleData = static_cast<FIntVector*>(LockedData);
        				for (int32 i = 0; i < TriangleCount; i++)
        				{
        					Triangles[i] = TriangleData[i];
        				}
        			}

        			AsyncTask(ENamedThreads::GameThread, [AsyncCallback, TriangleArray = MoveTemp(Triangles), VerticesArray = MoveTemp(Vertices)] ()mutable {
							AsyncCallback(MoveTemp(VerticesArray), MoveTemp(TriangleArray));        				
							});
        			
        			delete TriangleCounterReadback;
        			delete TriangleReadback;
        			delete VertexCounterReadback;
        			delete VertexReadback;
        			
        		}
		        else
		        {
		        	AsyncTask(ENamedThreads::ActualRenderingThread, [RunnerFunc]() {
						RunnerFunc(RunnerFunc);
						});
		        }
        	};

        	AsyncTask(ENamedThreads::ActualRenderingThread, [RunnerFunc]() {
				RunnerFunc(RunnerFunc);
				});

			
        }

    	GraphBuilder.Execute();
    });
}


void UGPUSDFCutter::ReadbackMeshData(const TArray<FVector>& Vertices, const TArray<FIntVector>& Triangles)
{	
	// 切换到下一个缓冲区
	int32 WriteBufferIndex = CurrentMeshBuffer;
	CurrentMeshBuffer = (CurrentMeshBuffer + 1) % 2;
	
	// 存储顶点数据
	MeshVertices[WriteBufferIndex] = Vertices;
	MeshTriangles[WriteBufferIndex] = Triangles;
	
	// 标记有新数据可用
	bHasPendingMeshData = true;
	
	UE_LOG(LogTemp, Log, TEXT("Mesh data received: %d vertices, %d triangles"), Vertices.Num(), Triangles.Num());
	
}


void UGPUSDFCutter::UpdateDynamicMesh(const TArray<FVector>& Vertices, const TArray<FIntVector>& Triangles)
{
	if (!TargetMeshActor || Vertices.Num() == 0 || Triangles.Num() == 0)
		return;

	UDynamicMeshComponent* DynamicMeshComp = TargetMeshActor->GetDynamicMeshComponent();
	if (!DynamicMeshComp) return;

	// 开始网格编辑
	DynamicMeshComp->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		// 清空现有网格
		EditMesh.Clear();
		
		// 1. 添加顶点
		TArray<int32> VertexIDs;
		VertexIDs.SetNumUninitialized(Vertices.Num());
		
		for (int32 i = 0; i < Vertices.Num(); i++)
		{
			// 将FVector转换为FVector3d并添加顶点
			VertexIDs[i] = EditMesh.AppendVertex(FVector3d(Vertices[i]));
		}
		
		// 2. 添加三角形
		for (const FIntVector& Triangle : Triangles)
		{
			// 检查三角形索引是否有效
			if (Triangle.X >= 0 && Triangle.X < VertexIDs.Num() &&
				Triangle.Y >= 0 && Triangle.Y < VertexIDs.Num() &&
				Triangle.Z >= 0 && Triangle.Z < VertexIDs.Num())
			{
				int32 VertIdx0 = VertexIDs[Triangle.X];
				int32 VertIdx1 = VertexIDs[Triangle.Y];
				int32 VertIdx2 = VertexIDs[Triangle.Z];
				
				// 确保顶点ID有效
				if (EditMesh.IsVertex(VertIdx0) && EditMesh.IsVertex(VertIdx1) && EditMesh.IsVertex(VertIdx2))
				{
					EditMesh.AppendTriangle(VertIdx0, VertIdx1, VertIdx2);
				}
			}
		}
		
		// 3. 计算法线（可选，但推荐）
		if (EditMesh.TriangleCount() > 0)
		{
			// 确保有法线属性
			if (!EditMesh.HasAttributes())
			{
				EditMesh.EnableAttributes();
			}
			
			if (EditMesh.HasAttributes() && EditMesh.Attributes()->PrimaryNormals())
			{
				UE::Geometry::FDynamicMeshNormalOverlay* Normals = EditMesh.Attributes()->PrimaryNormals();
				// 计算每个顶点的法线
				UE::Geometry::FMeshNormals::InitializeOverlayToPerVertexNormals(Normals);
			}
		}
		
		return true; // 编辑成功
	});
	
	// 4. 通知组件网格已更新
	DynamicMeshComp->NotifyMeshUpdated();
	DynamicMeshComp->MarkRenderStateDirty();
	
	// 5. 重建碰撞
	DynamicMeshComp->bDeferCollisionUpdates = false;
	DynamicMeshComp->UpdateCollision(true);
	
	
	// 触发渲染更新
	DynamicMeshComp->NotifyMeshUpdated();
	DynamicMeshComp->MarkRenderDynamicDataDirty();
	
	UE_LOG(LogTemp, Log, TEXT("Mesh updated with %d vertices and %d triangles"), Vertices.Num(), Triangles.Num());


	OnMeshGenerated.ExecuteIfBound(DynamicMeshComp);
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

