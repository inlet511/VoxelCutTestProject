// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "SDFVolumeProvider.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Async/ParallelFor.h"
#include "StaticMeshResources.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/StaticMeshVertexBuffer.h" 
#include "HapticProbeComponent.generated.h"

// 辅助结构体：仅存储用于决策的几何信息
struct FGeoSampleData
{
	FVector Gradient; // SDF梯度 (法线)
	float Depth;      // SDF深度
	bool bIsValid;    // 是否命中
};


UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class SDFCUTHAPTIC_API UHapticProbeComponent : public USceneComponent
{
	GENERATED_BODY()

public: 
	// Sets default values for this component's properties
	UHapticProbeComponent();
	
	// 挂有 GPUSDFCutter的Acor
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Haptics")
	AActor* CutterActor;	
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haptics", meta=(UseComponentPicker, AllowedClasses="/Script/Engine.StaticMeshComponent"))
	FComponentReference VisualMeshRef;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haptics", meta=(UseComponentPicker, AllowedClasses="/Script/Engine.SceneMeshComponent"))
	FComponentReference RayStartPointRef;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Haptics")
	float SamplingDensity = 1.0f;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Haptics")
	float SamplingMinSpacing = 0.75f;

	
	// --- 物理参数 ---
	UPROPERTY(EditAnywhere, Category = "Haptics")
	float BaseStiffness = 1.0f;
	
	// 计算反馈力 (可以在 Haptic 线程调用)
	// 返回 true 如果产生了碰撞
	UFUNCTION(BlueprintCallable, Category = "Haptics")
	bool CalculateForce(FVector& OutForce, FVector& OutTorque);

	/**
	 * 使用球体追踪 (Sphere Tracing) 沿射线寻找 SDF 表面，在Calculate Force内部调用(假设已经有了读取锁)
	 * @param StartPoint 射线的起始点 (通常是上一帧的安全位置，或设备物理手柄的位置)
	 * @param EndPoint 射线的终点 (当前探针陷入内部的位置)
	 * @param OutSurfacePoint 如果找到，输出表面接触点的世界坐标
	 * @return 是否成功找到表面
	 */
	bool FindSurfacePointFromRay(const FVector& StartPoint, const FVector& EndPoint, FVector& OutSurfacePoint);
	
	/**
	 * 根据静态网格更新探针形状
	 * @param NewMesh        要采样的网格
	 * @param PointDensity   采样密度 (点数 / 平方厘米)，建议值 10~50
	 */
	UFUNCTION(BlueprintCallable, Category = "Haptics")
	void UpdateProbeMesh();
	
	// 调试用的属性----------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haptics|Debug")
	bool bVisualizeSamplePoints = false;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haptics|Debug")
	bool bLogCalcTime = false;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haptics|Debug")
	bool bVisualizeForce = false;

	/** 调试点的尺寸 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haptics|Debug", meta=(EditCondition="bVisualizeSamplePoints"))
	float DebugPointSize = 5.0f;
	
protected:
	// Called when the game starts
	virtual void BeginPlay() override;
	
	// 设置目标 SDF Volume Provider
	void SetSDFVolumeProvider();
	
	
	/**
	 * @param Mesh          目标网格资源
	 * @param Density       采样密度 (点/平方厘米)。例如 0.01 代表每 100cm² 一个点。
	 * @param MinSpacing    最小间距系数 (0.0~1.0)。值越大分布越均匀，但如果太大可能导致点数不足。推荐 0.75
	 */
	void GenerateUniformSurfacePoints(const UStaticMesh* Mesh, float Density);
	
	
	// 辅助：在三角形ABC内部生成一个随机点
	FVector GetRandomPointInTriangle(const FVector& A, const FVector& B, const FVector& C);	
	
	// --- 依赖 ---
	ISDFVolumeProvider* SDFProvider = nullptr;
	

	// 材质刚度系数表 (对应 G 通道的 ID)
	UPROPERTY(VisibleAnywhere, Category = "Haptics|Material")
	TMap<int32, float> MaterialStiffnessScales;

	// --- 内部数据 ---
	// 采样点 (相对于组件的局部坐标)
	TArray<FVector> LocalSamplePoints;

public:
	// Called every frame
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	
private:
	// 探针的MeshComponent
	UStaticMeshComponent* ProbeMeshComp;

	// 探测射线起点组件
	USceneComponent* RayStart;
};
