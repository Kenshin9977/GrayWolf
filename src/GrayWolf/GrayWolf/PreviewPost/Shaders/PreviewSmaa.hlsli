// GrayWolf's bindings for SMAA (../ThirdParty/SMAA, MIT): HLSL 4.1 as SMAA.hlsl's own porting macros,
// with the samplers on fixed registers (its HLSL_4 path declares them as effect state blocks).
#include "PreviewPost.hlsli"

#define SMAA_RT_METRICS PostMetrics
#define SMAA_PRESET_HIGH
#define SMAA_CUSTOM_SL 1

SamplerState LinearSampler : register(s0);   // min/mag linear, mip point, clamp
SamplerState PointSampler : register(s1);    // point, clamp

#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)

#include "../ThirdParty/SMAA/SMAA.hlsl"

Texture2D ColorTex  : register(t0);   // the premultiplied frame (PostPremultiply_ps)
Texture2D EdgesTex  : register(t1);
Texture2D AreaTex   : register(t2);   // AreaTex.h, R8G8
Texture2D SearchTex : register(t3);   // SearchTex.h, R8
Texture2D BlendTex  : register(t4);
