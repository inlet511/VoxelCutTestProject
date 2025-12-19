// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/VolumeTexture.h"
#include "SDFGenLibrary.generated.h"

/**
 * 
 */
UCLASS()
class SDFCUTEDITOR_API USDFGenLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/**
	 * 将 StaticMesh 转换为 SDF Volume Texture 并保存
	 * @param InputMesh - 输入的静态网格
	 * @param PackagePath - 保存路径 (例如 "/Game/Textures/")
	 * @param AssetName - 资源名称
	 * @param ResolutionXY - X和Y轴的分辨率
	 * @param Slices - Z轴的分辨率 (Slices数量)
	 * @param BoundsScale - 包围盒缩放系数 (防止SDF在边界被截断，建议 1.1 或 1.2)
	 */
	UFUNCTION(BlueprintCallable, Category = "SDF Tools")
	static UVolumeTexture* GenerateSDFFromStaticMesh(
		UStaticMesh* InputMesh, 
		FString PackagePath, 
		FString AssetName, 
		int32 ResolutionXY = 128, 
		int32 Slices = 128,
		float BoundsScale = 1.1f,
		int32 MaterialID = 0,
		bool bGenerate2D = false
	);
};
