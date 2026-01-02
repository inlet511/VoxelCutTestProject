// SDFMeshExporter.cpp

#include "SDFMeshExporter.h"
#include "Generators/MarchingCubes.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/MeshNormals.h"
#include "Misc/FileHelper.h"
#include "Async/ParallelFor.h"


using namespace UE::Geometry;

bool FSDFMeshExporter::ExtractMeshFromSDF(
	const TArray<FFloat16Color>& SDFData,
	const FIntVector& Dimensions,
	float VoxelSize,
	const FBox& LocalBounds,
	const FMarchingCubesConfig& Config,
	FDynamicMesh3& OutMesh,
	TArray<int32>* OutMaterialIDs)
{
	if (SDFData.Num() == 0 || Dimensions.X <= 0 || Dimensions.Y <= 0 || Dimensions.Z <= 0)
	{
		return false;
	}

	// Configure marching cubes
	FMarchingCubes MarchingCubes;

	// Set bounds from local bounds
	MarchingCubes.Bounds = TAxisAlignedBox3<double>(
		FVector3d(LocalBounds.Min),
		FVector3d(LocalBounds.Max)
	);

	// Set cube size
	float ActualCubeSize = Config.CubeSize > 0 ? Config.CubeSize : VoxelSize;
	MarchingCubes.CubeSize = static_cast<double>(ActualCubeSize);

	// Set iso value
	MarchingCubes.IsoValue = static_cast<double>(Config.IsoValue);

	// Set computation options
	MarchingCubes.bParallelCompute = Config.bParallelCompute;

	// Capture SDF data for the implicit function
	const TArray<FFloat16Color>* SDFDataPtr = &SDFData;
	const FIntVector* DimensionsPtr = &Dimensions;
	const FBox* BoundsPtr = &LocalBounds;
	float LocalVoxelSize = VoxelSize;

	// Define the implicit function
	// FMarchingCubes expects: TFunction<double(FVector3d)>
	// Position is in local space, need to convert to voxel space
	MarchingCubes.Implicit = [SDFDataPtr, DimensionsPtr, BoundsPtr, LocalVoxelSize](FVector3d LocalPos) -> double
	{
		// Convert local position to voxel coordinates
		FVector RelativePos = FVector(LocalPos.X, LocalPos.Y, LocalPos.Z) - BoundsPtr->Min;
		FVector VoxelCoord = RelativePos / LocalVoxelSize;

		// Sample SDF using trilinear interpolation
		return static_cast<double>(SampleSDFValue(*SDFDataPtr, *DimensionsPtr, VoxelCoord));
	};

	// Generate the mesh
	MarchingCubes.Generate();

	// Copy to output mesh
	OutMesh.Clear();
	OutMesh.EnableVertexNormals(FVector3f::UpVector);

	// Add vertices
	for (int32 i = 0; i < MarchingCubes.Vertices.Num(); ++i)
	{
		OutMesh.AppendVertex(MarchingCubes.Vertices[i]);
	}

	// Add triangles
	for (int32 i = 0; i < MarchingCubes.Triangles.Num(); ++i)
	{
		OutMesh.AppendTriangle(
			MarchingCubes.Triangles[i].A,
			MarchingCubes.Triangles[i].B,
			MarchingCubes.Triangles[i].C
		);
	}

	// Compute normals
	FMeshNormals::QuickComputeVertexNormals(OutMesh);

	// Extract material IDs for each vertex if requested
	if (OutMaterialIDs)
	{
		OutMaterialIDs->SetNum(OutMesh.MaxVertexID());

		ParallelFor(OutMesh.MaxVertexID(), [&](int32 VertexID)
		{
			if (!OutMesh.IsVertex(VertexID))
			{
				(*OutMaterialIDs)[VertexID] = 0;
				return;
			}

			FVector3d VertexPos = OutMesh.GetVertex(VertexID);

			// Convert back to voxel space
			FVector RelativePos = FVector(VertexPos.X, VertexPos.Y, VertexPos.Z) - LocalBounds.Min;
			FVector VoxelCoord = RelativePos / VoxelSize;

			// Sample material ID (nearest neighbor)
			int32 X = FMath::RoundToInt(VoxelCoord.X);
			int32 Y = FMath::RoundToInt(VoxelCoord.Y);
			int32 Z = FMath::RoundToInt(VoxelCoord.Z);

			X = FMath::Clamp(X, 0, Dimensions.X - 1);
			Y = FMath::Clamp(Y, 0, Dimensions.Y - 1);
			Z = FMath::Clamp(Z, 0, Dimensions.Z - 1);

			int32 Index = GetVoxelIndex(X, Y, Z, Dimensions);
			if (SDFData.IsValidIndex(Index))
			{
				(*OutMaterialIDs)[VertexID] = FMath::RoundToInt(SDFData[Index].G.GetFloat());
			}
			else
			{
				(*OutMaterialIDs)[VertexID] = 0;
			}
		});
	}

	return OutMesh.TriangleCount() > 0;
}

float FSDFMeshExporter::SampleSDFValue(
	const TArray<FFloat16Color>& SDFData,
	const FIntVector& Dimensions,
	const FVector& VoxelCoord)
{
	// Trilinear interpolation
	int32 X0 = FMath::FloorToInt(VoxelCoord.X);
	int32 Y0 = FMath::FloorToInt(VoxelCoord.Y);
	int32 Z0 = FMath::FloorToInt(VoxelCoord.Z);

	int32 X1 = X0 + 1;
	int32 Y1 = Y0 + 1;
	int32 Z1 = Z0 + 1;

	// Clamp to valid range
	X0 = FMath::Clamp(X0, 0, Dimensions.X - 1);
	Y0 = FMath::Clamp(Y0, 0, Dimensions.Y - 1);
	Z0 = FMath::Clamp(Z0, 0, Dimensions.Z - 1);
	X1 = FMath::Clamp(X1, 0, Dimensions.X - 1);
	Y1 = FMath::Clamp(Y1, 0, Dimensions.Y - 1);
	Z1 = FMath::Clamp(Z1, 0, Dimensions.Z - 1);

	float Alpha = VoxelCoord.X - FMath::FloorToFloat(VoxelCoord.X);
	float Beta = VoxelCoord.Y - FMath::FloorToFloat(VoxelCoord.Y);
	float Gamma = VoxelCoord.Z - FMath::FloorToFloat(VoxelCoord.Z);

	// Sample 8 corners
	float C000 = SDFData[GetVoxelIndex(X0, Y0, Z0, Dimensions)].R.GetFloat();
	float C100 = SDFData[GetVoxelIndex(X1, Y0, Z0, Dimensions)].R.GetFloat();
	float C010 = SDFData[GetVoxelIndex(X0, Y1, Z0, Dimensions)].R.GetFloat();
	float C110 = SDFData[GetVoxelIndex(X1, Y1, Z0, Dimensions)].R.GetFloat();
	float C001 = SDFData[GetVoxelIndex(X0, Y0, Z1, Dimensions)].R.GetFloat();
	float C101 = SDFData[GetVoxelIndex(X1, Y0, Z1, Dimensions)].R.GetFloat();
	float C011 = SDFData[GetVoxelIndex(X0, Y1, Z1, Dimensions)].R.GetFloat();
	float C111 = SDFData[GetVoxelIndex(X1, Y1, Z1, Dimensions)].R.GetFloat();

	// Trilinear interpolation
	float C00 = FMath::Lerp(C000, C100, Alpha);
	float C10 = FMath::Lerp(C010, C110, Alpha);
	float C01 = FMath::Lerp(C001, C101, Alpha);
	float C11 = FMath::Lerp(C011, C111, Alpha);

	float C0 = FMath::Lerp(C00, C10, Beta);
	float C1 = FMath::Lerp(C01, C11, Beta);

	return FMath::Lerp(C0, C1, Gamma);
}

int32 FSDFMeshExporter::GetVoxelIndex(int32 X, int32 Y, int32 Z, const FIntVector& Dimensions)
{
	return Z * (Dimensions.X * Dimensions.Y) + Y * Dimensions.X + X;
}

FString FSDFMeshExporter::MeshToOBJString(
	const FDynamicMesh3& Mesh,
	const FOBJExportConfig& Config,
	const TArray<FLinearColor>* VertexColors)
{
	FString OBJContent;
	OBJContent.Reserve(Mesh.VertexCount() * 100); // Rough estimate

	// Header comment
	OBJContent += TEXT("# OBJ exported from SDFCut Plugin\n");
	OBJContent += FString::Printf(TEXT("# Vertices: %d, Triangles: %d\n"),
		Mesh.VertexCount(), Mesh.TriangleCount());
	OBJContent += TEXT("\n");

	// Build vertex index map (FDynamicMesh3 may have gaps)
	TMap<int32, int32> VertexIDToOBJIndex;
	int32 OBJVertexIndex = 1; // OBJ indices are 1-based

	// Write vertices (with optional vertex colors)
	for (int32 VertexID : Mesh.VertexIndicesItr())
	{
		FVector3d Pos = Mesh.GetVertex(VertexID);

		if (Config.bIncludeVertexColors && VertexColors && VertexColors->IsValidIndex(VertexID))
		{
			const FLinearColor& Color = (*VertexColors)[VertexID];
			OBJContent += FString::Printf(TEXT("v %.6f %.6f %.6f %.4f %.4f %.4f\n"),
				Pos.X, Pos.Y, Pos.Z,
				Color.R, Color.G, Color.B);
		}
		else
		{
			OBJContent += FString::Printf(TEXT("v %.6f %.6f %.6f\n"),
				Pos.X, Pos.Y, Pos.Z);
		}

		VertexIDToOBJIndex.Add(VertexID, OBJVertexIndex);
		OBJVertexIndex++;
	}

	OBJContent += TEXT("\n");

	// Write vertex normals if requested and available
	if (Config.bIncludeNormals && Mesh.HasVertexNormals())
	{
		for (int32 VertexID : Mesh.VertexIndicesItr())
		{
			FVector3f Normal = Mesh.GetVertexNormal(VertexID);
			OBJContent += FString::Printf(TEXT("vn %.6f %.6f %.6f\n"),
				Normal.X, Normal.Y, Normal.Z);
		}
		OBJContent += TEXT("\n");
	}

	// Write faces
	bool bHasNormals = Config.bIncludeNormals && Mesh.HasVertexNormals();

	for (int32 TriID : Mesh.TriangleIndicesItr())
	{
		FIndex3i Tri = Mesh.GetTriangle(TriID);

		int32 A = VertexIDToOBJIndex[Tri.A];
		int32 B = VertexIDToOBJIndex[Tri.B];
		int32 C = VertexIDToOBJIndex[Tri.C];

		// Reverse winding for right-handed coordinate system
		if (Config.bReverseWinding)
		{
			Swap(B, C);
		}

		if (bHasNormals)
		{
			// f v1//vn1 v2//vn2 v3//vn3
			OBJContent += FString::Printf(TEXT("f %d//%d %d//%d %d//%d\n"),
				A, A, B, B, C, C);
		}
		else
		{
			OBJContent += FString::Printf(TEXT("f %d %d %d\n"), A, B, C);
		}
	}

	return OBJContent;
}

bool FSDFMeshExporter::WriteMeshToOBJ(
	const FDynamicMesh3& Mesh,
	const FString& FilePath,
	const FOBJExportConfig& Config,
	const TArray<FLinearColor>* VertexColors)
{
	FString OBJContent = MeshToOBJString(Mesh, Config, VertexColors);
	return FFileHelper::SaveStringToFile(OBJContent, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

TArray<FLinearColor> FSDFMeshExporter::MaterialIDsToColors(const TArray<int32>& MaterialIDs)
{
	TArray<FLinearColor> Colors;
	Colors.SetNum(MaterialIDs.Num());

	// Color palette for different material IDs (matching EVolumeMaterial enum)
	// 0: Enamel - Off-white
	// 1: Dentin - Yellow
	// 2: Caries - Brown
	// 3: Fill - Metallic gray
	static const TArray<FLinearColor> MaterialPalette = {
		FLinearColor(1.0f, 1.0f, 0.9f),  // 0: Enamel - Off-white
		FLinearColor(1.0f, 0.9f, 0.6f),  // 1: Dentin - Yellow
		FLinearColor(0.4f, 0.3f, 0.2f),  // 2: Caries - Brown
		FLinearColor(0.6f, 0.6f, 0.7f),  // 3: Fill - Metallic gray
	};

	for (int32 i = 0; i < MaterialIDs.Num(); ++i)
	{
		int32 MatID = MaterialIDs[i];
		if (MatID >= 0 && MatID < MaterialPalette.Num())
		{
			Colors[i] = MaterialPalette[MatID];
		}
		else
		{
			// Fallback: encode as grayscale
			float Gray = FMath::Clamp(static_cast<float>(MatID) / 10.0f, 0.0f, 1.0f);
			Colors[i] = FLinearColor(Gray, Gray, Gray);
		}
	}

	return Colors;
}