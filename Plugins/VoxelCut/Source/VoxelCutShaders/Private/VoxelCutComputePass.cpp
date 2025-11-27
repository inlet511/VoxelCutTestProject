#include "VoxelCutComputePass.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "GlobalShader.h"
#include "RenderGraphUtils.h"
#include "UniformBuffer.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"

UE_DISABLE_OPTIMIZATION

// 实现UNIFORM_BUFFER
IMPLEMENT_UNIFORM_BUFFER_STRUCT(FToolTransformUniformBuffer, "ToolTransformUniformBuffer");
IMPLEMENT_UNIFORM_BUFFER_STRUCT(FSDFBoundsUniformBuffer, "SDFBoundsUniformBuffer");
// 实现Global Shader
IMPLEMENT_GLOBAL_SHADER(FVoxelCutCS,"/Project/Shaders/VoxelCutCS.usf","MainCS",SF_Compute);

void FVoxlCutShaderInterface::DispatchRenderThread(
	FVoxelCutCSParams Params,
	FRHICommandListImmediate& RHICmdList,
	TFunction<void(TArray<FlatOctreeNode>)> AsyncCallback
)
{
	FRDGBuilder GraphBuilder(RHICmdList);
	{
		// 获取ComputeShader
		TShaderMapRef<FVoxelCutCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		bool bIsShaderValid = ComputeShader.IsValid();
		if (bIsShaderValid)
		{
			// 1. 初始化参数
			FVoxelCutCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVoxelCutCS::FParameters>();

			// 2. 传入SDF参数
			PassParameters->ToolSDF = Params.ToolSDF->GetSDFTextureRHI();

			constexpr uint32 ElementSize = sizeof(FlatOctreeNode);
			const uint32 ArrayElementCount = Params.OctreeNodesArray.Num();

			if (ArrayElementCount == 0)
			{
				UE_LOG(LogTemp, Warning, TEXT("Octree Array is Empty!"));
				GraphBuilder.Execute(); // 手动提交命令列表
				AsyncCallback(TArray<FlatOctreeNode>());
				return;
			}
			// 3. 传入Input Buffer		

			// 3.1 Buffer Description
			FRDGBufferDesc InputBufferDesc = FRDGBufferDesc::CreateStructuredDesc(ElementSize, ArrayElementCount);
			InputBufferDesc.Usage = BUF_ShaderResource | BUF_Static; // 用作SRV，数据基本不变

			// 3.2 创建Buffer
			FRDGBufferRef InputBuffer = GraphBuilder.CreateBuffer(InputBufferDesc, TEXT("InputBuffer"));

			// 3.3 Buffer数据上传
			GraphBuilder.QueueBufferUpload(
				InputBuffer,
				Params.OctreeNodesArray.GetData(), // 源数据指针
				ElementSize * ArrayElementCount, // 总数据大小（字节数）
				ERDGInitialDataFlags::None
			);
			// 3.4 设置给InputBuffer
			PassParameters->InputBuffer = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InputBuffer, PF_Unknown));


			// 4. 传入 Output Buffer

			// 4.1 创建输出缓冲区（与输入缓冲区相同结构）
			FRDGBufferDesc OutputBufferDesc = FRDGBufferDesc::CreateStructuredDesc(ElementSize, ArrayElementCount);
			OutputBufferDesc.Usage = BUF_UnorderedAccess | BUF_SourceCopy; // 输出缓冲区需要UAV支持
			// 4.2 创建Buffer
			FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(OutputBufferDesc, TEXT("OutputBuffer"));
			// 4.3 把输入Buffer拷贝到输出Buffer
			AddCopyBufferPass(GraphBuilder, OutputBuffer, InputBuffer);
			// 4.4 设置给OutputBuffer
			PassParameters->OutputBuffer = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutputBuffer, PF_Unknown));

			// 5. 传入UniformBuffer
			//  5.1 传递ToolTransform Uniform Buffer
			auto* ToolTransformUBParameters = GraphBuilder.AllocParameters<FToolTransformUniformBuffer>();
			FTransform InverseTransform = Params.ToolTransform.Inverse(); // 直接传进去inverse Transform, shader中不好计算inverse
			ToolTransformUBParameters->ToolTransform = FMatrix44f(InverseTransform.ToMatrixWithScale());
			TRDGUniformBufferRef<FToolTransformUniformBuffer> ToolTransformUB = GraphBuilder.CreateUniformBuffer(ToolTransformUBParameters);
			PassParameters->ToolTransformUniformBuffer = ToolTransformUB;

			// 5.2 传递SDF边界 UniformBuffer
			auto* SDFBoundsUniformBuffer = GraphBuilder.AllocParameters<FSDFBoundsUniformBuffer>();
			SDFBoundsUniformBuffer->SDFBoundsMin = FVector3f(Params.ToolSDF->GetSDFBounds().Min);
			SDFBoundsUniformBuffer->SDFBoundsMax = FVector3f(Params.ToolSDF->GetSDFBounds().Max);
			SDFBoundsUniformBuffer->VolumeTextureSize = Params.ToolSDF->GetVolumeSize();
			TRDGUniformBufferRef<FSDFBoundsUniformBuffer> SDFBoundsBuffer = GraphBuilder.CreateUniformBuffer(SDFBoundsUniformBuffer);
			PassParameters->SDFBoundsUniformBuffer = SDFBoundsBuffer;

			FIntVector GroupCount(FMath::DivideAndRoundUp<int>(ArrayElementCount, 64), 1, 1);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("ExecuteVoxelCuteComputeShader"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[&PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
				});


			FRHIGPUBufferReadback* GPUBufferReadback = new FRHIGPUBufferReadback(TEXT("ExecuteVoxelCutComputeShaderOutput"));
			AddEnqueueCopyPass(GraphBuilder, GPUBufferReadback, OutputBuffer, 0u);


			auto RunnerFunc = [GPUBufferReadback, AsyncCallback, ArrayElementCount](auto&& RunnerFunc) -> void {
				UE_LOG(LogTemp, Warning, TEXT("Waiting for GPU Readback..."));

				if (GPUBufferReadback->IsReady()) {

					// 锁定缓冲区以读取数据
					void* LockedData = GPUBufferReadback->Lock(ArrayElementCount * ElementSize);
					if (LockedData)
					{
						TArray<FlatOctreeNode> ResultArray;
						ResultArray.SetNumUninitialized(ArrayElementCount);

						FMemory::Memcpy(ResultArray.GetData(), LockedData, ArrayElementCount * ElementSize);

						GPUBufferReadback->Unlock();

						// 确保回调在游戏线程执行
						AsyncTask(ENamedThreads::GameThread, [AsyncCallback, ResultArray]() {
							AsyncCallback(ResultArray);
							});

						delete GPUBufferReadback;
					}
					else
					{
						// 读取失败，返回空数组
						TArray<FlatOctreeNode> EmptyArray;
						AsyncTask(ENamedThreads::GameThread, [AsyncCallback, EmptyArray]() {
							AsyncCallback(EmptyArray);
							});
					}
				}
				else {
					AsyncTask(ENamedThreads::ActualRenderingThread, [RunnerFunc]() {
						RunnerFunc(RunnerFunc);
						});
				}
				};

			AsyncTask(ENamedThreads::ActualRenderingThread, [RunnerFunc]() {
				RunnerFunc(RunnerFunc);
				});
		}
	}

	GraphBuilder.Execute();
}

UE_ENABLE_OPTIMIZATION