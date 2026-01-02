# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

SDFCut is an Unreal Engine plugin providing GPU-accelerated runtime mesh cutting using Signed Distance Fields (SDF). It targets dental/surgical simulation with real-time haptic feedback.

## Build Commands

- Open the host `.uproject` in Unreal Editor and enable the SDFCut plugin
- Regenerate Visual Studio project files, then build in Visual Studio
- After shader changes, restart the editor or rebuild the plugin to recompile shaders

## Architecture

### Modules (3 total, Win64 only)

| Module | Type | Purpose |
|--------|------|---------|
| SDFCut | Runtime | Core GPU cutting system |
| SDFCutHaptic | Runtime | Haptic probe feedback (depends on SDFCut) |
| SDFCutEditor | Editor | SDF generation utilities |

### Data Flow

```
Static Meshes → SDF Texture (Editor) → UGPUSDFCutter (Runtime)
    → GPU Compute Shader → Volume Render Target
    → ISDFVolumeProvider → UHapticProbeComponent
```

### Key Classes

**UGPUSDFCutter** (`Source/SDFCut/Public/GPUSDFCutter.h`)
- Manages render targets, SRVs/UAVs, and compute dispatch
- Tracks tool/target transforms; dispatches updates when dirty
- Implements `ISDFVolumeProvider` for thread-safe SDF queries
- CPU data cache: `TArray<FFloat16Color> CPU_SDFData` (R=distance, G=materialID)

**FUpdateSDFCS** (`Source/SDFCut/Public/UpdateSDFShader.h`)
- Compute shader parameter layout with uniform buffer `FCutUB`
- Shader file: `Shaders/DynamicSDFUpdateCS.usf`
- CSG subtraction: `result = max(OriginalSDF, -ToolSDF)`

**UHapticProbeComponent** (`Source/SDFCutHaptic/Public/HapticProbeComponent.h`)
- Calculates penalty forces from SDF penetration at sampled surface points
- Material-specific stiffness scaling (Enamel > Dentin > Caries > Fill)
- Parallel calculation for >64 sample points

**USDFGenLibrary** (`Source/SDFCutEditor/Public/SDFGenLibrary.h`)
- Blueprint-callable SDF generation from static meshes
- Uses GeometryCore for AABB tree and FastWinding queries

**FSDFMeshExporter** (`Source/SDFCut/Public/SDFMeshExporter.h`)
- Runtime mesh extraction from SDF using Marching Cubes (via GeometryCore's FMarchingCubes)
- OBJ file export with vertex colors for material ID visualization
- Used by `UGPUSDFCutter::ExportToOBJ()` and `ExtractMesh()`

### Threading Model

- Game thread: Component management, data structures
- Render thread: All GPU work via `ENQUEUE_RENDER_COMMAND`
- `FRWLock DataRWLock` protects `CPU_SDFData` for haptic queries

## Critical Patterns

**GPU dispatch pattern:**
```cpp
ENQUEUE_RENDER_COMMAND(CommandName)([...](FRHICommandListImmediate& RHICmdList) {
    FRDGBuilder GraphBuilder(RHICmdList);
    // GPU work
    GraphBuilder.Execute();
});
```

**Thread-safe SDF reads:**
```cpp
FRWScopeLock ReadLock(SDFProvider->GetDataLock(), SLT_ReadOnly);
// Safe to query CPU_SDFData
```

**Shader path mapping:** `/SDF/Shaders` → `Plugins/SDFCut/Shaders` (registered in `SDFCut.cpp`)

## Modifying Shaders

1. Update `FCutUB` struct in `UpdateSDFShader.h`
2. Update matching layout in `DynamicSDFUpdateCS.usf`
3. For new SRV/UAV: add to `FParameters`, create in `DispatchLocalUpdate()` via `GraphBuilder.CreateSRV/CreateUAV`
4. Restart editor or rebuild plugin

## What Not to Change Lightly

- Shader struct layout and binding order (mismatches cause crashes)
- Threading model (no blocking GPU readbacks on game thread)
- Transform tracking flow in `TickComponent`

## Debugging

- Add `UE_LOG(LogTemp, ...)` in `GPUSDFCutter` to trace `InitResources`, `DispatchLocalUpdate`, and render-thread lambdas
- Check `ToolRHI`/`VolumeRHI` null checks in `DispatchLocalUpdate()` for binding issues
- Test map: `Content/Maps/VolumePaintLevel.umap`

## Mesh Export (Runtime)

Export cut SDF volume to OBJ mesh:

**Blueprint/C++ Usage:**
```cpp
// Simple export
SDFCutter->ExportToOBJ(TEXT("C:/Output/mesh.obj"));

// Detailed export with custom resolution
SDFCutter->ExportToOBJWithDetail(
    TEXT("C:/Output/mesh_hires.obj"),
    VoxelSize * 0.5f,  // Smaller cube = more detail
    true,              // Include normals
    true               // Include material colors
);

// Get raw mesh data for custom processing
TArray<FVector> Vertices;
TArray<int32> Triangles, MaterialIDs;
TArray<FVector> Normals;
SDFCutter->ExtractMesh(Vertices, Triangles, Normals, MaterialIDs);
```

**OBJ Format Notes:**
- Vertices include RGB colors encoding material ID (Enamel=off-white, Dentin=yellow, Caries=brown, Fill=gray)
- Triangle winding reversed for right-handed coordinate system compatibility
