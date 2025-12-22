// Fill out your copyright notice in the Description page of Project Settings.


#include "HapticProbeComponent.h"
#include "GPUSDFCutter.h"
#include "DrawDebugHelpers.h" 


UHapticProbeComponent::UHapticProbeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UHapticProbeComponent::BeginPlay()
{
	Super::BeginPlay();
	SetSDFVolumeProvider();
	
	// 初始化材质系数 
	MaterialStiffnessScales.Add(0, 1.0f); // 牙釉质 (硬)
	MaterialStiffnessScales.Add(1, 0.6f); // 牙本质 (中)
	MaterialStiffnessScales.Add(2, 0.2f); // 龋坏 (软)
	MaterialStiffnessScales.Add(3, 0.8f); // 填充物(较硬)
	
	UpdateProbeMesh();

	// 获取RayStart
	RayStart = Cast<USceneComponent>(RayStartPointRef.GetComponent(GetOwner()));
}

void UHapticProbeComponent::SetSDFVolumeProvider()
{
	if (!CutterActor)
	{
		UE_LOG(LogTemp,Warning, TEXT("请设置CutterActor"));
	}
	// 尝试获取接口
	UGPUSDFCutter* Cutter = CutterActor->FindComponentByClass<UGPUSDFCutter>();
	
	if (Cutter)
	{
		SDFProvider = static_cast<ISDFVolumeProvider*>(Cutter);
	}
	
	if (!Cutter || !SDFProvider)
	{
		UE_LOG(LogTemp, Warning, TEXT("CutterActor中没有GPUSDFCutter组件!"))
	}
}

bool UHapticProbeComponent::CalculateForce(FVector& OutForce, FVector& OutTorque)
{
    if (!SDFProvider) return false;

    // [开始计时]
    double StartTime = FPlatformTime::Seconds();

    // 1. 获取读锁
    FRWScopeLock ReadLock(SDFProvider->GetDataLock(), SLT_ReadOnly);

    FVector FinalForce = FVector::ZeroVector;
    FVector FinalTorque = FVector::ZeroVector;

    // 获取公共数据
    const FTransform ProbeCompTransform = ProbeMeshComp->GetComponentTransform();
    const FVector ProbeLocation = ProbeCompTransform.GetLocation();
    const FVector ProbeForward = ProbeCompTransform.GetUnitAxis(EAxis::X); // 假设X轴是探针前方
    const float VoxelSize = SDFProvider->GetVoxelSize();
    const int32 NumPoints = LocalSamplePoints.Num();

    // =========================================================
    // 步骤 1: 收集几何信息 (Gather Geometry)
    // =========================================================
    
    // 预分配数据容器
    TArray<FGeoSampleData> SampleResults;
    SampleResults.SetNum(NumPoints);

    // 阈值：决定是否开启多线程
    bool bUseParallel = (NumPoints > 64) && !bVisualizeSamplePoints;

    auto GatherFunction = [&](int32 Idx)
    {
        const FVector& LocalPt = LocalSamplePoints[Idx];
        FVector WorldPt = ProbeCompTransform.TransformPosition(LocalPt);
        FVector VoxelCoord;

        SampleResults[Idx].bIsValid = false;

        if (SDFProvider->WorldToVoxelSpace(WorldPt, VoxelCoord))
        {
            float SDFVal = SDFProvider->SampleSDF(VoxelCoord);

            if (SDFVal < 0.0f) // 碰撞
            {
                // 计算梯度 (法线方向)
                const float H = 1.0f; 
                float DX_P = SDFProvider->SampleSDF(VoxelCoord + FVector(H, 0, 0));
                float DX_N = SDFProvider->SampleSDF(VoxelCoord - FVector(H, 0, 0));
                float DY_P = SDFProvider->SampleSDF(VoxelCoord + FVector(0, H, 0));
                float DY_N = SDFProvider->SampleSDF(VoxelCoord - FVector(0, H, 0));
                float DZ_P = SDFProvider->SampleSDF(VoxelCoord + FVector(0, 0, H));
                float DZ_N = SDFProvider->SampleSDF(VoxelCoord - FVector(0, 0, H));

                FVector Gradient(DX_P - DX_N, DY_P - DY_N, DZ_P - DZ_N);
                
                SampleResults[Idx].Gradient = Gradient.GetSafeNormal();
                SampleResults[Idx].Depth = -SDFVal * VoxelSize;
                SampleResults[Idx].bIsValid = true;
            }
        }
    };

    if (bUseParallel)
    {
        ParallelFor(NumPoints, GatherFunction);
    }
    else
    {
        for (int32 i = 0; i < NumPoints; i++) GatherFunction(i);
    }

    // =========================================================
    // 步骤 2: 分析受力方向 (Analyze Direction)
    // =========================================================

    FVector ConsensusNormal = FVector::ZeroVector;
    int32 HitCount = 0;
    float MinDepth = FLT_MAX;

    // 2.1 寻找最浅深度
    for (const auto& Sample : SampleResults)
    {
        if (Sample.bIsValid)
        {
            HitCount++;
            if (Sample.Depth < MinDepth) MinDepth = Sample.Depth;
        }
    }

    if (HitCount == 0) return false; // 无碰撞

    // 2.2 计算共识法线 (只统计"表面层"的点)
    // 表面层定义：最浅深度 + 1.5个 体素厚度
    float SurfaceThreshold = MinDepth + (VoxelSize * 1.5f);
    FVector NormalAccumulator = FVector::ZeroVector;
    int32 SurfacePointsCount = 0;

    for (const auto& Sample : SampleResults)
    {
        // 只有靠近表面的点，其梯度才是可信的表面法线
        if (Sample.bIsValid && Sample.Depth <= SurfaceThreshold)
        {
            NormalAccumulator += Sample.Gradient;
            SurfacePointsCount++;
        }
    }

    if (SurfacePointsCount > 0)
    {
        ConsensusNormal = NormalAccumulator.GetSafeNormal();
    }
    else
    {
        // 极端情况：所有点都很深 (Deep Penetration)
        // 此时局部梯度不可信，暂时设为零，后面由射线探测补救
        ConsensusNormal = FVector::ZeroVector;
    }

    // =========================================================
    // 步骤 3: 射线探测真实深度 (Ray Marching for True Depth)
    // =========================================================

	FVector SafeStartPoint = RayStart->GetComponentLocation();
    FVector SurfaceHitPoint;
    bool bFoundSurface = FindSurfacePointFromRay(SafeStartPoint, ProbeLocation, SurfaceHitPoint);

    float PenetrationDepth = 0.0f;
    FVector ForceDirection = FVector::ZeroVector;

    if (bFoundSurface)
    {
        // 3.1 成功找到表面
        PenetrationDepth = FVector::Dist(ProbeLocation, SurfaceHitPoint);
        
        // 决策方向：
        // 如果之前算出的 ConsensusNormal 有效（即有浅层点），优先用它（保留表面纹理感）
        // 如果 ConsensusNormal 无效（完全深层穿透），则用 (SafeStart - Probe) 的方向（救生索方向）
        
        if (!ConsensusNormal.IsNearlyZero())
        {
            ForceDirection = ConsensusNormal;
            
            // 保护：防止 ConsensusNormal 指向内部（虽然SDF梯度通常指向外部，但以防万一）
            // 检查 ForceDirection 是否大致指向 SafeStartPoint
            FVector RescueDir = (SafeStartPoint - ProbeLocation).GetSafeNormal();
            if (FVector::DotProduct(ForceDirection, RescueDir) < -0.2f)
            {
                // 方向反了！强制修正为救生方向
                ForceDirection = RescueDir;
            }
        }
        else
        {
            // 深层穿透模式：直接指向表面点
            ForceDirection = (SurfaceHitPoint - ProbeLocation).GetSafeNormal();
        }
    }
    else
    {
        // 3.2 射线没找到表面（可能起点也在内部，或者SDF也是空的）
        // 降级方案：使用局部采样的最大深度作为估算
        PenetrationDepth = MinDepth; // 使用之前统计的最浅深度作为保守估计
        ForceDirection = ConsensusNormal.IsNearlyZero() ? -ProbeForward : ConsensusNormal;
    }

    // =========================================================
    // 步骤 4: 计算最终力与力矩 (Final Calculation)
    // =========================================================

    // 这里简化了材质获取，直接取探针中心位置的材质，或者取最近点的材质
    // 为了更精确，应该在 SphereTracing 时顺便获取 SurfaceHitPoint 的材质 ID
    float Stiffness = BaseStiffness; 
    
    // 计算力：F = k * x * n
    FinalForce = ForceDirection * (PenetrationDepth * Stiffness);

    // 计算力矩：T = r x F
    // 力的作用点应该在"表面接触点" (SurfaceHitPoint)，而不是探针中心
    // 如果 SphereTracing 失败，退化为探针中心（力矩为0）
    if (bFoundSurface)
    {
        FVector Arm = SurfaceHitPoint - ProbeLocation;
        FinalTorque = FVector::CrossProduct(Arm, FinalForce);
    }
    else
    {
        FinalTorque = FVector::ZeroVector;
    }

    // 可视化
    if (bVisualizeForce)
    {
        DrawDebugLine(GetWorld(), ProbeLocation, ProbeLocation + FinalForce, FColor::Purple, false, 0.0f, 0, 1.0f);
        if (bFoundSurface)
        {
            DrawDebugPoint(GetWorld(), SurfaceHitPoint, 8.0f, FColor::Cyan, false, 0.0f);
            DrawDebugLine(GetWorld(), SafeStartPoint, SurfaceHitPoint, FColor::Green, false, 0.0f);
        }
    }

    OutForce = FinalForce;
    OutTorque = FinalTorque;

    return HitCount > 0;
}

bool UHapticProbeComponent::FindSurfacePointFromRay(const FVector& StartPoint, const FVector& EndPoint,
	FVector& OutSurfacePoint)
{
	if (!SDFProvider) return false;

	// 1. 初始化射线参数
	FVector Direction = EndPoint - StartPoint;
	float TotalDistance = Direction.Size();
    
	// 如果起点和终点重合，无法定义射线
	if (TotalDistance < KINDA_SMALL_NUMBER) return false;

	Direction /= TotalDistance; // 归一化

	// 2. 步进参数
	float CurrentDist = 0.0f;
	const int32 MaxSteps = 32; // 最大迭代次数 (防止死循环，通常10-20次就够了)
	const float VoxelSize = SDFProvider->GetVoxelSize();
	const float SurfaceThreshold = VoxelSize * 0.5f; // 认为接触表面的阈值 (半个体素)

	FVector CurrentPos = StartPoint;

	// 3. Sphere Tracing 循环
	for (int32 i = 0; i < MaxSteps; i++)
	{
		// 转换到体素空间采样
		FVector VoxelCoord;
		if (!SDFProvider->WorldToVoxelSpace(CurrentPos, VoxelCoord))
		{
			// 如果跑出了 SDF 体积范围，终止
			return false;
		}

		float SDFVal = SDFProvider->SampleSDF(VoxelCoord);

		// --- 命中检测 ---
		// 如果 SDFVal <= 阈值，说明我们撞到了表面 (或者起点就在内部)
		// 注意：我们假设 StartPoint 是在外部的 (SDF > 0)。
		// 如果 SDFVal 突然变负，说明跨越了表面。
		if (SDFVal <= SurfaceThreshold)
		{
			// 找到表面！
			// 为了更精确，可以做一个简单的二分回退，但对于触觉来说，当前位置已经足够好
			OutSurfacePoint = CurrentPos;
            
			// 可视化调试 (可选)
			// DrawDebugPoint(GetWorld(), OutSurfacePoint, 5.0f, FColor::Cyan, false, 0.1f);
            
			return true;
		}

		// --- 步进 ---
		// SDF 的特性：当前点以 SDFVal 为半径的球体内绝对没有物体
		// 所以我们可以安全地向前跳跃 SDFVal 的距离
        
		// 限制最小步长，防止在表面附近无限逼近导致死循环
		// 限制最大步长，防止直接穿透薄壁物体
		float StepSize = FMath::Max(SDFVal, VoxelSize * 0.1f); 
        
		CurrentDist += StepSize;

		// 如果步进超过了终点，说明射线路径上没有阻挡 (未发生穿透)
		if (CurrentDist >= TotalDistance)
		{
			return false; 
		}

		CurrentPos = StartPoint + Direction * CurrentDist;
	}

	return false;
}


void UHapticProbeComponent::UpdateProbeMesh()
{
	ProbeMeshComp = Cast<UStaticMeshComponent>(VisualMeshRef.GetComponent(GetOwner()));
	// 如果没选，尝试回退到 Owner 自身的组件 (可选逻辑)
	if (!ProbeMeshComp)
	{
		AActor* Owner = GetOwner();
		if (Owner)
		{
			ProbeMeshComp = Owner->FindComponentByClass<UStaticMeshComponent>();
		}
	}
	
	if (!ProbeMeshComp)
	{
		UE_LOG(LogTemp, Warning, TEXT("HapticProbe: No valid StaticMeshComponent found."));
		return;
	}
	
	UStaticMesh* MeshAsset = ProbeMeshComp->GetStaticMesh();
	if (!MeshAsset)
	{
		UE_LOG(LogTemp, Warning, TEXT("HapticProbe: Assigned component has no StaticMesh asset."));
		return;
	}

	// 清空旧数据
	LocalSamplePoints.Empty();

	GenerateUniformSurfacePoints(MeshAsset, SamplingDensity);	
	

	UE_LOG(LogTemp, Log, TEXT("Generated %d sample points for probe."), LocalSamplePoints.Num());
}


void UHapticProbeComponent::GenerateUniformSurfacePoints(const UStaticMesh* Mesh, float Density)
{	
    if (!Mesh || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.Num() == 0)
    {
        return ;
    }
	
	LocalSamplePoints.Reset();

    // 获取 LOD0 数据
    const FStaticMeshLODResources& LODModel = Mesh->GetRenderData()->LODResources[0];
    const FPositionVertexBuffer& VertexBuffer = LODModel.VertexBuffers.PositionVertexBuffer;
    const FRawStaticIndexBuffer& IndexBuffer = LODModel.IndexBuffer;

    // 1. 计算总面积 (用于估算目标点数)
    double TotalArea = 0.0;
    int32 NumTriangles = IndexBuffer.GetNumIndices() / 3;
    
    // 为了性能，先预计算所有三角形的面积
    TArray<double> TriAreas;
    TriAreas.Reserve(NumTriangles);

    for (int32 i = 0; i < NumTriangles; i++)
    {
        FVector V0 = FVector(VertexBuffer.VertexPosition(IndexBuffer.GetIndex(i * 3 + 0)));
        FVector V1 = FVector(VertexBuffer.VertexPosition(IndexBuffer.GetIndex(i * 3 + 1)));
        FVector V2 = FVector(VertexBuffer.VertexPosition(IndexBuffer.GetIndex(i * 3 + 2)));

        // 三角形面积 = 0.5 * |(V1-V0) x (V2-V0)|
        double Area = 0.5 * FVector::CrossProduct(V1 - V0, V2 - V0).Size();
        TriAreas.Add(Area);
        TotalArea += Area;
    }

    // 2. 计算目标点数
    // 如果 Density < 1 (例如 0.1)，这里也能算出正确的目标总数
    int32 TargetCount = FMath::RoundToInt(TotalArea * Density);
    if (TargetCount <= 0) return;

	if (TargetCount > 0)
	{
		LocalSamplePoints.Reserve(TargetCount);
	}
	
    // 3. 生成候选点 (Oversampling)
    // 为了让分布均匀，我们生成比目标多得多的点 (例如 10 倍)，然后从中筛选
    // 这样可以保证我们有足够的“备胎”来填补空隙，同时剔除太近的点
    int32 NumCandidates = TargetCount * 10; 
    TArray<FVector> Candidates;
    Candidates.Reserve(NumCandidates);

    // 使用 "累积器" 算法来分配候选点，完美解决小三角形和低密度问题
    double CurrentAreaAccumulator = 0.0;
    // 这里的 Step 是指：每隔多少面积生成一个候选点
    double AreaStep = TotalArea / (double)NumCandidates; 
    // 随机初始偏移，避免每次都从第一个三角形的顶点开始
    double CurrentThreshold = FMath::FRandRange(0.0, AreaStep); 

    for (int32 i = 0; i < NumTriangles; i++)
    {
        CurrentAreaAccumulator += TriAreas[i];

        // 如果当前累积面积超过了阈值，就生成点
        while (CurrentAreaAccumulator >= CurrentThreshold)
        {
            // 在当前三角形内随机采样
            FVector V0 = FVector(VertexBuffer.VertexPosition(IndexBuffer.GetIndex(i * 3 + 0)));
            FVector V1 = FVector(VertexBuffer.VertexPosition(IndexBuffer.GetIndex(i * 3 + 1)));
            FVector V2 = FVector(VertexBuffer.VertexPosition(IndexBuffer.GetIndex(i * 3 + 2)));
            
            // 虚幻内置的重心坐标随机采样
            Candidates.Add(GetRandomPointInTriangle(V0, V1, V2));

            // 推进阈值
            CurrentThreshold += AreaStep;
        }
    }

    // 4. 泊松盘筛选 (Poisson Disk Rejection)
    // 计算理想的最小距离。
    // 理论上均匀分布的间距 r ~= sqrt(Area / N)
    float IdealRadius = FMath::Sqrt(TotalArea / TargetCount);
    float RejectDistSq = FMath::Square(IdealRadius * SamplingMinSpacing);

    // 随机打乱候选点顺序，避免扫描线纹理
    // (虽然上面的采样已经是随机的，但打乱一下更保险)
    int32 LastIndex = Candidates.Num() - 1;
    for (int32 i = 0; i <= LastIndex; ++i)
    {
        int32 Index = FMath::RandRange(i, LastIndex);
        if (i != Index) Candidates.Swap(i, Index);
    }

    // 筛选
    for (const FVector& Cand : Candidates)
    {
        // 如果已经凑够了，停止
        if (LocalSamplePoints.Num() >= TargetCount) break;

        bool bTooClose = false;

        for (const FVector& Existing : LocalSamplePoints)
        {
            if (FVector::DistSquared(Cand, Existing) < RejectDistSq)
            {
                bTooClose = true;
                break;
            }
        }

        if (!bTooClose)
        {
            LocalSamplePoints.Add(Cand);
        }
    }
}

FVector UHapticProbeComponent::GetRandomPointInTriangle(const FVector& A, const FVector& B, const FVector& C)
{
	// 使用重心坐标均匀采样
	// r1, r2 是 [0, 1] 的随机数
	float r1 = FMath::FRand();
	float r2 = FMath::FRand();

	// 如果点落在了平行四边形的另一半，将其折叠回三角形内
	if (r1 + r2 > 1.0f)
	{
		r1 = 1.0f - r1;
		r2 = 1.0f - r2;
	}
	
	return A + r1 * (B - A) + r2 * (C - A);
}

// Called every frame
void UHapticProbeComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                          FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

