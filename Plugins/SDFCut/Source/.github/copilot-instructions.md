# Copilot instructions for SDFCut plugin

Purpose: give coding agents the minimal, concrete context needed to be productive editing this Unreal Engine plugin.

- **Big picture:** This plugin provides GPU-accelerated local SDF updates for runtime mesh cutting. The runtime component is `UGPUSDFCutter` (SDFCut/Public/GPUSDFCutter.h, SDFCut/Private/GPUSDFCutter.cpp). Shaders live under `/SDF/Shaders` and are registered in `SDFCut/Private/SDFCut.cpp` via `AddShaderSourceDirectoryMapping`.

- **Key components & boundaries:**
  - `UGPUSDFCutter` — game/runtime component that manages render targets, SRVs/UAVs, and dispatches compute work on the render thread.
  - `FUpdateSDFCS` / `UpdateSDFShader.h` — compute shader parameter layout (uniform buffer `FCutUB`, SRV/UAV bindings).
  - `SDFGenLibrary` (SDFCutEditor/Private/SDFGenLibrary.cpp) — editor-only utility that generates `UVolumeTexture` assets (CPU path).

- **Important files to reference when editing behavior:**
  - SDFCut/Private/GPUSDFCutter.cpp — allocation, RDG usage, `ENQUEUE_RENDER_COMMAND`, `DispatchLocalUpdate()` implementation.
  - SDFCut/Public/UpdateSDFShader.h — shader parameter struct names (`FCutUB`, `FUpdateSDFCS::FParameters`).
  - SDFCut/Private/SDFCut.cpp — registers shader source mapping: `/SDF/Shaders` → plugin Shaders folder.
  - SDFCutEditor/Private/SDFGenLibrary.cpp — shows conventions for creating `UVolumeTexture` assets and atlas packing.

- **Patterns and conventions (project-specific):**
  - All GPU updates are dispatched from the game thread by enqueueing lambda work to the render thread (use `ENQUEUE_RENDER_COMMAND`). Do not call RHI functions directly on game thread.
  - Use Render Graph (FRDGBuilder) and `RegisterExternalTexture` when referencing engine textures or render targets from the shader pass.
  - The compute shader name is bound by `IMPLEMENT_GLOBAL_SHADER(FUpdateSDFCS, "/SDF/Shaders/DynamicSDFUpdateCS.usf", "LocalSDFUpdateKernel", SF_Compute)` — if you change the USF path/name update both the engine mapping (`AddShaderSourceDirectoryMapping`) and the `IMPLEMENT_GLOBAL_SHADER` call.
  - Material parameter: `SDFMaterialInstanceDynamic` is given `VolumeTexture` (see GPUSDFCutter.cpp). Use the same parameter name when updating materials.

- **How to add / change shader parameters safely:**
  - Update `FCutUB` in `UpdateSDFShader.h` first, then modify USF to match the new struct layout. Recompile shader by rebuilding the plugin or by restarting the editor (shader recompilation may be required).
  - When adding SRV/UAV bindings, add them to `FParameters` and create SRV/UAV in `DispatchLocalUpdate()` using `GraphBuilder.CreateSRV` / `CreateUAV`.

- **Editor & build workflows:**
  - Shaders are placed under `Plugins/SDFCut/Shaders`. `SDFCut/Private/SDFCut.cpp` maps `/SDF/Shaders` to that folder. After changing shaders, either restart the editor or trigger shader recompilation.
  - Plugin builds are done via the normal Unreal build flow (open the host `.uproject` in Editor and enable the plugin, or build via Visual Studio / UnrealBuildTool). When in doubt: regenerate project files and build in Visual Studio.

- **Examples (concrete snippets to search/modify):**
  - To find the compute dispatch: search for `DispatchLocalUpdate()` or `FUpdateSDFCS` in `SDFCut/`.
  - To change how the VolumeRT is created/looked-up: edit `GPUSDFCutter::InitResources()` where `UKismetRenderingLibrary::CreateRenderTargetVolume` is used.

- **Testing & debugging tips:**
  - Add `UE_LOG(LogTemp, ...)` in `GPUSDFCutter` to trace when `InitResources`, `DispatchLocalUpdate`, and render-thread lambdas execute.
  - Validate SRV/UAV by checking `ToolRHI` / `VolumeRHI` null checks in `DispatchLocalUpdate()`; adding early logs there helps diagnose binding issues.

- **What not to change lightly:**
  - The shader struct layout and binding order — mismatches between USF and `UpdateSDFShader.h` cause subtle rendering bugs/crashes.
  - The thread/context usage: do not perform blocking GPU readbacks on the game thread; follow the existing enqueue/readback flow.

If any part is unclear or you want examples expanded (editor workflow, shader live-reload steps, or a short how-to for adding a new uniform), tell me which section to expand.
