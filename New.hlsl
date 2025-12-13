
float CurrentPosDepth = Parameters.ScreenPosition.w;


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
    