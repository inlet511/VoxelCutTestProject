// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "SDFVolumeProvider.h"
#include "HapticProbeComponent.generated.h"


UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class PHYSICSTEST_API UHapticProbeComponent : public USceneComponent
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
};
