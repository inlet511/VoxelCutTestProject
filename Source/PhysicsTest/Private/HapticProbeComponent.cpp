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

/*
bool UHapticProbeComponent::CalculateForce(FVector& OutForce, FVector& OutTorque)
{
	if (!SDFProvider) return false;

	// [开始计时] 获取高精度当前时间
	double StartTime = FPlatformTime::Seconds();
	
	// 1. 获取线程锁 (读锁)
	// 这一点至关重要！因为 Cutter 可能正在后台线程写入数据
	FRWScopeLock ReadLock(SDFProvider->GetDataLock(), SLT_ReadOnly);

	FVector TotalForce = FVector::ZeroVector;
	FVector TotalTorque = FVector::ZeroVector;
	int32 Hits = 0;

	// 获取Probe组件的世界变换
	FTransform ProbeCompTransform = ProbeMeshComp->GetComponentTransform();
	float VoxelSize = SDFProvider->GetVoxelSize();

	for (const FVector& LocalPt : LocalSamplePoints)
	{
		// 1. 变换点到世界空间
		FVector WorldPt = ProbeCompTransform.TransformPosition(LocalPt);

		// 2. 转换到体素空间 (利用接口)
		FVector VoxelCoord;
		bool bIsValid = false;
		
		if (SDFProvider)
		{
			bIsValid = SDFProvider->WorldToVoxelSpace(WorldPt, VoxelCoord);
		}
		if (!bIsValid)
		{
			continue; // 超出 Volume 边界
		}

		// 3. 采样 SDF
		float SDFVal = SDFProvider->SampleSDF(VoxelCoord);

		// =========================================================
		// 可视化调试
		// =========================================================
		if (bVisualizeSamplePoints)
		{
			// 颜色逻辑：
			// 红色 = 钻入物体内部 (SDF < 0)
			// 绿色 = 在物体外部 (SDF >= 0)
			// 黄色 = 无效区域 (超出包围盒)
			FColor DebugColor = FColor::Green;

			if (!bIsValid)
			{
				DebugColor = FColor::Yellow; // 警告：点在模型包围盒外
			}
			else if (SDFVal < 0.0f)
			{
				DebugColor = FColor::Red;    // 碰撞！
			}

			// 绘制点
			DrawDebugPoint(GetWorld(), WorldPt, DebugPointSize, DebugColor, false, 0.0f);
		}
		
		
		if (SDFVal < 0.0f) // 碰撞！
		{
			// 计算深度 (cm)
			float Depth = -SDFVal * VoxelSize;

			// 计算梯度 (力方向)
			const float H = 1.0f; 

			// 2. 采样 6 个邻居点的 SDF 值
			// 注意：SampleSDF 内部必须处理边界检查(Clamp)，防止数组越界！
			float DistX_Pos = SDFProvider->SampleSDF(VoxelCoord + FVector(H, 0, 0));
			float DistX_Neg = SDFProvider->SampleSDF(VoxelCoord - FVector(H, 0, 0));

			float DistY_Pos = SDFProvider->SampleSDF(VoxelCoord + FVector(0, H, 0));
			float DistY_Neg = SDFProvider->SampleSDF(VoxelCoord - FVector(0, H, 0));

			float DistZ_Pos = SDFProvider->SampleSDF(VoxelCoord + FVector(0, 0, H));
			float DistZ_Neg = SDFProvider->SampleSDF(VoxelCoord - FVector(0, 0, H));

			// 3. 组合梯度向量
			// 公式: f'(x) ≈ (f(x+h) - f(x-h)) / 2h
			// 因为我们只需要方向（后面会 Normalize），所以除以 2h 可以省略
			FVector Gradient;
			Gradient.X = DistX_Pos - DistX_Neg;
			Gradient.Y = DistY_Pos - DistY_Neg;
			Gradient.Z = DistZ_Pos - DistZ_Neg;

			// 4. 归一化
			// SDF 的梯度方向指向距离增加的方向（即指向物体外部）
			// GetSafeNormal() 会自动处理零向量的情况
			Gradient = Gradient.GetSafeNormal();
			
			
            
			// 计算材质
			int32 MatID = SDFProvider->SampleMaterialID(VoxelCoord);
			float MatScale = MaterialStiffnessScales.FindRef(MatID);
			if(MatScale <= 0) MatScale = 1.0f;

			// 累加力
			FVector PointForce = Gradient * (Depth * BaseStiffness * MatScale);
			TotalForce += PointForce;
            
			// 累加力矩
			FVector Arm = WorldPt - ProbeCompTransform.GetLocation();
			TotalTorque += FVector::CrossProduct(Arm, PointForce);

			Hits++;
		}
	}
	
	if (bLogCalcTime)
	{
		// [结束计时]
		double EndTime = FPlatformTime::Seconds();
    
		// 转换为毫秒 (ms)
		double DurationMs = (EndTime - StartTime) * 1000.0;
		static int32 DebugLogCounter = 0;
		if (DebugLogCounter++ % 100 == 0) // 每100次调用才打印一次
		{
			UE_LOG(LogTemp, Warning, TEXT("CalculateForce Cost: %.4f ms"), DurationMs);
		}
	}
	

	OutForce = TotalForce;
	OutTorque = TotalTorque;
	
	if (bVisualizeForce)
	{
		FVector Start = ProbeMeshComp->GetComponentLocation();
		FVector ForceEnd = Start + TotalForce * .1f;
		FVector TorqueEnd = Start + TotalTorque * .1f;
		DrawDebugLine(GetWorld(), Start, ForceEnd, FColor::Purple,false);
		DrawDebugLine(GetWorld(), Start, TorqueEnd, FColor::Cyan,false);
	}
	
	return Hits > 0;
}

*/

bool UHapticProbeComponent::CalculateForce(FVector& OutForce, FVector& OutTorque)
{
    if (!SDFProvider) return false;

    // [开始计时]
    double StartTime = FPlatformTime::Seconds();

    // 1. 获取读锁 (保持不变，确保并行期间数据不被修改)
    FRWScopeLock ReadLock(SDFProvider->GetDataLock(), SLT_ReadOnly);

    FVector FinalForce = FVector::ZeroVector;
    FVector FinalTorque = FVector::ZeroVector;
    int32 TotalHits = 0;

    // 获取公共数据 (避免在循环内重复调用函数)
    const FTransform ProbeCompTransform = ProbeMeshComp->GetComponentTransform();
    const FVector ProbeLocation = ProbeCompTransform.GetLocation();
    const float VoxelSize = SDFProvider->GetVoxelSize();
    const int32 NumPoints = LocalSamplePoints.Num();

    // 阈值：如果点数太少(例如少于64个)，开启多线程的开销可能比直接算还大
    // 同时，如果开启了可视化，必须强制单线程
    bool bUseParallel = (NumPoints > 64) && !bVisualizeSamplePoints;

    if (bUseParallel)
    {
        // =========================================================
        // 并行模式 (ParallelFor)
        // =========================================================
        
        // 预分配临时缓冲区，避免锁竞争
        TArray<FVector> TempForces;
        TArray<FVector> TempTorques;
        TArray<uint8> TempHits; // 0 or 1
        
        TempForces.AddZeroed(NumPoints);
        TempTorques.AddZeroed(NumPoints);
        TempHits.AddZeroed(NumPoints);

        ParallelFor(NumPoints, [&](int32 Idx)
        {
            const FVector& LocalPt = LocalSamplePoints[Idx];
            
            // 1. 变换点到世界空间
            FVector WorldPt = ProbeCompTransform.TransformPosition(LocalPt);

            // 2. 转换到体素空间
            FVector VoxelCoord;
            if (SDFProvider->WorldToVoxelSpace(WorldPt, VoxelCoord))
            {
                // 3. 采样 SDF
                float SDFVal = SDFProvider->SampleSDF(VoxelCoord);

                if (SDFVal < 0.0f) // 碰撞！
                {
                    float Depth = -SDFVal * VoxelSize;

                    // --- 梯度计算 (内联以减少开销) ---
                    const float H = 1.0f; 
                    float DX_P = SDFProvider->SampleSDF(VoxelCoord + FVector(H, 0, 0));
                    float DX_N = SDFProvider->SampleSDF(VoxelCoord - FVector(H, 0, 0));
                    float DY_P = SDFProvider->SampleSDF(VoxelCoord + FVector(0, H, 0));
                    float DY_N = SDFProvider->SampleSDF(VoxelCoord - FVector(0, H, 0));
                    float DZ_P = SDFProvider->SampleSDF(VoxelCoord + FVector(0, 0, H));
                    float DZ_N = SDFProvider->SampleSDF(VoxelCoord - FVector(0, 0, H));

                    FVector Gradient(DX_P - DX_N, DY_P - DY_N, DZ_P - DZ_N);
                    Gradient = Gradient.GetSafeNormal();

                    // --- 材质计算 ---
                    int32 MatID = SDFProvider->SampleMaterialID(VoxelCoord);
                    float MatScale = MaterialStiffnessScales.FindRef(MatID);
                    if(MatScale <= 0) MatScale = 1.0f;

                    // --- 计算力 ---
                    FVector PointForce = Gradient * (Depth * BaseStiffness * MatScale);
                    
                    // --- 计算力矩 ---
                    FVector Arm = WorldPt - ProbeLocation;
                    FVector PointTorque = FVector::CrossProduct(Arm, PointForce);

                    // 写入临时数组 (无锁，因为 Index 唯一)
                    TempForces[Idx] = PointForce;
                    TempTorques[Idx] = PointTorque;
                    TempHits[Idx] = 1;
                }
            }
        });

        // Reduce: 汇总结果
        for (int32 i = 0; i < NumPoints; i++)
        {
            if (TempHits[i] > 0)
            {
                FinalForce += TempForces[i];
                FinalTorque += TempTorques[i];
                TotalHits++;
            }
        }
    }
    else
    {
        // =========================================================
        // 串行模式 (用于调试或点数极少时)
        // =========================================================
        for (const FVector& LocalPt : LocalSamplePoints)
        {
            FVector WorldPt = ProbeCompTransform.TransformPosition(LocalPt);
            FVector VoxelCoord;
            
            bool bIsValid = SDFProvider->WorldToVoxelSpace(WorldPt, VoxelCoord);
            if (!bIsValid)
            {
                if (bVisualizeSamplePoints) DrawDebugPoint(GetWorld(), WorldPt, DebugPointSize, FColor::Yellow, false, 0.0f);
                continue;
            }

            float SDFVal = SDFProvider->SampleSDF(VoxelCoord);

            // 可视化逻辑 (仅在串行模式下运行)
            if (bVisualizeSamplePoints)
            {
                FColor DebugColor = (SDFVal < 0.0f) ? FColor::Red : FColor::Green;
                DrawDebugPoint(GetWorld(), WorldPt, DebugPointSize, DebugColor, false, 0.0f);
            }

            if (SDFVal < 0.0f)
            {
                float Depth = -SDFVal * VoxelSize;
                
                // 梯度计算
                const float H = 1.0f; 
                float DX_P = SDFProvider->SampleSDF(VoxelCoord + FVector(H, 0, 0));
                float DX_N = SDFProvider->SampleSDF(VoxelCoord - FVector(H, 0, 0));
                float DY_P = SDFProvider->SampleSDF(VoxelCoord + FVector(0, H, 0));
                float DY_N = SDFProvider->SampleSDF(VoxelCoord - FVector(0, H, 0));
                float DZ_P = SDFProvider->SampleSDF(VoxelCoord + FVector(0, 0, H));
                float DZ_N = SDFProvider->SampleSDF(VoxelCoord - FVector(0, 0, H));

                FVector Gradient(DX_P - DX_N, DY_P - DY_N, DZ_P - DZ_N);
                Gradient = Gradient.GetSafeNormal();

                int32 MatID = SDFProvider->SampleMaterialID(VoxelCoord);
                float MatScale = MaterialStiffnessScales.FindRef(MatID);
                if(MatScale <= 0) MatScale = 1.0f;

                FVector PointForce = Gradient * (Depth * BaseStiffness * MatScale);
                FinalForce += PointForce;
                
                FVector Arm = WorldPt - ProbeLocation;
                FinalTorque += FVector::CrossProduct(Arm, PointForce);

                TotalHits++;
            }
        }
    }

    // [结束计时] & Log
    if (bLogCalcTime)
    {
        double EndTime = FPlatformTime::Seconds();
        double DurationMs = (EndTime - StartTime) * 1000.0;
        static int32 DebugLogCounter = 0;
        if (DebugLogCounter++ % 100 == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("CalculateForce Cost: %.4f ms (Parallel: %s, Points: %d)"), 
                DurationMs, bUseParallel ? TEXT("ON") : TEXT("OFF"), NumPoints);
        }
    }

    OutForce = FinalForce;
    OutTorque = FinalTorque;

    // 力向量可视化 (可以在主线程安全绘制)
    if (bVisualizeForce)
    {
        FVector Start = ProbeLocation;
        FVector ForceEnd = Start + FinalForce;
        DrawDebugLine(GetWorld(), Start, ForceEnd, FColor::Purple, false);
    }

    return TotalHits > 0;
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

