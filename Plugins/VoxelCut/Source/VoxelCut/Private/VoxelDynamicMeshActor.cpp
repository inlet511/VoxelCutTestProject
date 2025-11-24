// Fill out your copyright notice in the Description page of Project Settings.


#include "VoxelDynamicMeshActor.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "DynamicMesh/MeshNormals.h"

// Sets default values
AVoxelDynamicMeshActor::AVoxelDynamicMeshActor()
{
	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

	// 创建根组件
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	
	UDynamicMeshComponent* MeshComp = GetDynamicMeshComponent();
	// 启用碰撞
	//MeshComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	MeshComp->SetupAttachment(RootComponent); // 显式附加
	MeshComp->SetRelativeTransform(FTransform::Identity);
	MeshComp->SetMobility(EComponentMobility::Movable);
	MeshComp->SetSimulatePhysics(false);
}


void AVoxelDynamicMeshActor::AssignMesh(UStaticMesh* InMesh)
{
	if (!IsValid(InMesh))
		return;
	
	UDynamicMeshComponent* MeshComp = this->GetDynamicMeshComponent();
	UDynamicMesh* NewMesh = this->AllocateComputeMesh();
	
	FGeometryScriptCopyMeshFromAssetOptions Options;
	Options.bApplyBuildSettings = true;
	FGeometryScriptMeshReadLOD Lodsettings;
	Lodsettings.LODIndex = 0;
	EGeometryScriptOutcomePins Outcome;
	UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(InMesh, NewMesh, Options, Lodsettings, Outcome);
	
	if (Outcome == EGeometryScriptOutcomePins::Success)
	{
		MeshComp->SetMesh(MoveTemp(NewMesh->GetMeshRef()));
		MeshComp->UpdateBounds();
		MeshComp->UpdateCollision();
		// 计算法线
		FMeshNormals::QuickComputeVertexNormals(*MeshComp->GetMesh());
		
		MeshComp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		MeshComp->SetCollisionObjectType(ECC_WorldDynamic);
		MeshComp->SetCollisionResponseToAllChannels(ECR_Overlap);
		MeshComp->RecreatePhysicsState();
	
		// 同步到渲染系统
		MeshComp->NotifyMeshUpdated();
		MeshComp->MarkRenderStateDirty();
	}
	else
	{
		// 释放Mesh
		this->ReleaseComputeMesh(NewMesh);
	}
}

// Called when the game starts or when spawned
void AVoxelDynamicMeshActor::BeginPlay()
{
	Super::BeginPlay();

    if (SourceMesh && !GetDynamicMeshComponent()->GetMesh())
    {
        AssignMesh(SourceMesh);
    }
}

#if WITH_EDITOR
void AVoxelDynamicMeshActor::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	// 检测是否修改了SourceMesh属性
	FName PropertyName = (PropertyChangedEvent.Property != nullptr) ? PropertyChangedEvent.Property->GetFName() : NAME_None;
	if (PropertyName == GET_MEMBER_NAME_CHECKED(AVoxelDynamicMeshActor, SourceMesh))
	{
		// 立即应用新模型到DynamicMeshComponent
		AssignMesh(SourceMesh);
	}
}
#endif

// Called every frame
void AVoxelDynamicMeshActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

