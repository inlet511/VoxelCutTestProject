// 参数设置
#define JITTER_SAMPLES 4 // 抖动采样次数
#define DENSITY_THRESHOLD 0.01

float3 localcamvec = normalize(mul(Parameters.CameraVector, (float3x3)LWCToFloat(GetPrimitiveData(Parameters).WorldToLocal)));

float totaldensity = 0;
float3 accumulatedColor = 0;
float3 VolumeRes = NumFrames;
float3 RayStart = CurrentPos;

// 坐标规范化（你的原有代码）
RayStart -= 0.5;
RayStart *= (VolumeRes - 1) / VolumeRes;
RayStart += 0.5;

// 抖动采样循环
for (int j = 0; j < JITTER_SAMPLES; j++) {
    float3 RayPos = RayStart;
    
    // 添加微小抖动（减少规则图案）
    if (j > 0) {
        RayPos += (0.5 / VolumeRes) * (hash(j) - 0.5);
    }
    
    // Raymarching循环
    for (int i = 0; i < MaxSteps; i++) {    
        // 使用三线性插值而非最近邻采样
        float4 cursample = Tex.Sample(TexSampler, RayPos);
        float curdensity = cursample.x;
        
        // 提前退出条件
        if (curdensity > DENSITY_THRESHOLD) {
            // 你的着色计算代码
            accumulatedColor += shadingResult;
            totaldensity += curdensity;
        }
        
        // 动态步长调整（基于当前密度）
        float stepSize = max(0.01, 0.1 * (1.0 - curdensity));
        RayPos -= localcamvec * stepSize;
        
        // 边界检查
        if (any(RayPos < 0) || any(RayPos > 1)) {
            break;
        }
    }
}

// 平均化采样结果
if (JITTER_SAMPLES > 1) {
    accumulatedColor /= JITTER_SAMPLES;
    totaldensity /= JITTER_SAMPLES;
}

return float4(accumulatedColor, totaldensity);