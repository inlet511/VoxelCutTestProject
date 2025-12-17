bool HitSurface = false;
float3 SurfaceNormal = float3(0, 0, 1);
float3 SurfaceColor = float3(0, 0, 0);
float TotalDistance = 0;

float3 LocalBoundsMin = GetPrimitiveData(Parameters).LocalObjectBoundsMin;
float3 LocalBoundsMax = GetPrimitiveData(Parameters).LocalObjectBoundsMax;
float3 LocalBoundsSize = LocalBoundsMax - LocalBoundsMin;

// Step1. 计算ViewSpace下的射线起始位置和射线方向
float3 TranslatedStartPos = WorldPosition + LWCHackToFloat(PrimaryView.PreViewTranslation); // 加上预偏移量
// 射线起始位置，ViewSpace
float4 ViewStartPos = mul(float4(TranslatedStartPos, 1.0), PrimaryView.TranslatedWorldToView);

// 世界空间射线方向
float3 WorldSpaceRayDir = normalize(-CameraVector); // 射线方向
// 计算 ViewSpace 下的步进增量
// 注意：向量转换时 w 分量为 0，因为方向不受平移影响
float3 ViewSpaceRayDir = mul(float4(WorldSpaceRayDir, 0.0), PrimaryView.TranslatedWorldToView).xyz;
// ViewSpace步进量
float DepthStep = ViewSpaceRayDir.z * StepSize;

// Step2. 计算现有场景深度线性深度(ViewSpace)
float2 ScreenUV = MaterialFloat2(ScreenAlignedPosition(Parameters.ScreenPosition).xy);
// 线性场景深度
float SceneDepthLinear = CalcSceneDepth(ScreenUV);

// 初始化循环变量
float CurrentDepth = ViewStartPos.z;    // 当前点的 ViewSpace 深度
float3 CurrentWorldPos = WorldPosition; // 当前点的世界坐标 (用于采样纹理)

float PrevSampledValue = 1.0; // 假设起始点在空气中(密度>0)
float3 PrevWorldPos = CurrentWorldPos;

float SampledValue = 1.0;

for (int i = 0; i < MaxSteps; i++) {
    // 当前点的深度如果超过了场景深度(在View空间比较)，则返回0
    if (CurrentDepth > SceneDepthLinear) {
        return float4(0, 0, 0, 0);
    }

    // 当前世界位置转换为局部位置
    float3 LocalPos = mul(float4(CurrentWorldPos, 1.0), (WSDemote(GetWorldToLocal(Parameters)))).xyz;
    float3 UVW = (LocalPos - LocalBoundsMin) / LocalBoundsSize;

    // 2. 只有在 UVW 范围内才真正采样纹理
    // 这样既避免了四方连续(Tiling)，又避免了边界闪烁
    bool bIsInside = all(UVW >= 0.0) && all(UVW <= 1.0);

    if (bIsInside) {
        SampledValue = Texture3DSample(VolumeTex, VolumeTexSampler, UVW).x;
    }

    if (SampledValue <= 0) {
        HitSurface = true;
        break;
    }

    // --- 循环末尾：更新"上一步"的数据 ---
    PrevSampledValue = SampledValue;
    PrevWorldPos = CurrentWorldPos;

    // --- B. 步进 ---
    // 1. 线性累加深度
    CurrentDepth += DepthStep;

    // 2. 累加世界坐标
    CurrentWorldPos += WorldSpaceRayDir * StepSize;
}

if (HitSurface) {
    // [核心优化]：线性插值计算精确表面位置
    // 公式推导：我们想找 t，使得 lerp(PrevVal, CurrVal, t) == 0
    // 结果：t = PrevVal / (PrevVal - CurrVal)
    // 注意防止除以0 (虽然理论上跨越0点不会分母为0，但为了安全加个极小值)
    float t = PrevSampledValue / (PrevSampledValue - SampledValue + 0.000001);

    // 获取平滑后的世界坐标
    float3 RefinedWorldPos = lerp(PrevWorldPos, CurrentWorldPos, t);

    // 重新计算平滑后的 UVW，用于法线采样
    float3 RefinedLocalPos = mul(float4(RefinedWorldPos, 1.0), (WSDemote(GetWorldToLocal(Parameters)))).xyz;
    float3 RefinedUVW = (RefinedLocalPos - LocalBoundsMin) / LocalBoundsSize;

    float3 NOffset = float3(NormalOffset, 0, 0); // 稍微加大一点 Offset 可以获得更平滑的法线

    // X轴梯度: (右 - 左)
    float valX_Pos = Texture3DSample(VolumeTex, VolumeTexSampler, saturate(RefinedUVW + NOffset.xyy)).x;
    float valX_Neg = Texture3DSample(VolumeTex, VolumeTexSampler, saturate(RefinedUVW - NOffset.xyy)).x;
    float gradX = valX_Pos - valX_Neg;

    // Y轴梯度: (前 - 后)
    float valY_Pos = Texture3DSample(VolumeTex, VolumeTexSampler, saturate(RefinedUVW + NOffset.yxy)).x;
    float valY_Neg = Texture3DSample(VolumeTex, VolumeTexSampler, saturate(RefinedUVW - NOffset.yxy)).x;
    float gradY = valY_Pos - valY_Neg;

    // Z轴梯度: (上 - 下)
    float valZ_Pos = Texture3DSample(VolumeTex, VolumeTexSampler, saturate(RefinedUVW + NOffset.yyx)).x;
    float valZ_Neg = Texture3DSample(VolumeTex, VolumeTexSampler, saturate(RefinedUVW - NOffset.yyx)).x;
    float gradZ = valZ_Pos - valZ_Neg;

    SurfaceNormal = normalize(float3(gradX, gradY, gradZ));

   // --- 3. 高级光照计算 (Advanced Lighting) ---
    
    // A. 准备向量
    float3 L = normalize(LightDir.xyz);           // 光源方向
    float3 V = normalize(-WorldSpaceRayDir);      // 视线方向 (从表面指向相机)
    float3 N = SurfaceNormal;                     // 法线
    float3 H = normalize(L + V);                  // 半角向量 (Blinn-Phong 核心)

    // B. 漫反射 (Diffuse)
    float NdotL = max(0.0, dot(N, L));
    float3 Diffuse = BaseColor.xyz * NdotL;

    // C. 高光 (Specular) - Blinn-Phong
    // 建议将这些硬编码数值改为 Custom Node 的输入引脚
    float SpecPower = 32.0;       // 高光范围：值越大，光斑越小越锐利
    float NdotH = max(0.0, dot(N, H));
    float SpecTerm = pow(NdotH, SpecPower) * SpecularIntensity;
    float3 Specular = SpecularColor.xyz * SpecTerm;

    // D. 边缘光 (Rim Light) -
    // 原理：当法线 N 和视线 V 接近垂直时 (dot接近0)，边缘发光
    float RimExp = 3.0;           // 边缘衰减：值越大，边缘光越细
    float RimBoost = 1.5;         // 边缘光强度
    float NdotV = max(0.0, dot(N, V));
    float RimTerm = pow(1.0 - NdotV, RimExp) * RimBoost;
    // 边缘光颜色通常比 BaseColor 更亮，或者稍微偏冷色调
    float3 RimColorFinal = RimColor.xyz * RimTerm; 

    // --- 4. 最终合成 ---
    SurfaceColor = AmbientColor.xyz + Diffuse + Specular + RimColorFinal;

    return float4(SurfaceColor, 1.0);
}
else {
    return 0;
}
