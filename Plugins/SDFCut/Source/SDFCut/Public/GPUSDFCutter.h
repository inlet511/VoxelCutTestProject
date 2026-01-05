// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "SDFVolumeProvider.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/TextureRenderTargetVolume.h"
#include "SDFVolumeProvider.h"
#include "RHIResources.h"
#include "RenderGraphFwd.h"
#include <atomic>
#include "GPUSDFCutter.generated.h"


class UVolumeTexture;
class AStaticMeshActor;

// 牙齿材质类型
enum EVolumeMaterial
{
	Enamel=0, // 牙釉质
	Dentin=1, // 牙本质
	Caries=2, // 龋坏部分
	Fill=3   //  金属填充物
};

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class SDFCUT_API UGPUSDFCutter : public USceneComponent, public ISDFVolumeProvider
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
	bool GetSDFValueAndNormal(FVector WorldLocation, float& OutSDFValue, FVector& OutNormal, int32& OutMaterialID);

	UFUNCTION(BlueprintCallable, Category = "SDF")
	float CalculateCurrentVolume(int32 MaterialID, bool bWorldSpace = true);

	/**
	 * Export current SDF volume to OBJ mesh file.
	 * Uses Marching Cubes algorithm to extract iso-surface at SDF=0.
	 *
	 * @param FilePath      Full path to output OBJ file
	 * @param bIncludeNormals  Include vertex normals in export
	 * @param bIncludeMaterialColors  Encode material IDs as vertex colors
	 * @return True if export succeeded
	 */
	UFUNCTION(BlueprintCallable, Category = "GPU SDF Cutter|Export")
	bool ExportToOBJ(
		const FString& FilePath,
		bool bIncludeNormals = true,
		bool bIncludeMaterialColors = true
	);

	/**
	 * Export current SDF volume to OBJ mesh file with custom cube size.
	 * Smaller cube size = more detail but slower export.
	 *
	 * @param FilePath      Full path to output OBJ file
	 * @param CubeSize      Size of marching cubes cells (0 = use voxel size)
	 * @param bIncludeNormals  Include vertex normals in export
	 * @param bIncludeMaterialColors  Encode material IDs as vertex colors
	 * @return True if export succeeded
	 */
	UFUNCTION(BlueprintCallable, Category = "GPU SDF Cutter|Export")
	bool ExportToOBJWithDetail(
		const FString& FilePath,
		float CubeSize = 0.0f,
		bool bIncludeNormals = true,
		bool bIncludeMaterialColors = true
	);

	/**
	 * Extract mesh from current SDF data (for further processing).
	 * Returns mesh in local space of the target mesh component.
	 *
	 * @param OutVertices    Output vertex positions
	 * @param OutTriangles   Output triangle indices (3 indices per triangle)
	 * @param OutNormals     Output vertex normals
	 * @param OutMaterialIDs Output per-vertex material IDs
	 * @param CubeSize       Size of marching cubes cells (0 = use voxel size)
	 * @return True if extraction succeeded
	 */
	UFUNCTION(BlueprintCallable, Category = "GPU SDF Cutter|Export")
	bool ExtractMesh(
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<int32>& OutMaterialIDs,
		float CubeSize = 0.0f
	);
	
	// SDF纹理（CPU端引用）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Textures")
	UVolumeTexture* OriginalSDFTexture = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Textures")
	UVolumeTexture* ToolSDFTexture = nullptr;

	UPROPERTY()
	UTextureRenderTargetVolume* VolumeRT = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU SDF Cutter")
	AActor* TargetMeshActor = nullptr;
	
	UPROPERTY(EditInstanceOnly, Category = "GPU SDF Cutter")
	FName TargetComponentTag; 

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GPU SDF Cutter")
	AActor* CutToolActor = nullptr;
	
	UPROPERTY(EditInstanceOnly, Category = "GPU SDF Cutter")
	FName CutToolComponentTag; 	


	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "GPU SDF Cutter")
	UMaterialInterface* SDFMaterialInstance = nullptr;
	
	UPROPERTY()
	class UMaterialInstanceDynamic* SDFMaterialInstanceDynamic;


	// --- ISDFVolumeProvider 实现 ---
	virtual bool WorldToVoxelSpace(const FVector& WorldLocation, FVector& OutVoxelCoord) const override;
	virtual float SampleSDF(const FVector& VoxelCoord) const override; // 包装原有的 SampleSDFTrilinear
	virtual int32 SampleMaterialID(const FVector& VoxelCoord) const override;
	virtual float GetVoxelSize() const override { return VoxelSize; }
	virtual FRWLock& GetDataLock() override { return DataRWLock; }
private:
	// 读写锁，防止切削回读时，Haptics正在读取导致崩溃
	FRWLock DataRWLock;

	void FindReferenceComponents();
	UStaticMeshComponent* TargetMeshComponent = nullptr;	
	UStaticMeshComponent* CutToolComponent = nullptr;

	
protected:
	// Called when the game starts
	virtual void BeginPlay() override;
	

	// 触觉参数
	float ProbeStiffness = 1000.0f; // 基础刚度 N/cm
	float ProbeDamping = 5.0f;      // 阻尼（可选，需要上一帧速度）
	

	// 辅助：快速计算梯度的函数（比通用的GetSDFValueAndNormal更快，去掉了边界检查等）
	FVector CalculateGradientAtVoxel(const FVector& VoxelCoord) const;

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
    
	void UpdateCPUDataPartial(FIntVector UpdateMin, FIntVector UpdateSize, TArray<FFloat16Color>& LocalData);
	
	// 辅助：获取体素索引
	int32 GetVoxelIndex(int32 X, int32 Y, int32 Z) const;

	// CPU端缓存的SDF数据 (线性数组: Z * Y * X)
	TArray<FFloat16Color> CPU_SDFData;
    
	// 标记是否正在回读，防止重入
	std::atomic<bool> bIsReadingBack{false};

	// 标记是否需要延迟执行初始纹理复制（子关卡加载时 RHI 资源可能未就绪）
	std::atomic<bool> bPendingInitialCopy{false};

	// 标记是否需要延迟初始化（等待资源就绪）
	bool bPendingResourceInit = false;

	// 执行初始纹理复制
	void ExecuteInitialTextureCopy();
};
