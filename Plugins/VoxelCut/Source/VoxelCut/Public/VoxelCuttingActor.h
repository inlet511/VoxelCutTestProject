// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelCutComponent.h"
#include "DynamicMeshActor.h"
#include "VoxelCuttingActor.generated.h"

UCLASS()
class VOXELCUT_API AVoxelCuttingActor : public AActor
{
	GENERATED_BODY()

public:
	// Sets default values for this actor's properties
	AVoxelCuttingActor();

	// 切削组件
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Cut")
	UVoxelCutComponent* VoxelCutComponent;

	UPROPERTY(EditAnywhere,BlueprintReadWrite, Category = "Voxel Cut")
	ADynamicMeshActor* TargetActor;

	UPROPERTY(EditAnywhere,BlueprintReadWrite, Category = "Voxel Cut")
	ADynamicMeshActor* ToolActor;

	// 开始/停止切削
	UFUNCTION(BlueprintCallable, Category = "Voxel Cut")
	void StartCutting();
    
	UFUNCTION(BlueprintCallable, Category = "Voxel Cut")
	void StopCutting();
	
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	
protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;

private:
	
	// 切削工具网格组件
	UPROPERTY()
	UDynamicMeshComponent* CutToolMeshComponent;
    
	// 切削对象网格组件
	UPROPERTY()
	UDynamicMeshComponent* TargetMeshComponent;

	void SetupComponents();
	void CheckComponents();
};
