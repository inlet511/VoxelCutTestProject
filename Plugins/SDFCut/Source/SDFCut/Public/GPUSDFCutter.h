// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "BoxTypes.h"
#include "Components/SceneComponent.h"
#include "GPUSDFCutter.generated.h"

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnMeshGenerated, UDynamicMeshComponent*, MeshComponent);

class UVolumeTexture;
class UDynamicMeshComponent;

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class SDFCUT_API UGPUSDFCutter : public USceneComponent
{
	GENERATED_BODY()

public:
	// Sets default values for this component's properties
	UGPUSDFCutter();
	
	// 初始化SDF纹理（原始物体和切削工具）
	UFUNCTION(BlueprintCallable, Category = "GPU SDF Cutter")
	void InitSDFTextures(UVolumeTexture* InOriginalSDF, UVolumeTexture* InToolSDF, const UE::Geometry::FAxisAlignedBox3d& InOriginalBounds, float InCubeSize = 5.0f);

	// 每帧更新切削工具Transform
	UFUNCTION(BlueprintCallable, Category = "GPU SDF Cutter")
	void UpdateToolTransform(const FTransform& InToolTransform);

	// 设置网格组件（用于渲染生成的模型）
	UFUNCTION(BlueprintCallable, Category = "GPU SDF Cutter")
	void SetTargetMeshComponent(UDynamicMeshComponent* InTargetMeshComponent);

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
	// 初始化GPU资源（纹理、缓冲区、Render Graph）
	void InitGPUResources();

	// 构建Render Graph，调度两个Compute Shader
	void DispatchRenderGraph();

	// 回读GPU生成的网格数据
	void ReadbackMeshData(TRDGBufferRef<FVector> VertexBuffer, TRDGBufferRef<FIntVector> TriangleBuffer, int32 NumVertices, int32 NumTriangles);

	// 用回读的数据更新DynamicMeshComponent
	void UpdateDynamicMesh(const TArray<FVector>& Vertices, const TArray<FIntVector>& Triangles);

	// 计算工具在原始物体空间的AABB（用于优化SDF更新范围）
	UE::Geometry::FAxisAlignedBox3d ComputeToolAABBInOriginalSpace(const FTransform& ToolTransform);

	// SDF纹理（CPU端引用）
	UPROPERTY()
	UVolumeTexture* OriginalSDFTexture = nullptr;
	UPROPERTY()
	UVolumeTexture* ToolSDFTexture = nullptr;

	// 目标网格组件（用于渲染）
	UPROPERTY()
	UDynamicMeshComponent* TargetMeshComponent = nullptr;

	// 原始物体的边界和体素尺寸
	UE::Geometry::FAxisAlignedBox3d OriginalBounds;
	float CubeSize = 5.0f;
	FIntVector DynamicSDFDimensions = FIntVector(64, 64, 64); // 动态SDF分辨率（可调整）

	// 切削工具当前Transform
	FTransform CurrentToolTransform;
	bool bToolTransformUpdated = false; // 标记Transform是否更新

	// GPU资源
	FTextureRHIRef OriginalSDFRHIRef;
	FTextureRHIRef ToolSDFRHIRef;
	FTextureRHIRef DynamicSDFRHIRef; // 动态SDF纹理（RWTexture3D）


	// 双缓冲：避免回读阻塞，当前帧渲染上一帧的网格
	TArray<FVector> ReadbackVertices[2];
	TArray<FIntVector> ReadbackTriangles[2];
	int32 CurrentBufferIndex = 0;
	bool bHasValidMeshData = false;

	// Render Graph相关
	bool bGPUResourcesInitialized = false;
	FGraphEventRef LastRenderGraphEvent;
};
