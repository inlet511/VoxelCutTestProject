// Fill out your copyright notice in the Description page of Project Settings.


#include "YourActor.h"

#include "CanvasItem.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Canvas.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"

UE_DISABLE_OPTIMIZATION
// Sets default values
AYourActor::AYourActor()
{
	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
}

// Called when the game starts or when spawned
void AYourActor::BeginPlay()
{
	Super::BeginPlay();
	// 0. 前置检查：确保核心资源存在
	if (!SourceTexture || !BaseMaterial || !MyMeshActor) return;




	// --- 关键修改 1: 强制关闭流送并等待高分辨率 Mip 加载 ---
	// 告诉流送系统：我们需要最高精度的 Mip
	SourceTexture->SetForceMipLevelsToBeResident(30.0f); 
	// 强制阻塞 GameThread 直到流送完成 (确保 TextureRHI 变成 1024x1024)
	SourceTexture->WaitForStreaming(true,true); 

	int32 TextureSizeX = SourceTexture->GetSizeX();
	int32 TextureSizeY = SourceTexture->GetSizeY();
	
	// 1. 创建 RenderTarget (Game Thread)
	// 使用 Kismet 库一行代码创建，自动处理格式初始化
	MyRenderTarget = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSizeX, TextureSizeY, RTF_RGBA8);

	// 2. 获取底层资源指针 (Game Thread)
	// 必须在入队前获取 Resource 指针，不能在 Lambda 中直接访问 UObject
	SourceTexture->UpdateResource(); // 强制刷新资源，防止 RHI 为空
	FTextureResource* SrcRes = SourceTexture->GetResource();
	FTextureRenderTargetResource* DstRes = MyRenderTarget->GameThread_GetRenderTargetResource();

	// 3. 入队渲染命令 (Render Thread)
	ENQUEUE_RENDER_COMMAND(Cmd_CopyTexture)(
		[SrcRes, DstRes,TextureSizeX,TextureSizeY](FRHICommandListImmediate& RHICmdList)
		{
			// 确保 RHI 资源已创建
			if (!SrcRes || !DstRes || !SrcRes->TextureRHI || !DstRes->TextureRHI) return;

			FRHITexture* SrcRHI = SrcRes->TextureRHI;
			FRHITexture* DstRHI = DstRes->TextureRHI;

			// A. 状态转换：准备复制
			RHICmdList.Transition(FRHITransitionInfo(SrcRHI, ERHIAccess::Unknown, ERHIAccess::CopySrc));
			RHICmdList.Transition(FRHITransitionInfo(DstRHI, ERHIAccess::Unknown, ERHIAccess::CopyDest));


			FRHICopyTextureInfo CopyInfo;
			CopyInfo.Size = FIntVector(TextureSizeX, TextureSizeY, 0);
			
			
			// B. 执行底层复制 (显存 -> 显存)
			RHICmdList.CopyTexture(SrcRHI, DstRHI, CopyInfo);

			// C. 状态恢复：准备被 Shader 读取
			RHICmdList.Transition(FRHITransitionInfo(DstRHI, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
		});

	// 4. 创建并设置动态材质 (Game Thread)
	// 此时 RenderTarget 对象已存在，可以直接绑定，GPU 会保证在 DrawCall 时上面的 Copy 命令已完成
	UMaterialInstanceDynamic* DynMat = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	DynMat->SetTextureParameterValue(TextureParameterName, MyRenderTarget);

	// 5. 应用到模型
	MyMeshActor->GetStaticMeshComponent()->SetMaterial(0, DynMat);
    
	// 保存引用防止 GC
	MyDynMaterial = DynMat; 

}

// Called every frame
void AYourActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

UE_ENABLE_OPTIMIZATION
