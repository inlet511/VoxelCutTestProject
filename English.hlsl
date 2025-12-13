Material Settings: Check "Disable Depth Test", BlendMode: "Alpha Composite"

Custom Node Input Params: Only One Param named WorldPosition, Connect Absolute World Position to it.
Custom Node Output : float4, connect to Material's emissive channel.
Custom Node's hlsl Code:
-----------------------------------
// Absolute World Position connects to : WorldPosition 
float3 CurrentWorldPos = WorldPosition;
// Minus the PreOffset
float3 TranslatedWorldPos = CurrentWorldPos - LWCHackToFloat(PrimaryView.PreViewTranslation);
 
// CurrentPixel WorldSpace Position -> ViewSpace
float4 CurrentViewPos = mul(float4(TranslatedWorldPos,1.0), PrimaryView.TranslatedWorldToView);

// -CurrentViewPos.z should be the Depth of the current Pixel in View Sapce.
// ? Not sure if the NearPlane part is neccesary , but that doesn't seem to make a big difference
float CurrentPosDepth = -CurrentViewPos.z + PrimaryView.NearPlane;

float2 ScreenUV = MaterialFloat2(ScreenAlignedPosition(Parameters.ScreenPosition).xy);
// Linear (Background)SceneDepth in ViewSpace
float SceneDepthLinear = CalcSceneDepth(ScreenUV);

// Compare the two depth, but the result seems to be odd, not as expected.
if(CurrentPosDepth > SceneDepthLinear){
     return float4(1,0,0,0);
 }
 else
 {
    return float4(0,1,0,1);
 }
-----------------------------------------

    