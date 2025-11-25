// Fill out your copyright notice in the Description page of Project Settings.


#include "ExampleComputeShader/ComputeShaderUser.h"
#include "ExampleComputeShader.h"
#include "ComputeShader/Public/ExampleComputeShader/ExampleComputeShader.h"

// Sets default values
AComputeShaderUser::AComputeShaderUser()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

}

// Called when the game starts or when spawned
void AComputeShaderUser::BeginPlay()
{
	Super::BeginPlay();
	FExampleComputeShaderDispatchParams DispatchParams(134,256,0);
	FExampleComputeShaderInterface::Dispatch(DispatchParams,[&](int OutValue)
	{
		GEngine->AddOnScreenDebugMessage(-1,5.0f,FColor::Purple,FString::Printf(TEXT("Returned Value: %d"), OutValue));
	});
	
	
}

// Called every frame
void AComputeShaderUser::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

