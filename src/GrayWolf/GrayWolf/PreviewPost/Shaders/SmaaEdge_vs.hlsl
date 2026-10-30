#include "PreviewSmaa.hlsli"

void main(uint Id : SV_VertexID, out float4 Pos : SV_Position, out float2 Uv : TEXCOORD0, out float4 Offset[3] : TEXCOORD1)
{
    const FullscreenOut f = Fullscreen(Id);
    Pos = f.Pos;
    Uv = f.Uv;
    SMAAEdgeDetectionVS(Uv, Offset);
}
