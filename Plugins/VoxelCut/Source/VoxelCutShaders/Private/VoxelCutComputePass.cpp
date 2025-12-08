#include "VoxelCutComputePass.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "GlobalShader.h"
#include "RenderGraphUtils.h"
#include "UniformBuffer.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"

// 实现UNIFORM_BUFFER
IMPLEMENT_UNIFORM_BUFFER_STRUCT(FToolUB, "ToolUB");
// 实现Global Shader
IMPLEMENT_GLOBAL_SHADER(FVoxelCutCS,"/Project/Shaders/VoxelCutCS.usf","MainCS",SF_Compute);

void FVoxlCutShaderInterface::DispatchRenderThread(
	FRHICommandListImmediate& RHICmdList,
	FVoxelCutCSParams Params,
	TFunction<void(TArray<FlatOctreeNode>)> AsyncCallback
)
{
	FRDGBuilder GraphBuilder(RHICmdList);
	{
		// 获取ComputeShader
		TShaderMapRef<FVoxelCutCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		bool bIsShaderValid = ComputeShader.IsValid();

		//BEGIN_RDG_EVENT(GraphBuilder, "VoxelCutComputeShader");

		if (bIsShaderValid)
		{
			// 1. 初始化参数
			FVoxelCutCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVoxelCutCS::FParameters>();

			// 2. 传入SDF参数
			PassParameters->ToolSDF = Params.ToolSDFGenerator->GetSDFTextureRHI();
			PassParameters->ToolSDFSampler = TStaticSamplerState<SF_Bilinear, AM_Mirror, AM_Mirror>::GetRHI();
			
			constexpr uint32 ElementSize = sizeof(FlatOctreeNode);
			const uint32 ArrayElementCount = Params.OctreeNodesArray.Num();

			// 3. 传入Input Buffer
			// 3.1 Buffer Description
			FRDGBufferDesc InputBufferDesc = FRDGBufferDesc::CreateStructuredDesc(ElementSize, ArrayElementCount);


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
			// 4.2 创建Buffer
			FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(OutputBufferDesc, TEXT("OutputBuffer"));
			// 4.3 把输入Buffer拷贝到输出Buffer
			AddCopyBufferPass(GraphBuilder, OutputBuffer, InputBuffer);
			// 4.4 设置给OutputBuffer
			PassParameters->OutputBuffer = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutputBuffer, PF_Unknown));

			// 5. 传入UniformBuffer
			auto* ToolUBParameters = GraphBuilder.AllocParameters<FToolUB>();
			FTransform InverseTransform = Params.ToolTransform.Inverse(); // 直接传进去inverse Transform, shader中不好计算inverse
			ToolUBParameters->ToolInverseTransform = FMatrix44f(InverseTransform.ToMatrixWithScale());
			ToolUBParameters->ToolBoundsLocalMin = FVector3f(Params.ToolSDFGenerator->GetSDFBounds().Min);
			ToolUBParameters->ToolBoundsLocalMax = FVector3f(Params.ToolSDFGenerator->GetSDFBounds().Max);
			ToolUBParameters->VolumeTextureSize = Params.ToolSDFGenerator->GetVolumeSize();
			TRDGUniformBufferRef<FToolUB> ToolUB = GraphBuilder.CreateUniformBuffer(ToolUBParameters);
			PassParameters->ToolUB = ToolUB;

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
						AsyncTask(ENamedThreads::GameThread, [AsyncCallback, ResultArray = MoveTemp(ResultArray)] ()mutable {
							AsyncCallback(MoveTemp(ResultArray));
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

