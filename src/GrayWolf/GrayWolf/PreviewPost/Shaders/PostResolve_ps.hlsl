// The still so far, back to Output: the alpha-weighted mean colour, encoded, and the mean alpha.
#include "PreviewPost.hlsli"

Texture2D<float4> Accum : register(t0);

float4 main(FullscreenOut i) : SV_Target0
{
    const float4 s = Accum.Load(int3(i.Pos.xy, 0));
    const float3 c = s.a > 0 ? s.rgb / s.a : 0;
    return float4(LinearToSrgb(saturate(c)), saturate(s.a * PostParams.x));
}
