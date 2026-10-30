#include "PreviewSmaa.hlsli"

// the blended premultiplied colour, straight again (Output's alpha is straight)
float4 main(float4 Pos : SV_Position, float2 Uv : TEXCOORD0, float4 Offset : TEXCOORD1) : SV_Target0
{
    const float4 c = SMAANeighborhoodBlendingPS(Uv, Offset, ColorTex, BlendTex);
    return c.a > 0 ? float4(c.rgb / c.a, c.a) : 0;
}
