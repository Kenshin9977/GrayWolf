#include "PreviewSmaa.hlsli"

float4 main(float4 Pos : SV_Position, float2 Uv : TEXCOORD0, float2 Pix : TEXCOORD1, float4 Offset[3] : TEXCOORD2) : SV_Target0
{
    return SMAABlendingWeightCalculationPS(Uv, Pix, Offset, EdgesTex, AreaTex, SearchTex, 0);   // SMAA 1x: no subsample
}
