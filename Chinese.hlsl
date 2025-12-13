
材质设置：勾选"禁用深度测试"，并设置"混合模式"为AlphaComposite
Custom Node 输入参数: 一个参数(WorldPosition), Absolute World Position 输入给它.
Custom Node 输出 : float4, 连接到材质的Emissive.
Custom Node 的 hlsl 代码:
------------------------------------------
// Absolute World Position 连接到 WorldPosition参数
float3 CurrentWorldPos = WorldPosition;
// 减去"预偏移"
float3 TranslatedWorldPos = CurrentWorldPos - LWCHackToFloat(PrimaryView.PreViewTranslation);
 
// 当前像素表面的世界坐标系位置 -> ViewSpace坐标系位置
float4 CurrentViewPos = mul(float4(TranslatedWorldPos,1.0), PrimaryView.TranslatedWorldToView);

// -CurrentViewPos.z 应该就是ViewSpace的深度
// ? 不确定是否需要加上NearPlane? 但是似乎有没有对结果影响不大
float CurrentPosDepth = -CurrentViewPos.z + PrimaryView.NearPlane;

float2 ScreenUV = MaterialFloat2(ScreenAlignedPosition(Parameters.ScreenPosition).xy);
// 其他对象的线性场景深度
float SceneDepthLinear = CalcSceneDepth(ScreenUV);


if(CurrentPosDepth > SceneDepthLinear){
     return float4(1,0,0,0);
 }
 else
 {
    return float4(0,1,0,1);
 }
-------------------------------------------

对比两个深度，该物体未被遮挡的部分应该显示绿色，被遮挡的部分应该显示红色。但是实际结果很奇怪，红色绿色分布不符合实际深度情况