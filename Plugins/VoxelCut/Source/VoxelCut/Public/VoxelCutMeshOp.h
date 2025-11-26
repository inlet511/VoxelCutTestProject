// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ModelingOperators.h"
#include "BaseOps/VoxelBaseOp.h"
#include "MaVoxelData.h"
#include "ToolSDFGenerator.h"

namespace UE
{
	namespace Geometry
	{		
		class VOXELCUT_API FVoxelCutMeshOp  : public FVoxelBaseOp
		{
		public:
			virtual ~FVoxelCutMeshOp() {}

			// 输入：目标网格和刀具网格
			TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> TargetMesh;
			TSharedPtr<const FDynamicMesh3, ESPMode::ThreadSafe> CutToolMesh;
			TSharedPtr<FToolSDFGenerator> ToolSDF;
			
			// 变换矩阵
			FTransform TargetTransform;
			FTransform CutToolTransform;
			
    
			// 持久化体素数据（输入/输出）
			TSharedPtr<FMaVoxelData> PersistentVoxelData;
    
			// 切削参数
			double CutOffset = 0.0;
			bool bFillCutHole = true;
			bool bKeepBothParts = false;
			double MarchingCubeSize = 2.0;
			int32 MaxOctreeDepth = 6;
			double MinVoxelSize = 0.5;			
			bool bSmoothCutEdges = true;
			int32 SmoothingIteration = 0;
			double SmoothingStrength = 0.6;
    
			// 增量更新选项
			int32 UpdateMargin = 2;          // 更新边界扩展（体素单位）

			void SetTransform(const FTransformSRT3d& Transform);

			virtual void CalculateResult(FProgressCancel* Progress) override;
    
			// 初始化体素数据（首次使用）
			bool InitializeVoxelData(FProgressCancel* Progress);
    
			// 增量切削（基于现有体素数据）
			bool IncrementalCut(FProgressCancel* Progress);

			FDynamicMesh3* GetResultMesh() const
			{
				return ResultMesh.Get();
			}

		protected:
			// 体素化方法
			bool VoxelizeMesh(const FDynamicMesh3& Mesh, const FTransform& Transform, 
							 FMaVoxelData& VoxelData, FProgressCancel* Progress);
    
			// 局部更新：只更新受刀具影响的区域
			void UpdateLocalRegion(FMaVoxelData& TargetVoxels, const FDynamicMesh3& ToolMesh, 
								  const FTransform& ToolTransform, FProgressCancel* Progress);
    
			// 网格生成
			void ConvertVoxelsToMesh(const FMaVoxelData& Voxels, FProgressCancel* Progress);
    
		private:
			// 内部状态
			bool bVoxelDataInitialized = false;

			// 平滑模型
			void SmoothGeneratedMesh(FDynamicMesh3& Mesh, int32 Iterations);

		};
	}
}