float3 localcamvec = normalize(mul(Parameters.CameraVector, (float3x3)LWCToFloat(GetPrimitiveData(Parameters).WorldToLocal)));


float curdensity = 0;
float totaldist=  0;

float3 VolumeRes = NumFrames;
float3 RayPos = CurrentPos;

RayPos -= 0.5;
RayPos *= ( VolumeRes - 1) / (VolumeRes);
RayPos += 0.5;


for (int i = 0; i < MaxSteps; i++)
{	

// 采样体积纹理
    float4 cursample = Texture3DSample(Tex, TexSampler, RayPos);
    curdensity = cursample.x;
    
    // === 新增：深度检测逻辑 ===
    // 将当前射线位置转换到世界空间
    //float3 WorldPos = mul(float4(RayPos, 1.0), GetPrimitiveData(Parameters).LocalToWorld).xyz;
    float3 WorldPos =TransformLocalVectorToWorld(Parameters,RayPos).xyz;
    
    // 转换到裁剪空间
float4 ClipPos = mul(float4(WorldPos, 1.0), DFHackToFloat(ResolvedView.WorldToClip));
    
    // 透视除法，得到标准化设备坐标
    float3 NDCPos = ClipPos.xyz / ClipPos.w;
    
    // 转换到屏幕UV坐标（从[-1,1]到[0,1]）
    float2 CurrentScreenUV = NDCPos.xy * 0.5 + 0.5;
    
    // 采样当前屏幕位置对应的场景深度
    float CurrentSceneDepth = CalcSceneDepth(CurrentScreenUV);
    
    // 比较当前射线深度与场景深度[1,2](@ref)
    if (NDCPos.z > CurrentSceneDepth)
    {
        // 当前射线位置在场景物体后面，被遮挡，返回0
        return 0;
    }
    // === 深度检测结束 ===
	
	totaldist += curdensity;
	
	float e = totaldist * 0.001;

	if(curdensity < e )
	{
		return float2(1.0 / ((float) i + 1.0), 1);

	}
	
	RayPos -= localcamvec * curdensity*0.1;
	
	if (RayPos.x < 0 || RayPos.x > 1 || RayPos.y < 0 || RayPos.y > 1 || RayPos.z < 0 || RayPos.z > 1)
	{
		return 0;
	}
}



return 0;
