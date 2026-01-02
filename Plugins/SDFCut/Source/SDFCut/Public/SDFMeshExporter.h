// SDFMeshExporter.h
// Utility class for extracting meshes from SDF volumes and exporting to OBJ format.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"

/**
 * Utility class for extracting meshes from SDF volumes and exporting to OBJ format.
 * Designed for runtime use - no editor dependencies.
 */
class SDFCUT_API FSDFMeshExporter
{
public:
	/**
	 * Configuration for marching cubes mesh extraction
	 */
	struct FMarchingCubesConfig
	{
		// Iso-surface level (typically 0.0 for SDF)
		float IsoValue = 0.0f;

		// Size of each marching cube cell. Smaller = more detail, slower.
		// If <= 0, will use VoxelSize from provider
		float CubeSize = -1.0f;

		// Use parallel computation (recommended for large volumes)
		bool bParallelCompute = true;
	};

	/**
	 * Configuration for OBJ export
	 */
	struct FOBJExportConfig
	{
		// Include vertex normals in OBJ (computed from mesh)
		bool bIncludeNormals = true;

		// Include vertex colors (material IDs encoded as colors)
		bool bIncludeVertexColors = true;

		// Reverse winding order (UE uses left-handed, OBJ typically right-handed)
		bool bReverseWinding = true;
	};

	/**
	 * Extract mesh from SDF volume using Marching Cubes algorithm
	 *
	 * @param SDFData         Raw SDF data array (R=distance, G=materialID)
	 * @param Dimensions      3D dimensions of the SDF volume
	 * @param VoxelSize       Size of each voxel in local units
	 * @param LocalBounds     Local space bounds of the volume
	 * @param Config          Marching cubes configuration
	 * @param OutMesh         Output dynamic mesh
	 * @param OutMaterialIDs  Output per-vertex material IDs (optional)
	 * @return True if extraction succeeded
	 */
	static bool ExtractMeshFromSDF(
		const TArray<FFloat16Color>& SDFData,
		const FIntVector& Dimensions,
		float VoxelSize,
		const FBox& LocalBounds,
		const FMarchingCubesConfig& Config,
		UE::Geometry::FDynamicMesh3& OutMesh,
		TArray<int32>* OutMaterialIDs = nullptr
	);

	/**
	 * Export FDynamicMesh3 to OBJ format string
	 *
	 * @param Mesh            Input mesh to export
	 * @param Config          Export configuration
	 * @param VertexColors    Optional per-vertex colors (e.g., from material IDs)
	 * @return OBJ format string
	 */
	static FString MeshToOBJString(
		const UE::Geometry::FDynamicMesh3& Mesh,
		const FOBJExportConfig& Config,
		const TArray<FLinearColor>* VertexColors = nullptr
	);

	/**
	 * Write mesh directly to OBJ file
	 *
	 * @param Mesh            Input mesh
	 * @param FilePath        Output file path
	 * @param Config          Export configuration
	 * @param VertexColors    Optional per-vertex colors
	 * @return True if file was written successfully
	 */
	static bool WriteMeshToOBJ(
		const UE::Geometry::FDynamicMesh3& Mesh,
		const FString& FilePath,
		const FOBJExportConfig& Config,
		const TArray<FLinearColor>* VertexColors = nullptr
	);

	/**
	 * Convert material IDs to vertex colors for visualization
	 *
	 * @param MaterialIDs     Per-vertex material IDs
	 * @return Array of linear colors
	 */
	static TArray<FLinearColor> MaterialIDsToColors(const TArray<int32>& MaterialIDs);

private:
	// Helper: Sample SDF value at a position from the data array (trilinear interpolation)
	static float SampleSDFValue(
		const TArray<FFloat16Color>& SDFData,
		const FIntVector& Dimensions,
		const FVector& VoxelCoord
	);

	// Helper: Get voxel index from 3D coordinates
	static int32 GetVoxelIndex(int32 X, int32 Y, int32 Z, const FIntVector& Dimensions);
};
