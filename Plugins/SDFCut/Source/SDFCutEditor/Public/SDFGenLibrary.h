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
	
	/**
   * 将 BrushActor 的形状“烘焙”到 VolumeTexture 的 G 通道中
   * @param TargetTexture   目标体积纹理
   * @param VolumeActor     场景中承载该纹理的 Actor (用于确定体积的世界坐标范围)
   * @param BrushActor      作为笔刷的 Actor (必须有碰撞体，且是封闭模型)
   * @param MaterialID      要写入的材质 ID
   * @param bErase          如果是 true，则写入 0 (或者擦除)
   */
	UFUNCTION(BlueprintCallable, Category = "Volume Tools", meta = (WorldContext = "WorldContextObject"))
	static void BakeBrushToVolume(UObject* WorldContextObject, UVolumeTexture* TargetTexture, AActor* VolumeActor, AActor* BrushActor, int32 MaterialID, bool bErase);
	
	UFUNCTION(BlueprintCallable, Category = "Volume Texture Tools")
	static void FillVolumeTextureGChannel(UVolumeTexture* TargetTexture, float FillValue);
};
