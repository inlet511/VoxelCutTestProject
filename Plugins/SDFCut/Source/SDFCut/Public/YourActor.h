// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "YourActor.generated.h"

UCLASS()
class SDFCUT_API AYourActor : public AActor
{
	GENERATED_BODY()

public:
	// Sets default values for this actor's properties
	AYourActor();
	// 1. 源贴图 (在编辑器中赋值)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	class UTexture2D* SourceTexture;

	// 2. 基础材质 (必须包含一个 Texture Parameter)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	class UMaterialInterface* BaseMaterial;

	// 3. 静态网格组件
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rendering")
	class AStaticMeshActor* MyMeshActor;

	// 4. 参数名称 (材质中纹理参数的名字)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	FName TextureParameterName = FName("BaseColor");

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

	// 保存 RenderTarget 的引用，防止被 GC 回收，确保画面持久
	UPROPERTY()
	class UTextureRenderTarget2D* MyRenderTarget;

	// 保存动态材质实例的引用
	UPROPERTY()
	class UMaterialInstanceDynamic* MyDynMaterial;

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;
};
