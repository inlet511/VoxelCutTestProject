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
    // ---- 1. 精确表面位置插值 ----
    float t = PrevSampledValue / (PrevSampledValue - SampledValue + 0.000001);
    float3 RefinedWorldPos = lerp(PrevWorldPos, CurrentWorldPos, t);

    float3 RefinedLocalPos = mul(float4(RefinedWorldPos, 1.0), (WSDemote(GetWorldToLocal(Parameters)))).xyz;
    float3 RefinedUVW = (RefinedLocalPos - LocalBoundsMin) / LocalBoundsSize;

    // ---- 2. 获取材质ID ----
    float RawMatVal = Texture3DSample(VolumeTex, VolumeTexSampler, saturate(RefinedUVW)).y;
    int MatID = round(RawMatVal);

    // ---- 3. 计算法线 ----
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

    // ==========================================================
// --- 4. 材质参数化 (根据 ID 配置物理属性) ---
// ==========================================================

// 1. 初始化默认参数 (作为 Fallback)
float3 Mat_BaseColor = BaseColor.rgb; 
float Mat_Roughness = 0.4;
float Mat_SpecIntensity = 1.0;
float Mat_SSS_Strength = 0.0; 
float Mat_Reflection = 0.0;   
float Mat_Metalness = 0.0;    

// 2. 根据 ID 覆写参数
if (MatID == 0) // Enamel (牙釉质) - 半透明、光滑、类似陶瓷/玻璃
{ 
    // 牙釉质通常是乳白色或略带蓝/灰的透明感
    Mat_BaseColor = float3(0.9, 0.9, 0.88); 
    Mat_Roughness = 0.15; // 非常光滑，有湿润感
    Mat_SpecIntensity = 1.0;
    Mat_SSS_Strength = 0.4; // 适度的透光
    Mat_Reflection = 0.2;   // 牙釉质表面有微弱的环境反射
}
else if (MatID == 1) // Dentin (牙本质) - 不透明、偏黄、有机质感
{         
    // 牙本质位于釉质下方，通常呈淡黄色/骨色
    Mat_BaseColor = float3(0.85, 0.75, 0.55); 
    Mat_Roughness = 0.5;  // 相对粗糙，没有釉质那么亮
    Mat_SpecIntensity = 0.4;
    Mat_SSS_Strength = 1.0; // 强烈的次表面散射，产生温暖的辉光
}
else if (MatID == 2) // Caries (龋坏) - 黑色/深褐色、粗糙、无光泽
{
    // 腐坏组织吸收光线，表面凹凸不平
    Mat_BaseColor = float3(0.15, 0.1, 0.05); // 深褐色/黑色
    Mat_Roughness = 0.9;  // 极度粗糙，几乎无高光
    Mat_SpecIntensity = 0.1;
    Mat_SSS_Strength = 0.0; // 坏死组织不透光
}
else if (MatID == 3) // Fill (金属填充物) - 银汞合金/金
{
    // 金属特性：BaseColor决定反射色，无Diffuse
    Mat_BaseColor = float3(0.7, 0.7, 0.75); // 银灰色
    Mat_Roughness = 0.2; // 抛光金属
    Mat_SpecIntensity = 1.0;
    Mat_Reflection = 1.0; // 强环境反射
    Mat_Metalness = 1.0;  // 开启金属流程
}

// ==========================================================
// --- 5. 统一光照计算 (使用 Mat_ 变量) ---
// ==========================================================

float3 L = normalize(LightDir.xyz);           
float3 V = normalize(-WorldSpaceRayDir);      
float3 N = SurfaceNormal;                     
float3 H = normalize(L + V);                  

float NdotL = saturate(dot(N, L));
float NdotV = max(0.0, dot(N, V));
float NdotH = max(0.0, dot(N, H));

// --- A. 漫反射 (Diffuse) & SSS ---
// 金属没有漫反射，非金属有
// 牙齿使用 SSS 替代简单的 Lambert Diffuse
float wrap = 0.5; 
float NdotL_SSS = saturate((dot(N, L) + wrap) / (1.0 + wrap));

// SSS 颜色倾向：牙齿透光通常偏暖色 (红/橙)
float3 SSS_Tint = float3(1.0, 0.8, 0.7); 
float3 DiffuseColor = Mat_BaseColor * SSS_Tint * NdotL_SSS * Mat_SSS_Strength;

// 标准 Diffuse (用于不透光部分或低 SSS 部分)
float3 LambertDiffuse = Mat_BaseColor * NdotL;

// 混合 SSS 和 标准 Diffuse，并根据金属度屏蔽
// 如果 Metalness 为 1，Diffuse 变为 0
float3 FinalDiffuse = lerp(lerp(LambertDiffuse, DiffuseColor, Mat_SSS_Strength), float3(0,0,0), Mat_Metalness);


// --- B. 菲涅尔 (Fresnel F0) ---
// 关键：绝缘体(牙齿) F0 固定为 0.04-0.05，金属使用 BaseColor
float3 F0 = lerp(float3(0.05, 0.05, 0.05), Mat_BaseColor, Mat_Metalness);
float3 F = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);


// --- C. 高光 (Specular) ---
// 使用 Mat_Roughness 控制高光锐度
// 牙齿通常有双层高光(口水层+牙体)，这里为了统一，使用基于 Roughness 的计算
float SpecPower = 2.0 / (Mat_Roughness * Mat_Roughness + 0.001) - 2.0;
float SpecTerm = pow(NdotH, SpecPower);

// 金属的高光通常带有自身颜色(F)，非金属高光是白色的(但在PBR中通常由F项处理颜色)
// 这里简单处理：乘以 SpecularColor 和 F
float3 SpecularFinal = SpecTerm * SpecularColor.rgb * Mat_SpecIntensity * F;


// --- D. 边缘光 (Rim) ---
// 仅对非金属(牙齿)生效，增强体积感
float RimExp = 4.0;
float RimTerm = pow(1.0 - NdotV, RimExp) * RimBoost;
float3 RimColorFinal = RimColor * RimTerm * (1.0 - Mat_Metalness); 


// --- E. 环境反射 (Reflection) ---
float3 R = reflect(-V, N);
float3 EnvColor = TextureCubeSample(EnvMap, EnvMapSampler, R).rgb;

// 混合反射：金属反射强，牙釉质反射弱
// 使用 Mat_Reflection 控制强度，F 控制菲涅尔现象
float3 ReflectionFinal = EnvColor * F * (Mat_Reflection * ReflectionIntensity);


// --- 最终合成 ---
SurfaceColor = (AmbientColor.xyz * 0.3) + FinalDiffuse + SpecularFinal + RimColorFinal + ReflectionFinal;

return float4(SurfaceColor, 1.0);
}
else {
    return float4(0,0,0,0);
}