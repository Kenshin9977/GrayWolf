// XeGTAO's edge-aware denoise, a pass before the last one (vaGTAO.hlsl's CSDenoisePass).
#include "PreviewGtao.hlsli"

Texture2D<uint>    g_srcWorkingAOTerm : register(t0);
Texture2D<lpfloat> g_srcWorkingEdges  : register(t1);
RWTexture2D<uint>  g_outFinalAOTerm   : register(u0);

// two pixels a thread: dispatched ((width + 1) / 2 + 7) / 8, (height + 7) / 8
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void main(const uint2 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);
    XeGTAO_Denoise(pixCoordBase, g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges, g_samplerPointClamp, g_outFinalAOTerm, false);
}
