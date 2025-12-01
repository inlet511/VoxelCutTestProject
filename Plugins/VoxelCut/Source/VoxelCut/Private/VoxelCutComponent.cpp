// Fill out your copyright notice in the Description page of Project Settings.


#include "VoxelCutComponent.h"
#include "DynamicMesh/MeshTransforms.h"
#include "Engine/Engine.h"


UVoxelCutComponent::UVoxelCutComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
    
	CutState = ECutState::Idle;
	bCuttingEnabled = false;
	DistanceSinceLastUpdate = 0.0f;	
}

void UVoxelCutComponent::BeginPlay()
{
	Super::BeginPlay();
}


void UVoxelCutComponent::InitializeCutSystem()
{
	if (bSystemInitialized || !TargetMeshComponent || !CutToolMeshComponent)
		return;
    
	// 创建切削操作器（只创建一次）
	if (!CutOp.IsValid())
	{
		CutOp = MakeShared<FVoxelCutMeshOp>();
		// 绑定体素数据更新回调
		// 使用 TWeakObjectPtr 包装 UObject 指针，避免悬垂引用
		TWeakObjectPtr<UVoxelCutComponent> ThisWeakPtr(this);
		CutOp->OnVoxelDataUpdated.BindLambda([ThisWeakPtr]()
		{
			if (ThisWeakPtr.IsValid())
			{
				ThisWeakPtr->OnVoxelDataUpdated();
			}
		});
	}
    
	// 设置基础参数
	CutOp->bSmoothCutEdges = bSmoothEdges;
	CutOp->SmoothingIteration = SmoothingIteration;
	CutOp->SmoothingStrength = SmoothingStrength;
	CutOp->bFillCutHole = bFillHoles;
	CutOp->UpdateMargin = 5;
	CutOp->MarchingCubeSize = MarchingCubeSize;
	CutOp->MaxOctreeDepth = MaxOctreeDepth;
	CutOp->MinVoxelSize = MinVoxelSize;
	CutOp->CutToolMesh = CopyToolMesh();
	
    
	// 初始化目标物体体素
	UDynamicMesh* TargetDynamicMesh = TargetMeshComponent->GetDynamicMesh();
	if (TargetDynamicMesh)
	{
		// 创建目标网格的副本（只做一次）
		CutOp->TargetMesh = MakeShared<FDynamicMesh3>();
		TargetDynamicMesh->ProcessMesh([this](const FDynamicMesh3& SourceMesh)
		{
			CutOp->TargetMesh->Copy(SourceMesh);
		});        
		// 设置目标变换
		CutOp->TargetTransform = TargetMeshComponent->GetComponentTransform();

		// 体素化切割目标（只做一次）
		CutOp->InitializeVoxelData(nullptr);
	}

	//VisualizeOctreeNode();

	// 初始化切割工具VolumeTexture资源
	InitToolSDFAsync(64);

	//bSystemInitialized = true;
}

void UVoxelCutComponent::OnCutSystemInitialized()
{
	bSystemInitialized = true;
}

void UVoxelCutComponent::InitToolSDFAsync(int32 TextureSize)
{
	// 1. 防重复调用：如果正在预计算，直接返回
	if (bIsPrecomputingSDF)
	{
		UE_LOG(LogTemp, Warning, TEXT("SDF预计算已在进行中，跳过重复调用"));
		return;
	}

	// 2. 检查前置条件（CutOp和网格不能为空）
	if (!CutOp.IsValid() || !CutOp->CutToolMesh.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("CutOp或切割工具网格未初始化，无法预计算SDF"));
		return;
	}

	// 3. 标记为正在预计算
	bIsPrecomputingSDF = true;

	// 4. 创建SDF生成器
	ToolSDFGenerator = MakeShared<FToolSDFGenerator>();
	TWeakObjectPtr<UVoxelCutComponent> WeakSelf = this;
	// 5. 异步调用PrecomputeSDFAsync，传入回调函数
	ToolSDFGenerator->PrecomputeSDFAsync(
		*CutOp->CutToolMesh,  // 传入工具网格（const引用）
		TextureSize,          // 纹理尺寸
		[WeakSelf, this](bool bSuccess) // 回调：捕获弱引用避免循环引用
		{
			// 切回游戏线程执行业务逻辑（关键：避免跨线程操作CutOp）
			AsyncTask(ENamedThreads::GameThread, [this, WeakSelf, bSuccess]()
			{
				// 检查组件是否还存在（防止异步过程中组件被销毁）
				if (!WeakSelf.IsValid() || !WeakSelf.Pin())
				{
					return;
				}

				// 重置预计算状态
				bIsPrecomputingSDF = false;

				// 6. 处理异步结果
				if (bSuccess)
				{
					UE_LOG(LogTemp, Warning, TEXT("工具SDF预计算完成（异步）"));
					// 复制SDF生成器到CutOp（共享指针自动管理生命周期）
					CutOp->ToolSDFGenerator = ToolSDFGenerator;

					// 切削系统初始化完成
					OnCutSystemInitialized();
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("工具SDF预计算失败（异步）"));
					// 失败时释放无效的生成器
					ToolSDFGenerator.Reset();
					CutOp->ToolSDFGenerator.Reset();
				}
			});
		}
	);
}


void UVoxelCutComponent::EnableCutting()
{
	bCuttingEnabled = true;
    
	// 记录初始工具位置
	if (CutToolMeshComponent)
	{
		LastToolPosition = CutToolMeshComponent->GetComponentLocation();
		LastToolRotation = CutToolMeshComponent->GetComponentRotation();
	}
    
	DistanceSinceLastUpdate = 0.0f;
}

void UVoxelCutComponent::DisableCutting()
{
	bCuttingEnabled = false;
}


void UVoxelCutComponent::TickComponent(float DeltaTime, ELevelTick TickType,
									   FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bSystemInitialized || !bCuttingEnabled || !CutToolMeshComponent || !TargetMeshComponent)
		return;

	// 获取当前工具位置
	FTransform CurrentTransform = CutToolMeshComponent->GetComponentTransform();
	
	
	// 检查是否需要切削更新
	if (NeedsCutUpdate(CurrentTransform))
	{
		RequestCut(CurrentTransform);
        
		// 重置距离计数
		DistanceSinceLastUpdate = 0.0f;
		LastToolPosition = CurrentTransform.GetLocation();
		LastToolRotation = CurrentTransform.GetRotation().Rotator();
	}
	else
	{
		DistanceSinceLastUpdate += FVector::Distance(LastToolPosition, CurrentTransform.GetLocation());
	}
    
	// 更新状态机
	UpdateStateMachine();
}

void UVoxelCutComponent::OnVoxelDataUpdated()
{
	if (!CutOp.IsValid() || !CutOp->PersistentVoxelData.IsValid())
		return;

	VoxelUpdatedTimeStamp = FPlatformTime::Seconds();
	
	// 在游戏线程生成新的网格
	Async(EAsyncExecution::TaskGraphMainThread, [this]()
	{
		FProgressCancel Cancel;
		CutOp->ConvertVoxelsToMesh(*CutOp->PersistentVoxelData, &Cancel);
		FDynamicMesh3* ResultMesh = CutOp->GetResultMesh();
		OnCutComplete(ResultMesh);
	});
}

void UVoxelCutComponent::OnCutComplete(FDynamicMesh3* ResultMesh)
{
	if (ResultMesh)
	{
		// 更新结果网格
		if (TargetMeshComponent)
		{
			UDynamicMesh* DynamicMesh = TargetMeshComponent->GetDynamicMesh();
			if (DynamicMesh)
			{
				DynamicMesh->SetMesh(*ResultMesh);
				TargetMeshComponent->NotifyMeshUpdated(); 
			}
		}
	}

	CutCompleteTimeStamp = FPlatformTime::Seconds();

	UE_LOG(LogTemp, Warning, TEXT("体素化切削耗时: %.2f 毫秒"), (VoxelUpdatedTimeStamp - StartCutTimeStamp) * 1000.0);
	UE_LOG(LogTemp, Warning, TEXT("体素网格化耗时: %.2f 毫秒"), (CutCompleteTimeStamp - VoxelUpdatedTimeStamp) * 1000.0);
	
	// 更新状态
	FScopeLock Lock(&StateLock);
	CutState = ECutState::Completed;
}

bool UVoxelCutComponent::NeedsCutUpdate(const FTransform& InCurrentToolTransform)
{
	float Distance = FVector::Distance(LastToolPosition, InCurrentToolTransform.GetLocation());
	float AngleDiff = FQuat::Error(LastToolRotation.Quaternion(), InCurrentToolTransform.GetRotation());
	return (DistanceSinceLastUpdate + Distance >= UpdateThreshold) || 
		   (AngleDiff > FMath::DegreesToRadians(5.0f));
}

void UVoxelCutComponent::UpdateStateMachine()
{
	FScopeLock Lock(&StateLock);
    
	switch (CutState)
	{
	case ECutState::Idle:		
		break;        
	case ECutState::RequestPending:
		StartAsyncCut();		
		break;        
	case ECutState::Processing:
		break;        
	case ECutState::Completed:
		CutState = ECutState::Idle;		
		break;
	}
}

void UVoxelCutComponent::RequestCut(const FTransform& ToolTransform)
{
	FScopeLock Lock(&StateLock);
    
	// 保存当前请求数据
	CurrentToolTransform = ToolTransform;
	
	// 只有在空闲状态或有新请求时才更新
	if (CutState == ECutState::Idle || CutState == ECutState::Completed)
	{		
		CutState = ECutState::RequestPending;
	}
}

void UVoxelCutComponent::StartAsyncCut()
{
	FScopeLock Lock(&StateLock);
	// 如果已经在处理，则退出
	if (CutState==ECutState::Processing)
		return;
	CutState = ECutState::Processing;
	
    CutOp->CutToolTransform = CurrentToolTransform;

	StartCutTimeStamp = FPlatformTime::Seconds();
	
    // 在异步线程中执行更新体素
    Async(EAsyncExecution::ThreadPool, [this]()
    {
        CutOp->UpdateLocalRegion();
    });
}

TSharedPtr<FDynamicMesh3> UVoxelCutComponent::CopyToolMesh()
{
	if (!CutToolMeshComponent)
		return nullptr;
    
	TSharedPtr<FDynamicMesh3> CopiedMesh = MakeShared<FDynamicMesh3>();
    
	UDynamicMesh* SourceMesh = CutToolMeshComponent->GetDynamicMesh();
	if (SourceMesh)
	{
		SourceMesh->ProcessMesh([&CopiedMesh](const FDynamicMesh3& SourceMeshData)
		{
			CopiedMesh->Copy(SourceMeshData);
		});
	}
    
	return CopiedMesh;
}

void UVoxelCutComponent::VisualizeOctreeNode()
{
	if (!CutOp || !CutOp->PersistentVoxelData.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Voxel data is not valid for visualization"));
		return;
	}
	
	// 清空之前的可视化
	ClearOctreeVisualization();
	
	// 递归遍历八叉树并绘制边界框
	VisualizeOctreeNodeRecursive(CutOp->PersistentVoxelData->OctreeRoot, 0);
	
	UE_LOG(LogTemp, Log, TEXT("Octree visualization completed with %d boxes"), DebugBoxes.Num());
}

void UVoxelCutComponent::VisualizeOctreeNodeRecursive(const FOctreeNode& Node, int32 Depth)
{
	if (!GetWorld())
		return;
	
	// 根据节点深度选择颜色
	FColor NodeColor;
	switch (Depth % 6)
	{
	case 0: NodeColor = FColor::Red; break;
	case 1: NodeColor = FColor::Green; break;
	case 2: NodeColor = FColor::Blue; break;
	case 3: NodeColor = FColor::Yellow; break;
	case 4: NodeColor = FColor::Cyan; break;
	case 5: NodeColor = FColor::Magenta; break;
	default: NodeColor = FColor::White;
	}
	
	// 绘制节点边界框
	FVector Center = Node.Bounds.Center();
	FVector Extent = Node.Bounds.Extents();
	

	
	// 存储调试信息以便后续管理
	FDebugBoxInfo BoxInfo;
	BoxInfo.Center = Center;
	BoxInfo.Extent = Extent;
	BoxInfo.Color = NodeColor;
	DebugBoxes.Add(BoxInfo);
	
	// 如果是叶子节点
	if (Node.bIsLeaf)
	{
		FString VoxelInfo = FString::Printf(TEXT("L"));
		NodeColor = Node.Voxel >= 0 ? FColor::Red : FColor::Green;
		DrawDebugBox(GetWorld(), Center, Extent/4.0f, NodeColor, true, -1.0f, 0, 0.5f);
	}
	
	// 递归处理子节点
	for (const FOctreeNode& Child : Node.Children)
	{
		VisualizeOctreeNodeRecursive(Child, Depth + 1);
	}
}

void UVoxelCutComponent::ClearOctreeVisualization()
{
	DebugBoxes.Empty();
	FlushPersistentDebugLines(GetWorld());
}

void UVoxelCutComponent::PrintOctreeDetails()
{
    if (!CutOp || !CutOp->PersistentVoxelData.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("无法打印八叉树详情：体素数据未初始化"));
        return;
    }
    
    UE_LOG(LogTemp, Warning, TEXT("========== 八叉树详细信息 =========="));
    
    int32 TotalNodes = 0;
    int32 TotalLeaves = 0;
    int32 TotalVoxels = 0;
    
    // 递归打印
    PrintOctreeNodeRecursive(CutOp->PersistentVoxelData->OctreeRoot, 0, TotalNodes, TotalLeaves, TotalVoxels);
    
    UE_LOG(LogTemp, Warning, TEXT("八叉树统计摘要:"));
    UE_LOG(LogTemp, Warning, TEXT("  总节点数: %d"), TotalNodes);
    UE_LOG(LogTemp, Warning, TEXT("  叶子节点数: %d"), TotalLeaves);
    UE_LOG(LogTemp, Warning, TEXT("  非叶子节点数: %d"), TotalNodes - TotalLeaves);
    UE_LOG(LogTemp, Warning, TEXT("  存储的体素数: %d"), TotalVoxels);
    UE_LOG(LogTemp, Warning, TEXT("  根节点边界: Min(%s), Max(%s)"), 
           *CutOp->PersistentVoxelData->OctreeRoot.Bounds.Min.ToString(), 
           *CutOp->PersistentVoxelData->OctreeRoot.Bounds.Max.ToString());
    UE_LOG(LogTemp, Warning, TEXT("  根节点尺寸: %s"), 
           *(CutOp->PersistentVoxelData->OctreeRoot.Bounds.Max-CutOp->PersistentVoxelData->OctreeRoot.Bounds.Min).ToString());
    UE_LOG(LogTemp, Warning, TEXT("========== 结束八叉树信息 =========="));
}

void UVoxelCutComponent::PrintOctreeNodeRecursive(const FOctreeNode& Node, int32 Depth, int32& NodeCount, int32& LeafCount, int32& TotalVoxels)
{
    NodeCount++;
    
    // 创建缩进字符串以便于阅读层级关系
    FString Indent;
    for (int32 i = 0; i < Depth; i++)
    {
        Indent += "  ";
    }
    
    FString NodeType = Node.bIsLeaf ? TEXT("叶子") : TEXT("分支");
    FString EmptyStatus = Node.bIsEmpty ? TEXT("空") : TEXT("非空");
    
    UE_LOG(LogTemp, Warning, TEXT("%sL%d-%s-%s: 边界[%s -> %s], 尺寸(%s)"), 
           *Indent, Depth, *NodeType, *EmptyStatus,
           *Node.Bounds.Min.ToString(), 
           *Node.Bounds.Max.ToString(),
           *(Node.Bounds.Max-Node.Bounds.Min).ToString());
    
    if (Node.bIsLeaf)
    {
        LeafCount++;
        TotalVoxels += 8;
        
        if (!Node.bIsEmpty)
        {
            UE_LOG(LogTemp, Warning, TEXT("%s "),     *Indent);
            
            // 打印前几个体素的值作为样本

            FString SampleValues;

            SampleValues += FString::Printf(TEXT("%.2f "), Node.Voxel);
            
            UE_LOG(LogTemp, Warning, TEXT("%s  体素值样本: %s"), *Indent, *SampleValues);
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("%s  子节点数: %d"), *Indent, Node.Children.Num());
    }
    
    // 递归处理子节点
    for (const FOctreeNode& Child : Node.Children)
    {
        PrintOctreeNodeRecursive(Child, Depth + 1, NodeCount, LeafCount, TotalVoxels);
    }
}