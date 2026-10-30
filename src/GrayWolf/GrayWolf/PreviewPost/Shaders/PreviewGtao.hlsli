// GrayWolf's bindings for Intel's XeGTAO (../ThirdParty/XeGTAO, MIT): the entry points of its
// sample's vaGTAO.hlsl, cut to what the model viewport runs on D3D11 (cs_5_0, 32-bit depths, the
// normals from the depth prepass instead of R11G11B10 ones).

#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
#define XE_GTAO_FP32_DEPTHS
#define VA_SATURATE saturate

#include "../ThirdParty/XeGTAO/XeGTAO.h"

cbuffer GTAOConstantBuffer : register(b0)
{
    GTAOConstants g_GTAOConsts;
}

// The denoiser's GatherRed of its R8_UINT AO term, as four loads: a D3D11 device need not gather an
// integer format (NVIDIA's does not). The 2x2 texels around Uv, clamped, in Gather's order
// (lower left, lower right, upper right, upper left).
uint4 PreviewGatherUint(Texture2D<uint> Tex, float2 Uv, int2 Ofs)
{
    const int2 Hi = g_GTAOConsts.ViewportSize - 1;
    const int2 C = int2(floor(Uv * g_GTAOConsts.ViewportSize - 0.5)) + Ofs;   // the upper left texel
    const uint TL = Tex.Load(int3(clamp(C, 0, Hi), 0));
    const uint TR = Tex.Load(int3(clamp(C + int2(1, 0), 0, Hi), 0));
    const uint BL = Tex.Load(int3(clamp(C + int2(0, 1), 0, Hi), 0));
    const uint BR = Tex.Load(int3(clamp(C + int2(1, 1), 0, Hi), 0));
    return uint4(BL, BR, TR, TL);
}
#define XE_GTAO_GATHER_AO_TERM(tex, smp, uv, ofs) PreviewGatherUint(tex, uv, ofs)

#include "../ThirdParty/XeGTAO/XeGTAO.hlsli"

// point filtering, clamped (XeGTAO's g_samplerPointClamp)
SamplerState g_samplerPointClamp : register(s0);

// The prepass's normals: view space as XeGTAO has it (x right, y up, z away from the eye), n * 0.5 + 0.5.
lpfloat3 PreviewLoadNormal(Texture2D<float4> Normals, int2 Pos)
{
    return (lpfloat3)normalize(Normals.Load(int3(Pos, 0)).xyz * 2.0 - 1.0);
}

// vaGTAO.hlsl's: a Hilbert curve driving R2 (https://www.shadertoy.com/view/3tB3z3)
lpfloat2 PreviewSpatioTemporalNoise(uint2 pixCoord, uint temporalIndex)
{
    uint index = HilbertIndex(pixCoord.x, pixCoord.y);
    index += 288 * (temporalIndex % 64);
    return lpfloat2(frac(0.5 + index * float2(0.75487766624669276005, 0.5698402909980532659114)));
}
