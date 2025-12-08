
#include "/Engine/Public/Platform.ush"
#include "/Engine/Public/ShaderCommon.ush"

// 引入参数结构体
#include "ShaderParameters.hlsl"

// 采样SDF纹理（将世界坐标转换为纹理UV）
float SampleSDF(Texture3D<float> SDFTex, SamplerState Sampler, float3 WorldPos, FAxisAlignedBox SDFBounds)
{
    float3 UV = saturate((WorldPos - SDFBounds.Min) / (SDFBounds.Max - SDFBounds.Min));
    return SDFTex.SampleLevel(Sampler, UV, 0).r;
}

// 计算体素在世界空间的位置
float3 GetVoxelWorldPos(FIntVector VoxelIndex, FAxisAlignedBox Bounds, FIntVector Dimensions)
{
    float3 Step = (Bounds.Max - Bounds.Min) / Dimensions;
    return Bounds.Min + (VoxelIndex + 0.5f) * Step; // 体素中心点
}

[numthreads(8, 8, 8)] // 3D线程组，适配体素网格
void DynamicSDFUpdateKernel(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    FIntVector VoxelIndex = FIntVector(DispatchThreadId.x, DispatchThreadId.y, DispatchThreadId.z);
    
    // 超出动态SDF范围，直接返回
    if (VoxelIndex.X >= DynamicSDFDimensions.X || 
        VoxelIndex.Y >= DynamicSDFDimensions.Y || 
        VoxelIndex.Z >= DynamicSDFDimensions.Z)
    {
        return;
    }

    // 1. 计算当前体素在原始物体空间的位置
    float3 VoxelWorldPos = GetVoxelWorldPos(VoxelIndex, DynamicSDFBounds, DynamicSDFDimensions);

    // 2. 采样原始物体SDF
    float OriginalSDFValue = SampleSDF(
        SDFParams.OriginalSDF, 
        SDFParams.OriginalSDFSampler, 
        VoxelWorldPos, 
        DynamicSDFBounds
    );

    // 3. 将体素位置转换到切削工具SDF空间（应用工具的逆Transform）
    float3 ToolSpacePos = mul(float4(VoxelWorldPos, 1.0f), GlobalParams.ToolInverseTransform).xyz;

    // 4. 采样切削工具SDF（工具SDF的范围默认是[-ToolSize, ToolSize]，需与预生成时一致）
    float ToolSDFValue = SampleSDF(
        SDFParams.ToolSDF, 
        SDFParams.ToolSDFSampler, 
        ToolSpacePos, 
        FAxisAlignedBox(-50.0f, 50.0f) // 工具SDF预生成范围（需根据实际工具尺寸调整）
    );

    // 5. 布尔运算：保留原始物体（OriginalSDF < 0）且不在工具内部（ToolSDF > 0）的部分
    // 动态SDF = max(原始SDF, -工具SDF) → 工具内部（ToolSDF < 0）时，动态SDF = -ToolSDF > 0，被Marching Cubes丢弃
    float DynamicSDFValue = max(OriginalSDFValue, -ToolSDFValue);

    // 6. 只更新工具AABB范围内的体素（优化性能）
    if (GlobalParams.ToolAABB.Contains(VoxelWorldPos))
    {
        SDFParams.DynamicSDF[VoxelIndex] = DynamicSDFValue;
    }
    else
    {
        // 工具范围外，保持原始SDF值（避免重复计算）
        SDFParams.DynamicSDF[VoxelIndex] = OriginalSDFValue;
    }
}