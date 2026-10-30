#include "PreviewSmaa.hlsli"

// luma edges (discards where there is none: the target is cleared first)
float4 main(float4 Pos : SV_Position, float2 Uv : TEXCOORD0, float4 Offset[3] : TEXCOORD1) : SV_Target0
{
    return float4(SMAALumaEdgeDetectionPS(Uv, Offset, ColorTex), 0, 0);
}
