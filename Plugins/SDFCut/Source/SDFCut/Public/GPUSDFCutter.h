// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMeshActor.h"
#include "Components/SceneComponent.h"
#include "GPUSDFCutter.generated.h"

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnMeshGenerated, UDynamicMeshComponent*, Component);

class UVolumeTexture;
class UDynamicMeshComponent;

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class SDFCUT_API UGPUSDFCutter : public USceneComponent
{
	GENERATED_BODY()

public:
	// Sets default values for this component's properties
	UGPUSDFCutter();

	// 初始化SDF纹理
	UFUNCTION(BlueprintCallable, Category = "GPU SDF Cutter")
	void InitSDFTextures(
		UVolumeTexture* InOriginalSDF,
		UVolumeTexture* InToolSDF,
		const FBox& TargetLocalBox,
		float InVoxelSize = 5.0f);

	// 每帧更新切削工具Transform
	UFUNCTION(BlueprintCallable, Category = "GPU SDF Cutter")
	void UpdateToolTransform(const FTransform& InToolTransform);

	// 更新物体Transform（可选，如果物体也在移动）
	UFUNCTION(BlueprintCallable, Category = "GPU SDF Cutter")
	void UpdateObjectTransform(const FTransform& InObjectTransform);
	
	// SDF纹理（CPU端引用）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Textures")
	UVolumeTexture* OriginalSDFTexture = nullptr;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Textures")
	UVolumeTexture* ToolSDFTexture = nullptr;

	// 目标网格组件（用于渲染）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU SDF Cutter")
	ADynamicMeshActor* TargetMeshActor = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU SDF Cutter")
	ADynamicMeshActor* CutToolActor = nullptr;
	
	// 网格生成完成回调
	FOnMeshGenerated OnMeshGenerated;

protected:
	// Called when the game starts
	virtual void BeginPlay() override;

public:
	// Called every frame
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	// GPU资源初始化
	void InitGPUResources();
    
	// 局部更新调度
	void DispatchLocalUpdate(TFunction<void(const TArray<FVector>& Vertices, const TArray<FIntVector>& Triangles)> AsyncCallback);
    
	// 工具在切削对象空间的Local AABB
	void CalculateToolAABBInTargetSpace(const FTransform& ToolTransform, FIntVector& OutVoxelMin, FIntVector& OutVoxelMax);
    
	// 数据回读和网格更新
	void ReadbackMeshData(const TArray<FVector>& Vertices, const TArray<FIntVector>& Triangles);
	void UpdateDynamicMesh(const TArray<FVector>& Vertices, const TArray<FIntVector>& Triangles);

	// 当前状态
	FTransform CurrentObjectTransform;
	FTransform CurrentToolTransform;
	bool bToolTransformDirty = false;
	bool bGPUResourcesInitialized = false;

	// SDF参数
	FBox TargetLocalBounds; // 物体原始边界
	float VoxelSize = 5.0f;
	FIntVector SDFDimensions; // 

	// 工具尺寸信息
	FVector ToolOriginalSize; // 工具的原始尺寸（世界单位）
	FBox ToolLocalBounds;     // 工具的本地空间边界
	FIntVector ToolSDFDimensions; // 工具SDF纹理的尺寸

	// 计算切割工具尺寸信息
	void CalculateToolDimensions();
	FTransform GetToolWorldTransform() const;

	// GPU资源
	TRefCountPtr<IPooledRenderTarget> DynamicSDFTexturePooled;

	FTextureRHIRef OriginalSDFRHIRef;
	FTextureRHIRef ToolSDFRHIRef;

	// 双缓冲网格数据
	TArray<FVector> MeshVertices[2];
	TArray<FIntVector> MeshTriangles[2];
	int32 CurrentMeshBuffer = 0;
	bool bHasPendingMeshData = false;

};
