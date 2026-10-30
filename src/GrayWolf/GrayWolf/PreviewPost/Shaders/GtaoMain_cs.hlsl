// XeGTAO's main pass at its "high" quality (3 slices, 3 steps a side, as vaGTAO.hlsl's CSGTAOHigh).
#include "PreviewGtao.hlsli"

Texture2D<lpfloat>       g_srcWorkingDepth  : register(t0);
Texture2D<float4>        g_srcNormalmap     : register(t1);
RWTexture2D<uint>        g_outWorkingAOTerm : register(u0);
RWTexture2D<unorm float> g_outWorkingEdges  : register(u1);

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void main(const uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, 3, 3, PreviewSpatioTemporalNoise(pixCoord, g_GTAOConsts.NoiseIndex), PreviewLoadNormal(g_srcNormalmap, pixCoord),
                    g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges);
}
