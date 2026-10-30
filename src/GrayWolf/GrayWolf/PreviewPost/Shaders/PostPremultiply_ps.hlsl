// SMAA's input: Output premultiplied, so a blend across a transparent background's edge keeps its colour
// (SmaaBlend_ps divides it back out).
#include "PreviewPost.hlsli"

Texture2D<float4> Frame : register(t0);

float4 main(FullscreenOut i) : SV_Target0
{
    const float4 c = Frame.Load(int3(i.Pos.xy, 0));
    return float4(c.rgb * c.a, c.a);
}
