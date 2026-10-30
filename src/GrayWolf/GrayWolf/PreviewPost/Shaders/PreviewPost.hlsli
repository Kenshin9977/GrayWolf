// What the still accumulation's and SMAA's full screen passes share.

cbuffer PostConstants : register(b0)
{
    float4 PostMetrics;   // 1 / width, 1 / height, width, height (SMAA_RT_METRICS)
    float4 PostParams;    // x: 1 / the frames in the accumulator
};

struct FullscreenOut
{
    float4 Pos : SV_Position;
    float2 Uv : TEXCOORD0;
};

// One triangle over the target, uv (0, 0) at the top left.
FullscreenOut Fullscreen(uint Id)
{
    FullscreenOut o;
    o.Uv = float2((Id << 1) & 2, Id & 2);
    o.Pos = float4(o.Uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// The sRGB curve both ways: the tone map writes sRGB-encoded values to Output, the still averages
// them as linear light (a box filter of what the screen shows), then encodes the mean back.
float3 SrgbToLinear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float3 LinearToSrgb(float3 c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
