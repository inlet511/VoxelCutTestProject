bool HitSurface = false;
float3 SurfaceNormal = float3(0, 0, 1);
float3 SurfaceColor = float3(0, 0, 0);

// --- Step 0. 准备基础数据 ---
float3 LocalBoundsMin = GetPrimitiveData(Parameters).LocalObjectBoundsMin;
float3 LocalBoundsMax = GetPrimitiveData(Parameters).LocalObjectBoundsMax;
float3 LocalBoundsSize = LocalBoundsMax - LocalBoundsMin;

float3 WorldCameraPos = LWCHackToFloat(PrimaryView.WorldCameraOrigin);
float3 WorldSpaceRayDir = normalize(-CameraVector); 

// --- Step 1. 射线-包围盒相交检测 ---
float3 LocalRayOrigin = mul(float4(WorldCameraPos, 1.0), WSDemote(GetWorldToLocal(Parameters))).xyz;
float3 LocalRayDir = mul(float4(WorldSpaceRayDir, 0.0), WSDemote(GetWorldToLocal(Parameters))).xyz;

float3 InvRayDir = 1.0 / (LocalRayDir + 0.000001);
float3 t0 = (LocalBoundsMin - LocalRayOrigin) * InvRayDir;
float3 t1 = (LocalBoundsMax - LocalRayOrigin) * InvRayDir;
float3 tmin = min(t0, t1);
float3 tmax = max(t0, t1);

float tBoxEntry = max(max(tmin.x, tmin.y), tmin.z);
float tBoxExit = min(min(tmax.x, tmax.y), tmax.z);

[branch]
if (tBoxEntry > tBoxExit || tBoxExit < 0.0) {
    return float4(0, 0, 0, 0);
}

float StartDistance = max(0.0, tBoxEntry);
float3 CurrentWorldPos = WorldCameraPos + WorldSpaceRayDir * StartDistance;
float CurrentDist = StartDistance;

float3 ViewSpaceRayDir = mul(float4(WorldSpaceRayDir, 0.0), PrimaryView.TranslatedWorldToView).xyz;
float DepthStep = ViewSpaceRayDir.z * StepSize;

float3 TranslatedStartPos = CurrentWorldPos + LWCHackToFloat(PrimaryView.PreViewTranslation);
float4 ViewStartPos = mul(float4(TranslatedStartPos, 1.0), PrimaryView.TranslatedWorldToView);
float CurrentDepth = ViewStartPos.z;

float2 ScreenUV = MaterialFloat2(ScreenAlignedPosition(Parameters.ScreenPosition).xy);
float SceneDepthLinear = CalcSceneDepth(ScreenUV);

float PrevSampledValue = 1.0; 
float3 PrevWorldPos = CurrentWorldPos;
float SampledValue = 1.0;

// --- 循环开始 ---
[loop]
for (int i = 0; i < MaxSteps; i++) {
    [branch]
    if (CurrentDist > tBoxExit) break; 

    [branch]
    if (CurrentDepth > SceneDepthLinear) return float4(0, 0, 0, 0); 

    float3 LocalPos = mul(float4(CurrentWorldPos, 1.0), (WSDemote(GetWorldToLocal(Parameters)))).xyz;
    float3 UVW = (LocalPos - LocalBoundsMin) / LocalBoundsSize;

    [flatten]
    if (all(UVW >= 0.0) && all(UVW <= 1.0)) {
        SampledValue = Texture3DSample(VolumeTex, VolumeTexSampler, UVW).x;
    } else {
        SampledValue = 1.0;
    }

    [branch]
    if (SampledValue <= 0) {
        HitSurface = true;
        break;
    }

    PrevSampledValue = SampledValue;
    PrevWorldPos = CurrentWorldPos;

    CurrentDepth += DepthStep;
    CurrentWorldPos += WorldSpaceRayDir * StepSize;
    CurrentDist += StepSize;
}

[branch]
if (HitSurface) {
    // 精确表面位置插值
    float t = PrevSampledValue / (PrevSampledValue - SampledValue + 0.000001);
    float3 RefinedWorldPos = lerp(PrevWorldPos, CurrentWorldPos, t);

    float3 RefinedLocalPos = mul(float4(RefinedWorldPos, 1.0), (WSDemote(GetWorldToLocal(Parameters)))).xyz;
    float3 RefinedUVW = (RefinedLocalPos - LocalBoundsMin) / LocalBoundsSize;

    float3 NOffset = float3(NormalOffset, 0, 0); 

    // 梯度计算法线
    float valX_Pos = Texture3DSample(VolumeTex, VolumeTexSampler, saturate(RefinedUVW + NOffset.xyy)).x;
    float valX_Neg = Texture3DSample(VolumeTex, VolumeTexSampler, saturate(RefinedUVW - NOffset.xyy)).x;
    float gradX = valX_Pos - valX_Neg;

    float valY_Pos = Texture3DSample(VolumeTex, VolumeTexSampler, saturate(RefinedUVW + NOffset.yxy)).x;
    float valY_Neg = Texture3DSample(VolumeTex, VolumeTexSampler, saturate(RefinedUVW - NOffset.yxy)).x;
    float gradY = valY_Pos - valY_Neg;

    float valZ_Pos = Texture3DSample(VolumeTex, VolumeTexSampler, saturate(RefinedUVW + NOffset.yyx)).x;
    float valZ_Neg = Texture3DSample(VolumeTex, VolumeTexSampler, saturate(RefinedUVW - NOffset.yyx)).x;
    float gradZ = valZ_Pos - valZ_Neg;

    SurfaceNormal = normalize(float3(gradX, gradY, gradZ));

    // --- 光照计算 ---
    float3 L = normalize(LightDir.xyz);           
    float3 V = normalize(-WorldSpaceRayDir);      
    float3 N = SurfaceNormal;                     
    float3 H = normalize(L + V);                  

    // B. 次表面散射 (SSS)
    float wrap = 0.5; 
    float NdotL = dot(N, L);
    float NdotL_SSS = saturate((NdotL + wrap) / (1.0 + wrap));
    float3 SSSColor = BaseColor.xyz * float3(1.0, 0.8, 0.7); 
    float3 Diffuse = SSSColor * NdotL_SSS;

    // C. 菲涅尔 (Fresnel)
    float3 F0 = float3(0.05, 0.05, 0.05); 
    float NdotV = max(0.0, dot(N, V));
    float3 F = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);

    // D. 双层高光
    float RoughnessInner = 0.4;
    float SpecPowerInner = 2.0 / (RoughnessInner * RoughnessInner) - 2.0;
    float NdotH = max(0.0, dot(N, H));
    float SpecInnerTerm = pow(NdotH, SpecPowerInner) * 0.5;

    float RoughnessOuter = 0.05; 
    float SpecPowerOuter = 2.0 / (RoughnessOuter * RoughnessOuter) - 2.0;
    float SpecOuterTerm = pow(NdotH, SpecPowerOuter) * 2.0; 

    float3 SpecularFinal = (SpecInnerTerm + SpecOuterTerm) * SpecularColor.xyz * F * SpecularIntensity;

    // E. 边缘光
    float RimExp = 4.0;
    float RimTerm = pow(1.0 - NdotV, RimExp) * RimBoost;
    float3 RimColorFinal = RimColor * RimTerm * 0.5; 

    // --- F. 环境反射 (Cubemap Reflection) ---
    // 计算反射向量
    float3 R = reflect(-V, N);

    float3 EnvColor = TextureCubeSample(EnvMap, EnvMapSampler, R).rgb;

    // 混合反射：
    // 1. 使用菲涅尔项 F：边缘反射强，中心反射弱 (符合物理规律)
    // 2. 乘以 ReflectionIntensity 用于美术控制
    float3 ReflectionFinal = EnvColor * F * ReflectionIntensity;

    // --- 最终合成 ---
    SurfaceColor = (AmbientColor.xyz * 0.5) + Diffuse + SpecularFinal + RimColorFinal + ReflectionFinal;

    return float4(SurfaceColor, 1.0);
}
else {
    return float4(0,0,0,0);
}