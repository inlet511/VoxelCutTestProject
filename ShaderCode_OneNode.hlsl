// 输入参数
float3 VoluemWorldMin;
float3 VolumeWorldMax;
float3 VolumeRes;
float SurfaceThreshold;

bool HitSurface = false;
float3 SurfaceNormal = float3(0, 0, 1);
float3 SurfaceColor = float3(0, 0, 0);
float TotalDistance = 0;

float3 CurrentWorldPos = WorldPosition;

float3 LocalBoundsMin = GetPrimitiveData(Parameters).LocalObjectBoundsMin;
float3 LocalBoundsMax = GetPrimitiveData(Parameters).LocalObjectBoundsMax;
float3 LocalBoundsSize = LocalBoundsMax - LocalBoundsMin;


for(int i =0; i< MaxSteps; i++)
{    
    // 将当前世界位置转换到View空间
    float4 CurrentViewPos = mul(ResolvedView.TranslatedWorldToView, float4(CurrentWorldPos,1.0));
    float LinearDepthCurrent = -CurrentViewPos.z;

    // float3 ViewVector = normalize(-Parameters.CameraVector);
    // float3 ViewForward = mul(float3(0,0,1),(float3x3)ResolvedView.ViewToTranslatedWorld);
    // float CosTheta = dot(ViewVector, normalize(ViewForward));

    // float AxisalSceneDepth = SceneDepth * CosTheta;

    float3 ViewVector = CurrentWorldPos - CameraPos;
    float ActuralDistance = length(ViewVector);

    // 当前点的深度如果超过了场景深度(在View空间比较)，则返回0
    if(ActuralDistance > SceneDepth){
       return 0;
    }
    
    // 当前世界位置转换为局部位置
    float3 LocalPos = mul(float4(CurrentWorldPos,1.0), (WSDemote(GetWorldToLocal(Parameters)))).xyz;    
    float3 UVW = (LocalPos - LocalBoundsMin)/LocalBoundsSize;
    float SampledValue = Texture3DSample(VolumeTex, VolumeTexSampler, UVW).x;
    if(SampledValue <=0)
    {
        HitSurface = true;
      
        // 计算表面法线（通过梯度近似）
        float3 offset = float3(0.005, 0, 0); // 一个小的偏移量
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
    CurrentWorldPos += -CameraVector * StepSize;  
    
}

if(HitSurface){
   return float4(SurfaceColor, 1.0);
}
else{
   return 0;
}
