// GRAYWOLF_PREVIEW_AO_VIEW=1 (a check of the occlusion): XeGTAO's visibility as grey, where the prepass drew.
#include "PreviewPost.hlsli"

Texture2D<uint> Visibility : register(t0);
Texture2D<float> Depth : register(t1);

float4 main(FullscreenOut i) : SV_Target0
{
    const int3 P = int3(i.Pos.xy, 0);
    if (Depth.Load(P) >= 1.0) return 0;
    const float v = saturate(Visibility.Load(P) / 255.0);   // as the light is multiplied (not display encoded)
    return float4(v, v, v, 1);
}
