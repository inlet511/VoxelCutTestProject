// Fill out your copyright notice in the Description page of Project Settings.


#include "VoxelCuttingActor.h"

#include "DynamicMeshActor.h"


// Sets default values
AVoxelCuttingActor::AVoxelCuttingActor()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));    
    
	// 创建切削组件
	VoxelCutComponent = CreateDefaultSubobject<UVoxelCutComponent>(TEXT("VoxelCutComponent"));

	CutToolMeshComponent = nullptr;
	TargetMeshComponent = nullptr;
}

void AVoxelCuttingActor::StartCutting()
{
	if (VoxelCutComponent)
	{
		VoxelCutComponent->StartCutting();
	}
}

void AVoxelCuttingActor::StopCutting()
{
	if (VoxelCutComponent)
	{
		VoxelCutComponent->StopCutting();
	}
}


#if WITH_EDITOR
void AVoxelCuttingActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;

	if (PropertyName == GET_MEMBER_NAME_CHECKED(AVoxelCuttingActor, TargetActor))
	{
		if (TargetActor)
		{
			TargetMeshComponent = TargetActor->FindComponentByClass<UDynamicMeshComponent>();
		}
		else
		{
			TargetMeshComponent = nullptr;
		}
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(AVoxelCuttingActor, ToolActor))
	{
		if (ToolActor)
		{
			CutToolMeshComponent = ToolActor->FindComponentByClass<UDynamicMeshComponent>();
		}
		else
		{
			CutToolMeshComponent = nullptr;
		}
	}
}
#endif
// Called when the game starts or when spawned
void AVoxelCuttingActor::BeginPlay()
{
	Super::BeginPlay();

	SetupComponents();
	CheckComponents();
	
	if (VoxelCutComponent)
	{
		VoxelCutComponent->SetCutToolMesh(CutToolMeshComponent);
		VoxelCutComponent->SetTargetMesh(TargetMeshComponent);
		VoxelCutComponent->InitializeCutSystem();
		
	}
	
}

// Called every frame
void AVoxelCuttingActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

void AVoxelCuttingActor::SetupComponents()
{
	if (ToolActor)
	{
		CutToolMeshComponent = ToolActor->GetDynamicMeshComponent();
	}
	if (TargetActor)
	{
		TargetMeshComponent = TargetActor->GetDynamicMeshComponent();
	}
}

void AVoxelCuttingActor::CheckComponents()
{
	if (!CutToolMeshComponent || !TargetMeshComponent)
	{
		FString ErrorMsg = TEXT("错误：");
		if (!CutToolMeshComponent) ErrorMsg += TEXT("ToolActor未设置或缺少DynamicMeshComponent！\n");
		if (!TargetMeshComponent) ErrorMsg += TEXT("TargetActor未设置或缺少DynamicMeshComponent！");
		
		UE_LOG(LogTemp, Error, TEXT("%s"), *ErrorMsg);
		
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Red, ErrorMsg);
		}		
	}
}

