#include "VoxelCutComputePass.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "GlobalShader.h"
#include "RenderGraphUtils.h"
#include "UniformBuffer.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"

// 实现UNIFORM_BUFFER
IMPLEMENT_UNIFORM_BUFFER_STRUCT(FToolTransformUniformBuffer, "ToolTransformUniformBuffer");
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

		// 1. 初始化参数
		FVoxelCutCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVoxelCutCS::FParameters>();

		// 2. 传入SDF参数
		PassParameters->ToolSDF = Params.ToolSDF->GetSDFTextureRHI();

		constexpr uint32 ElementSize = sizeof(FlatOctreeNode);
		const uint32 ArrayElementCount = Params.OctreeNodesArray.Num();

		// 3. 传入Input Buffer		

		// 3.1 Buffer Description
		FRDGBufferDesc InputBufferDesc = FRDGBufferDesc::CreateStructuredDesc(ElementSize, ArrayElementCount);
		InputBufferDesc.Usage = BUF_ShaderResource | BUF_Static; // 用作SRV，数据基本不变

		// 3.2 创建Buffer
		FRDGBufferRef InputBuffer = GraphBuilder.CreateBuffer(InputBufferDesc,TEXT("InputBuffer"));

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
		OutputBufferDesc.Usage = BUF_UnorderedAccess | BUF_ShaderResource; // 输出缓冲区需要UAV支持
		// 4.2 创建Buffer
		FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(OutputBufferDesc,TEXT("OutputBuffer"));
		// 4.3 把输入Buffer拷贝到输出Buffer
		AddCopyBufferPass(GraphBuilder, OutputBuffer, InputBuffer);
		// 4.4 设置给OutputBuffer
		PassParameters->OutputBuffer = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutputBuffer, PF_Unknown));

		// 5. 传入UniformBuffer
		// 创建第一个Uniform Buffer
		auto* ToolTransformUniformBuffer = GraphBuilder.AllocObject<FToolTransformUniformBuffer>();
		ToolTransformUniformBuffer->ToolTransform = FMatrix44f(Params.ToolTransform.ToMatrixWithScale()); // 假设Params中有这个数据

		FIntVector GroupCount(8, 1, 1);
		
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
					// 创建结果数组并拷贝数据
					TArray<FlatOctreeNode> ResultArray;
					ResultArray.SetNumUninitialized(ArrayElementCount);
            
					// 将数据从GPU内存拷贝到TArray
					FMemory::Memcpy(ResultArray.GetData(), LockedData, ArrayElementCount * ElementSize);
            
					GPUBufferReadback->Unlock();

					// 确保回调在游戏线程执行
					AsyncTask(ENamedThreads::GameThread, [AsyncCallback, ResultArray]() {
						AsyncCallback(ResultArray);
					});
				}
				else
				{
					// 读取失败，返回空数组
					TArray<FlatOctreeNode> EmptyArray;
					AsyncTask(ENamedThreads::GameThread, [AsyncCallback, EmptyArray]() {
						AsyncCallback(EmptyArray);
					});
				}

				// 清理资源
				delete GPUBufferReadback;
			} else {
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
