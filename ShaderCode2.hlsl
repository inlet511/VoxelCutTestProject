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
float CurrentDepth = ViewStartPos.z; // 当前点的 ViewSpace 深度
float3 CurrentWorldPos = WorldPosition;   // 当前点的世界坐标 (用于采样纹理)

for(int i =0; i< MaxSteps; i++)
{

    // 当前点的深度如果超过了场景深度(在View空间比较)，则返回0
    if(CurrentDepth > SceneDepthLinear){
       return float4(0,0,0,0);
    }
    
    // 当前世界位置转换为局部位置
    float3 LocalPos = mul(float4(CurrentWorldPos,1.0), (WSDemote(GetWorldToLocal(Parameters)))).xyz;    
    float3 UVW = (LocalPos - LocalBoundsMin)/LocalBoundsSize;

   float SampledValue = 1.0; 

    // 2. 只有在 UVW 范围内才真正采样纹理
    // 这样既避免了四方连续(Tiling)，又避免了边界闪烁
    bool bIsInside = all(UVW >= 0.0) && all(UVW <= 1.0);
    
    if(bIsInside)
    {
        SampledValue = Texture3DSample(VolumeTex, VolumeTexSampler, UVW).x;
    }

    if(SampledValue <=0)
    {
        HitSurface = true;
      
        // 计算表面法线（通过梯度近似）
        float3 offset = float3(0.05, 0, 0); // 一个小的偏移量
        float densityX = Texture3DSample(VolumeTex, VolumeTexSampler, UVW + offset.xyy).x;
        float densityY = Texture3DSample(VolumeTex, VolumeTexSampler, UVW + offset.yxy).x;
        float densityZ = Texture3DSample(VolumeTex, VolumeTexSampler, UVW + offset.yyx).x;
        SurfaceNormal = normalize(float3(densityX - SampledValue, densityY - SampledValue, densityZ - SampledValue));

        // 简单的漫反射光照计算
        float3 LightDir = normalize(float3(1, 1, 1)); // 示例光源方向
        float Diffuse = max(0.2, dot(SurfaceNormal, LightDir)); // 包含环境光
        SurfaceColor = float3(1, 0, 0) * Diffuse; // 材质基础颜色 * 光照
        break;
    }


     // --- B. 步进 ---
    // 1. 线性累加深度 
    CurrentDepth += DepthStep;
    
    // 2. 累加世界坐标 
    CurrentWorldPos += WorldSpaceRayDir * StepSize;
    
}

if(HitSurface){
   return float4(SurfaceColor, 1.0);
}
else{
   return 0;
}
