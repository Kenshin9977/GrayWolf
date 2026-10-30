// One frame of the still added to the accumulator (additive blend): its linear colour weighted by
// its alpha (a straight alpha frame over a transparent background), and the alpha.
#include "PreviewPost.hlsli"

Texture2D<float4> Frame : register(t0);

float4 main(FullscreenOut i) : SV_Target0
{
    const float4 c = Frame.Load(int3(i.Pos.xy, 0));
    return float4(SrgbToLinear(saturate(c.rgb)) * c.a, c.a);
}
