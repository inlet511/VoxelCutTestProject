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

	//float4 cursample = PseudoVolumeTexture(Tex, TexSampler, RayPos, XYFrames, NumFrames);
    float4 cursample = Texture3DSample(Tex,TexSampler, RayPos);
	curdensity = cursample.x;

	
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
