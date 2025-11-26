// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "VoxelCutMeshOp.h"
#include "Components/DynamicMeshComponent.h"
#include "ToolSDFGenerator.h"
#include "VoxelCutComponent.generated.h"

using namespace UE::Geometry;

// 切削状态枚举
UENUM()
enum class ECutState : uint8
{
	Idle,           // 空闲状态
	RequestPending, // 有切削请求待处理
	Processing,     // 正在处理切削
	Completed       // 切削完成，等待下一帧更新
};

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class VOXELCUT_API UVoxelCutComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	// Sets default values for this component's properties
	UVoxelCutComponent();

	// 设置目标网格（需要被切削的物体）
	UFUNCTION(BlueprintCallable, Category = "Voxel Cut")
	void SetTargetMesh(UDynamicMeshComponent* TargetMeshComp)
	{
		TargetMeshComponent = TargetMeshComp;
	}

	// 设置切削工具网格
	UFUNCTION(BlueprintCallable, Category = "Voxel Cut")
	void SetCutToolMesh(UDynamicMeshComponent* ToolMeshComp)
	{
		CutToolMeshComponent = ToolMeshComp;
	}

	// 开始/停止切削
	UFUNCTION(BlueprintCallable, Category = "Voxel Cut")
	void StartCutting();
    
	UFUNCTION(BlueprintCallable, Category = "Voxel Cut")
	void StopCutting();

	// 切削参数
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Cut")
	bool bSmoothEdges = true;
    
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Cut")
	float MarchingCubeSize = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Cut")
	int32 MaxOctreeDepth = 6;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Cut")
	float MinVoxelSize = 0.5f;
    
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Cut")
	float SmoothingStrength = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Cut")
	int32 SmoothingIteration = 2;
    
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Cut")
	bool bFillHoles = true;
    
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Cut")
	float UpdateThreshold = 1.0f;

	// 切削状态
	UFUNCTION(BlueprintCallable, Category = "Voxel Cut")
	bool IsCutting() const { return bIsCutting; }
	
	// 获取切削结果网格
	UFUNCTION(BlueprintCallable, Category = "Voxel Cut")
	UDynamicMeshComponent* GetResultMesh() const { return TargetMeshComponent; }

	// 初始化切削系统
	void InitializeCutSystem();
	
protected:
	// Called when the game starts
	virtual void BeginPlay() override;

	

public:
	// Called every frame
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

private:
	
	// 目标网格
	UPROPERTY()
	UDynamicMeshComponent* TargetMeshComponent;

	// 工具网格
	UPROPERTY()
	UDynamicMeshComponent* CutToolMeshComponent;

	// 可重用的切削操作器
	TSharedPtr<FVoxelCutMeshOp> CutOp;
	
	// 状态管理
	ECutState CutState;
	std::atomic<bool> bIsCutting;      // 用户是否在切削模式
    
	// 工具位置跟踪
	FVector LastToolPosition;
	FRotator LastToolRotation;
	float DistanceSinceLastUpdate;
    
	// 当前请求的数据
	FTransform CurrentToolTransform;
	TSharedPtr<FDynamicMesh3> CurrentToolMesh;
    
	// 线程同步
	FCriticalSection StateLock;
    


	bool bSystemInitialized = false;
    
	// 检查是否需要更新切削
	bool NeedsCutUpdate(const FTransform& InCurrentToolTransform);
    
	// 状态机处理
	void UpdateStateMachine();
    
	// 请求切削
	void RequestCut(const FTransform& ToolTransform);
    
	// 开始异步切削
	void StartAsyncCut();
    
	// 切削完成回调
	void OnCutComplete(FDynamicMesh3* ResultMesh);
    
	// 复制工具网格（轻量级操作）
	TSharedPtr<FDynamicMesh3> CopyToolMesh();

	TSharedPtr<FToolSDFGenerator> ToolSDFGenerator;

// Debug 相关信息
	
	/** 调试框信息 */
	struct FDebugBoxInfo
	{
		FVector Center;
		FVector Extent;
		FColor Color;
	};
	
	TArray<FDebugBoxInfo> DebugBoxes;
	
	void VisualizeOctreeNode();

	void VisualizeOctreeNodeRecursive(const FOctreeNode& Node, int32 Depth);

	void ClearOctreeVisualization();

	void PrintOctreeDetails();
	void PrintOctreeNodeRecursive(const FOctreeNode& Node, int32 Depth, int32& NodeCount, int32& LeafCount, int32& TotalVoxels);
};
