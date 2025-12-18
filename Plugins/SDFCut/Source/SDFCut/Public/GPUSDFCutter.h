// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMeshActor.h"
#include "Components/SceneComponent.h"
#include "Engine/TextureRenderTargetVolume.h"
#include "GPUSDFCutter.generated.h"


class UVolumeTexture;
class AStaticMeshActor;

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class SDFCUT_API UGPUSDFCutter : public USceneComponent
{
	GENERATED_BODY()

public:
	// Sets default values for this component's properties
	UGPUSDFCutter();

	// 初始化SDF纹理
	UFUNCTION(BlueprintCallable, Category = "GPU SDF Cutter")
	void InitResources();

	// 更新切削工具Transform
	UFUNCTION(BlueprintCallable, Category = "GPU SDF Cutter")
	void UpdateToolTransform();

	// 更新切削对象Transform（可选，如果物体也在移动）
	UFUNCTION(BlueprintCallable, Category = "GPU SDF Cutter")
	void UpdateTargetTransform();
	
	UFUNCTION(BlueprintCallable, Category = "GPU SDF Cutter")
	bool GetSDFValueAndNormal(FVector WorldLocation, float& OutSDFValue, FVector& OutNormal);

	// SDF纹理（CPU端引用）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Textures")
	UVolumeTexture* OriginalSDFTexture = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Textures")
	UVolumeTexture* ToolSDFTexture = nullptr;

	UPROPERTY()
	UTextureRenderTargetVolume* VolumeRT = nullptr;

	// 目标网格组件（用于渲染）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU SDF Cutter")
	AStaticMeshActor* TargetMeshActor = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU SDF Cutter")
	AStaticMeshActor* CutToolActor = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "GPU SDF Cutter")
	UMaterialInterface* SDFMaterialInstance = nullptr;
	
	UPROPERTY()
	UMaterialInstanceDynamic* SDFMaterialInstanceDynamic;

protected:
	// Called when the game starts
	virtual void BeginPlay() override;

public:
	// Called every frame
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction) override;

private:

	// 局部更新调度
	void DispatchLocalUpdate();

	// 工具在切削对象空间的Local AABB
	void CalculateToolAABBInTargetSpace(const FTransform& ToolTransform, FIntVector& OutVoxelMin, FIntVector& OutVoxelMax);

	// 当前状态
	FTransform CurrentTargetTransform;
	FTransform CurrentToolTransform;

	// 工具和切削对象的相对Transform已更新
	bool bRelativeTransformDirty = false;
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

	FTextureRHIRef OriginalSDFRHIRef;
	FTextureRHIRef ToolSDFRHIRef;
	FTextureRHIRef VolumeRTRHIRef;

	// 初始化CPU端缓存
	void InitCPUData();
    
	void UpdateCPUDataPartial(FIntVector UpdateMin, FIntVector UpdateSize, TArray<float>& LocalData);

	// 辅助：三线性插值采样
	float SampleSDFTrilinear(const FVector& VoxelCoord) const;
	// 辅助：获取体素索引
	int32 GetVoxelIndex(int32 X, int32 Y, int32 Z) const;

	// CPU端缓存的SDF数据 (线性数组: Z * Y * X)
	TArray<float> CPU_SDFData;
    
	// 标记是否正在回读，防止重入
	bool bIsReadingBack = false;

};
