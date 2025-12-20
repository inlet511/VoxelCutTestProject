// ==========================================
// 体积雾 + 多材质颜色 + 深度遮挡 (修复版)
// ==========================================

struct Functions
{
    float HG_Phase(float g, float CosTheta)
    {
        float g2 = g * g;
        float Denom = 1.0 + g2 - 2.0 * g * CosTheta;
        return (1.0 - g2) / (4.0 * 3.14159 * pow(max(0.001, Denom), 1.5));
    }
};
Functions F;

// --- 1. 基础准备 ---
float3 AccumulatedRadiance = float3(0, 0, 0);
float Transmittance = 1.0; 

float3 LocalBoundsMin = GetPrimitiveData(Parameters).LocalObjectBoundsMin;
float3 LocalBoundsMax = GetPrimitiveData(Parameters).LocalObjectBoundsMax;
float3 LocalBoundsSize = LocalBoundsMax - LocalBoundsMin;

float3 WorldCameraPos = LWCHackToFloat(PrimaryView.WorldCameraOrigin);
float3 WorldSpaceRayDir = normalize(-CameraVector); 

// --- 2. 深度遮挡计算 (修复报错部分) ---
// 直接使用从引脚传入的 SceneDepthInput
// 注意：SceneDepth 节点返回的是“相机平面到物体的垂直距离”(View Space Z)
// 我们需要把它转换成“沿着射线方向的距离”
float SceneDepth = SceneDepthInput;

// 计算视线方向和相机朝向的夹角余弦
float CosRayAngle = dot(View.ViewForward, WorldSpaceRayDir);

// 修正距离：把垂直深度投影到射线方向上
// 避免除以0
float MaxRayDist = SceneDepth / max(0.0001, CosRayAngle);

// --- 3. 射线-包围盒相交 ---
float3 LocalRayOrigin = mul(float4(WorldCameraPos, 1.0), WSDemote(GetWorldToLocal(Parameters))).xyz;
float3 LocalRayDir = mul(float4(WorldSpaceRayDir, 0.0), WSDemote(GetWorldToLocal(Parameters))).xyz;

float3 InvRayDir = 1.0 / (LocalRayDir + 0.000001);
float3 t0 = (LocalBoundsMin - LocalRayOrigin) * InvRayDir;
float3 t1 = (LocalBoundsMax - LocalRayOrigin) * InvRayDir;
float3 tmin = min(t0, t1);
float3 tmax = max(t0, t1);

float tBoxEntry = max(max(tmin.x, tmin.y), tmin.z);
float tBoxExit = min(min(tmax.x, tmax.y), tmax.z);

// --- 4. 应用深度截断 ---
// 如果射线穿出盒子的点比墙壁还远，就截断在墙壁处
tBoxExit = min(tBoxExit, MaxRayDist);

// 再次检查：如果起点已经在墙后面了，或者没击中盒子，直接放弃
if (tBoxEntry > tBoxExit || tBoxExit < 0.0) {
    return float4(0, 0, 0, 0);
}

// --- 5. 步进初始化 ---
float StartDistance = max(0.0, tBoxEntry);
float Dither = View.GeneralPurposeTweak; 
float RayOffset = Dither * StepSize; 

float CurrentDist = StartDistance + RayOffset;
float3 CurrentWorldPos = WorldCameraPos + WorldSpaceRayDir * CurrentDist;

float3 L = normalize(LightDir);
float CosTheta = dot(L, WorldSpaceRayDir);

// --- 6. 循环 ---
[loop]
for (int i = 0; i < 128; i++) {
    
    // 退出条件
    if (CurrentDist > tBoxExit || Transmittance < 0.01) break; 

    float3 LocalPos = mul(float4(CurrentWorldPos, 1.0), (WSDemote(GetWorldToLocal(Parameters)))).xyz;
    float3 UVW = (LocalPos - LocalBoundsMin) / LocalBoundsSize;

    float SDFValue = 1.0;
    float MatIDRaw = 0.0;

    [flatten]
    if (all(UVW >= 0.0) && all(UVW <= 1.0)) {
        float2 SampleData = Texture3DSample(VolumeTex, VolumeTexSampler, UVW).rg;
        SDFValue = SampleData.r;
        MatIDRaw = SampleData.g;
    }

    float LocalDensity = saturate(SDFValue / -max(0.001, EdgeSoftness));
    
    [branch]
    if (LocalDensity > 0.001)
    {
        float3 ScatterColor = float3(1, 1, 1);
        int MatID = round(MatIDRaw); 

        if (MatID == 0) ScatterColor = Color_A;
        else if (MatID == 1) ScatterColor = Color_B;
        else if (MatID == 2) ScatterColor = Color_C;
        else if (MatID == 3) ScatterColor = Color_D;
        
        float RealDensity = LocalDensity * DensityScale;
        float ShadowTerm = saturate(exp(SDFValue * 5.0));
        float Phase = F.HG_Phase(0.5, CosTheta);
        
        float3 IncomingLight = LightColor * ShadowTerm * Phase;
        float3 Ambient = float3(0.1, 0.1, 0.15) * ScatterColor; 
        float3 TotalLight = (IncomingLight * ScatterColor) + Ambient;

        float StepTransmittance = exp(-RealDensity * StepSize);
        float3 StepColor = TotalLight * (1.0 - StepTransmittance);
        
        AccumulatedRadiance += StepColor * Transmittance;
        Transmittance *= StepTransmittance;
    }

    CurrentWorldPos += WorldSpaceRayDir * StepSize;
    CurrentDist += StepSize;
}

float FinalAlpha = 1.0 - Transmittance;
return float4(AccumulatedRadiance, FinalAlpha);