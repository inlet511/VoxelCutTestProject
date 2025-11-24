// Fill out your copyright notice in the Description page of Project Settings.


#include "VoxelCutComponent.h"

#include "DynamicMesh/MeshTransforms.h"
#include "Engine/Engine.h"


UVoxelCutComponent::UVoxelCutComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
    
	CutState = ECutState::Idle;
	bIsCutting = false;
	DistanceSinceLastUpdate = 0.0f;	
}

void UVoxelCutComponent::BeginPlay()
{
	Super::BeginPlay();
}


void UVoxelCutComponent::TickComponent(float DeltaTime, ELevelTick TickType,
									   FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bIsCutting || !CutToolMeshComponent || !TargetMeshComponent)
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




void UVoxelCutComponent::StartCutting()
{
	bIsCutting = true;
    
	// 记录初始工具位置
	if (CutToolMeshComponent)
	{
		LastToolPosition = CutToolMeshComponent->GetComponentLocation();
		LastToolRotation = CutToolMeshComponent->GetComponentRotation();
	}
    
	DistanceSinceLastUpdate = 0.0f;
}

void UVoxelCutComponent::StopCutting()
{
	bIsCutting = false;
}



void UVoxelCutComponent::OnCutComplete(FDynamicMesh3* ResultMesh)
{
	if (ResultMesh && ResultMesh->TriangleCount() > 0)
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
    
	// 更新状态
	FScopeLock Lock(&StateLock);
	CutState = ECutState::Completed;
}

void UVoxelCutComponent::InitializeCutSystem()
{
	if (bSystemInitialized || !TargetMeshComponent || !CutToolMeshComponent)
		return;
    
	// 创建切削操作器（只创建一次）
	if (!CutOp.IsValid())
	{
		CutOp = MakeShared<FVoxelCutMeshOp>();
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
	
    
	// 获取目标网格数据
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

	bSystemInitialized = true;

	//VisualizeOctreeNode();
	//PrintOctreeDetails();
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
	
	//UE_LOG(LogTemp, Warning, TEXT("Cut State: %s"),*StaticEnum<ECutState>()->GetNameStringByValue((int64)CutState));
	
	// 只有在空闲状态或有新请求时才更新
	if (CutState == ECutState::Idle || CutState == ECutState::Completed)
	{		
		CutState = ECutState::RequestPending;
		//UE_LOG(LogTemp, Warning, TEXT("Request Pending"));
	}
}

void UVoxelCutComponent::StartAsyncCut()
{
	FScopeLock Lock(&StateLock);
	// 如果已经在处理，则退出
	if (CutState==ECutState::Processing)
		return;
	CutState = ECutState::Processing;
    
    // 复制当前状态到局部变量（避免竞态条件）
    FTransform LocalToolTransform = CurrentToolTransform;
	
    CutOp->CutToolTransform = LocalToolTransform;
    
    // 在异步线程中执行实际切削计算
    Async(EAsyncExecution::ThreadPool, [this]()
    {
        try
        {
            CutOp->CalculateResult(nullptr);
            
            // 切削完成，回到主线程
            Async(EAsyncExecution::TaskGraphMainThread, [this]()
            {
                FDynamicMesh3* ResultMesh = CutOp->GetResultMesh();               
                
                OnCutComplete(ResultMesh);
            });
        }
        catch (const std::exception& e)
        {
            UE_LOG(LogTemp, Error, TEXT("Cut operation failed: %s"), UTF8_TO_TCHAR(e.what()));
            
            Async(EAsyncExecution::TaskGraphMainThread, [this]()
            {
                // 即使失败也标记为完成
                FScopeLock Lock(&StateLock);
                CutState = ECutState::Completed;
            });
        }
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
	
	// 使用DrawDebugBox绘制边界框
	DrawDebugBox(GetWorld(), Center, Extent, NodeColor, true, -1.0f, 0, 2.0f);
	
	// 存储调试信息以便后续管理
	FDebugBoxInfo BoxInfo;
	BoxInfo.Center = Center;
	BoxInfo.Extent = Extent;
	BoxInfo.Color = NodeColor;
	DebugBoxes.Add(BoxInfo);
	
	// 如果是叶子节点，绘制额外的信息
	if (Node.bIsLeaf)
	{
		// 绘制小球标记叶子节点
		DrawDebugSphere(GetWorld(), Center, 5.0f, 8, FColor::White, true, -1.0f, 0, 1.0f);
		
		// 显示体素数量信息
		if (Node.Voxels.Num() > 0)
		{
			FString VoxelInfo = FString::Printf(TEXT("Leaf L%d\nVoxels:%d"), Depth, Node.Voxels.Num());
			DrawDebugString(GetWorld(), Center + FVector(0,0,20), VoxelInfo, nullptr, FColor::White, -1.0f, true);
		}
	}
	else
	{
		// 非叶子节点显示深度信息
		FString DepthInfo = FString::Printf(TEXT("Node L%d\nChildren:%d"), Depth, Node.Children.Num());
		DrawDebugString(GetWorld(), Center + FVector(0,0,20), DepthInfo, nullptr, NodeColor, -1.0f, true);
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
        TotalVoxels += Node.Voxels.Num();
        
        if (!Node.bIsEmpty && Node.Voxels.Num() > 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("%s  体素配置: %d个体素"), 
                   *Indent, Node.Voxels.Num());
            
            // 打印前几个体素的值作为样本
            int32 SampleCount = FMath::Min(5, Node.Voxels.Num());
            FString SampleValues;
            for (int32 i = 0; i < SampleCount; i++)
            {
                SampleValues += FString::Printf(TEXT("%.2f "), Node.Voxels[i]);
            }
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