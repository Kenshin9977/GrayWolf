#include "stdafx.h"

#include "ModelPreviewGPU.h"
#include "ModelPreview.h"     // LoadPreviewGeometry / PreviewGeometry
#include "PreviewTextures.h"
#include "DirectXTex.h"       // CaptureTexture, SaveToWICFile

#include <directxtk/CommonStates.h>
#include <directxtk/DirectXHelpers.h>
#include <directxtk/Effects.h>
#include <directxtk/PostProcess.h>
#include <directxtk/VertexTypes.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    // Runs behind PBREffect's vertex shader and constants (b0 as in PBREffect.fx). PBREffect's own
    // pixel shaders cannot flip a back face's normal (two-sided cards), sharpen alpha for alpha to
    // coverage, or read per-game channels without repacking textures (MaterialDescription.h).
    const char* kPixelShader = R"(
cbuffer Constants : register(b0)
{
    float3   EyePosition            : packoffset(c0);
    float4x4 World                  : packoffset(c1);
    float3x3 WorldInverseTranspose  : packoffset(c5);
    float4x4 WorldViewProj          : packoffset(c8);
    float4x4 PrevWorldViewProj      : packoffset(c12);
    float3   LightDirection[3]      : packoffset(c16);
    float3   LightColor[3]          : packoffset(c19);
    float3   ConstantAlbedo         : packoffset(c22);
    float    Alpha                  : packoffset(c22.w);
    float    ConstantMetallic       : packoffset(c23.x);
    float    ConstantRoughness      : packoffset(c23.y);
    int      NumRadianceMipLevels   : packoffset(c23.z);
};
cbuffer Material : register(b1)
{
    float4 RoughMask, MetalMask, OcclusionMask;
    float  RoughInvert, RoughValue, MetalValue, Cutoff;
    uint   Flags, NormalEncoding; float2 MaterialPad;
    float4 TintColor, TintParams, TintChannel, OffsetChannel, MaskChannel;
    float4 Layer[2], LayerTint[2], MaskScale, ColorTint, ColorMaskChannel;
    float4 SpecularParams;   // x: the specular map's scale, y: 1 = pre-PBR (colour kept)
    float4 TransmissionColor, TransmissionMask;   // rgb linear, w: 1 = the map is a thickness
    float4 AlphaParams, AlphaMask;                // the alpha map's low, high, uv scale; its channel
    float4 EmissiveColor;                         // rgb: the emission's colour (linear)
};
cbuffer Scene : register(b2)
{
    float IblScale, Samples, Ambient, NoSpecular;   // Ambient: flat light added to the image based one
};

Texture2D<float4> AlbedoMap    : register(t0);
Texture2D<float4> NormalMap    : register(t1);
Texture2D<float4> RoughnessMap : register(t2);
Texture2D<float4> EmissiveMap  : register(t3);
TextureCube<float3> Radiance   : register(t4);
TextureCube<float3> Irradiance : register(t5);
Texture2D<float4> OcclusionMap : register(t6);
Texture2D<float4> SpecularMap  : register(t7);
Texture2D<float4> MetalnessMap : register(t8);
Texture2D<float4> TintMap      : register(t9);
Texture2D<float4> TintOffsetMap: register(t10);
Texture2D<float4> LayerAlbedo0 : register(t11);
Texture2D<float4> LayerNormal0 : register(t12);
Texture2D<float4> LayerRough0  : register(t13);
Texture2D<float4> LayerAlbedo1 : register(t14);
Texture2D<float4> LayerNormal1 : register(t15);
Texture2D<float4> LayerRough1  : register(t16);
Texture2D<float4> LayerMask    : register(t17);
Texture2D<float4> ColorMask    : register(t18);
Texture2D<float4> TransmissionMap : register(t19);
Texture2D<float4> AlphaMap     : register(t20);
SamplerState Surface : register(s0);
SamplerState IblSampler : register(s1);

struct PSIn
{
    float2 TexCoord   : TEXCOORD0;
    float4 PositionWS : TEXCOORD1;
    float3 NormalWS   : TEXCOORD2;
    float4 Diffuse    : COLOR0;
    float4 PositionPS : SV_Position;
};

static const float PI = 3.14159265f;

// Schueler, "Normal Mapping without Precomputed Tangents" (ShaderX5), as DirectXTK's CalculateTBN.
float3 PerturbNormal(float3 local, float3 p, float3 n, float2 uv)
{
    float3 dp1 = ddx(p), dp2 = ddy(p);
    float2 duv1 = ddx(uv), duv2 = ddy(uv);
    float3x3 M = float3x3(dp1, dp2, cross(dp1, dp2));
    float2x3 inv = float2x3(cross(M[1], M[2]), cross(M[2], M[0]));
    float3 t = mul(float2(duv1.x, duv2.x), inv);
    float3 b = mul(float2(duv1.y, duv2.y), inv);
    float s = rsqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
    return normalize(mul(local, float3x3(t * s, b * s, n)));
}

float3 DecodeNormal(float4 t)
{
    float2 xy = NormalEncoding == 1 ? t.ag : t.rg;
    xy = xy * 2 - 1;
    if (Flags & 256) xy.y = -xy.y;
    if (NormalEncoding == 2) return normalize(float3(xy, t.b * 2 - 1));
    return float3(xy, sqrt(saturate(1 - dot(xy, xy))));
}

// A layer's uv: rotated about the centre, then scaled (MaterialDescription.h, layers).
float2 LayerUV(float2 uv, float4 s)
{
    float c = cos(s.z), n = sin(s.z);
    float2 p = uv - 0.5;
    return (float2(c * p.x + n * p.y, c * p.y - n * p.x) + 0.5) * s.xy;
}

// A layer's colour, tinted by its own alpha.
float3 LayerColor(float4 c, float4 tint) { return c.rgb * lerp(1, tint.rgb, c.a); }

float LayerRoughness(Texture2D<float4> map, float2 uv, float4 tint, float fallback, bool hasMap)
{
    return hasMap ? 1 - map.Sample(Surface, uv).r : (tint.w >= 0 ? tint.w : fallback);
}

float3 FresnelSchlick(float3 f0, float x) { return f0 + (1 - f0) * pow(1 - x, 5); }

// DirectXTK's PBRCommon.fxh (MIT) lighting, fed a diffuse and a specular colour so both the metal
// and the specular workflows go through it.
float3 LightSurface(float3 V, float3 N, float3 cDiff, float3 cSpec, float roughness, float ao)
{
    float NdotV = saturate(dot(N, V));
    float alpha = max(roughness * roughness, 0.002);
    float3 color = 0;
    [unroll] for (int i = 0; i < 3; i++)
    {
        float3 L = normalize(-LightDirection[i]);
        float3 H = normalize(L + V);
        float NdotL = saturate(dot(N, L)), LdotH = saturate(dot(L, H)), NdotH = saturate(dot(N, H));
        float fd90 = 0.5 + 2 * roughness * LdotH * LdotH;
        float diffuse = (1 + (fd90 - 1) * pow(1 - NdotL, 5)) * (1 + (fd90 - 1) * pow(1 - NdotV, 5));
        float a2 = alpha * alpha, d = NdotH * NdotH * (a2 - 1) + 1;
        float D = a2 / max(1e-6, PI * d * d);
        float G = rcp(lerp(LdotH * LdotH, 1, a2 * 0.25));
        float3 spec = D * FresnelSchlick(cSpec, LdotH) * G;
        color += NdotL * LightColor[i] * (cDiff * diffuse + spec);
    }
    color += cDiff * (Irradiance.Sample(IblSampler, N) * IblScale + Ambient) * ao;
    float3 R = reflect(-V, N);
    float3 env = Radiance.SampleLevel(IblSampler, R, roughness * NumRadianceMipLevels);
    color += FresnelSchlick(cSpec, NdotV) * env * IblScale * ao;
    return color;
}

float4 PSMain(PSIn i, out uint Coverage : SV_Coverage) : SV_Target0
{
    float2 uv = i.TexCoord;
    float4 albedo = (Flags & 1) ? AlbedoMap.Sample(Surface, uv) : float4(0.62, 0.62, 0.66, 1);
    // an alpha map in place of the colour map's alpha: a soft edge between low and high
    if (Flags & 65536)
        albedo.a = saturate((dot(AlphaMap.Sample(Surface, uv * AlphaParams.zw), AlphaMask) - AlphaParams.x) / max(AlphaParams.y - AlphaParams.x, 1e-4));

    Coverage = 0xFFFFFFFF;
    if (Flags & 128)
    {
        // Alpha to coverage: Castano's sharpening gives a crisp edge at Cutoff, and the samples it
        // covers are written whole (alpha 1), so a transparent background never shows through.
        float a = saturate((albedo.a - Cutoff) / max(fwidth(albedo.a), 1e-4) + 0.5);
        if (Samples <= 1) clip(a - 0.5);
        else Coverage = (1u << (uint)round(a * Samples)) - 1;
    }

    // Layers: each at its own uv, the upper one over the lower by its mask.
    float mask = 0;
    float2 uv0 = uv, uv1 = uv;
    if (Flags & 2048)
    {
        uv0 = LayerUV(uv, Layer[0]);
        uv1 = LayerUV(uv, Layer[1]);
        if (Flags & 4096) mask = dot(LayerMask.Sample(Surface, uv * MaskScale.xy), MaskChannel);
        albedo.rgb = lerp(LayerColor(LayerAlbedo0.Sample(Surface, uv0), LayerTint[0]),
                          LayerColor(LayerAlbedo1.Sample(Surface, uv1), LayerTint[1]), mask);
    }
    // A colour mixed in by a mask.
    if (Flags & 8192) albedo.rgb *= lerp(1, ColorTint.rgb, dot(ColorMask.Sample(Surface, uv), ColorMaskChannel));
    // Tint gradient: a column of the gradient, row by the mesh's v (plus an offset map).
    if (Flags & 512)
    {
        float v = uv.y;
        if (Flags & 1024) v += TintParams.x * (2 * dot(TintOffsetMap.Sample(Surface, uv), OffsetChannel) - 1);
        float4 g = TintMap.SampleLevel(IblSampler, float2(TintColor.w, saturate(v)), 0);
        albedo.rgb *= TintParams.y > 0.5 ? g.rgb : dot(g, TintChannel) * TintColor.rgb;
    }

    // Two sided: a normal facing away is the back of a card or an open mesh, seen from behind.
    // (The exports' winding varies, so SV_IsFrontFace cannot tell.)
    float3 V = normalize(EyePosition - i.PositionWS.xyz);
    float3 Ng = normalize(i.NormalWS);
    if (dot(Ng, V) < 0) Ng = -Ng;
    float3 N = Ng;
    if (Flags & 2048)
    {
        // the layers' detail normals add their xy to the base normal's
        float3 base = (Flags & 2) ? DecodeNormal(NormalMap.Sample(Surface, uv)) : float3(0, 0, 1);
        float2 d0 = DecodeNormal(LayerNormal0.Sample(Surface, uv0)).xy * Layer[0].w;
        float2 d1 = DecodeNormal(LayerNormal1.Sample(Surface, uv1)).xy * Layer[1].w;
        N = PerturbNormal(normalize(float3(base.xy + lerp(d0, d1, mask), base.z)), i.PositionWS.xyz, Ng, uv);
    }
    else if (Flags & 2) N = PerturbNormal(DecodeNormal(NormalMap.Sample(Surface, uv)), i.PositionWS.xyz, Ng, uv);

    float3 color;
    {
        float rough = (Flags & 4) ? dot(RoughnessMap.Sample(Surface, uv), RoughMask) : RoughValue;
        if (RoughInvert > 0.5) rough = 1 - rough;
        if (Flags & 2048)
            rough = lerp(LayerRoughness(LayerRough0, uv0, LayerTint[0], rough, MaterialPad.x > 0.5),
                         LayerRoughness(LayerRough1, uv1, LayerTint[1], rough, MaterialPad.y > 0.5), mask);
        rough = clamp(rough, 0.03, 1);
        float ao = (Flags & 32) ? dot(OcclusionMap.Sample(Surface, uv), OcclusionMask) : 1;
        float3 cDiff, cSpec;
        if (Flags & 8)
        {
            float metal = dot(MetalnessMap.Sample(Surface, uv), MetalMask);
            cDiff = albedo.rgb * (1 - metal);
            cSpec = lerp(0.04, albedo.rgb, metal);
        }
        else if (Flags & 16)
        {
            cSpec = SpecularMap.Sample(Surface, uv).rgb * SpecularParams.x;
            cDiff = SpecularParams.y > 0.5 ? albedo.rgb : albedo.rgb * (1 - max(cSpec.r, max(cSpec.g, cSpec.b)));
        }
        else
        {
            cDiff = albedo.rgb * (1 - MetalValue);
            cSpec = lerp(0.04, albedo.rgb, MetalValue);
        }
        if (NoSpecular > 0.5) { cDiff = albedo.rgb; cSpec = 0.04; }
        color = LightSurface(V, N, cDiff, cSpec, rough, ao);
    }
    if (Flags & 64) color += EmissiveMap.Sample(Surface, uv).rgb * EmissiveColor.rgb;
    // Transmission: the light reaching the far side (the lights behind the surface, the sky along
    // -N) comes through, times the colour and the transmission map.
    if (Flags & 16384)
    {
        float t = dot(TransmissionMap.Sample(Surface, uv), TransmissionMask);
        if (TransmissionColor.w > 0.5) t = 1 - t;
        float3 back = Irradiance.Sample(IblSampler, -N) * IblScale + Ambient;
        [unroll] for (int k = 0; k < 3; k++) back += saturate(dot(N, normalize(LightDirection[k]))) * LightColor[k];
        color += albedo.rgb * TransmissionColor.rgb * t * back;
    }
    return float4(color, (Flags & 32768) ? albedo.a : 1);
}
)";

    // Over a transparent background the resolve leaves edge pixels premultiplied (colour times
    // coverage); tone mapping that would darken them, so the colour is divided back out first.
    const char* kUnpremultiplyShader = R"(
Texture2D<float4> Resolved : register(t0);
float4 VSMain(uint Id : SV_VertexID) : SV_Position
{
    float2 uv = float2((Id << 1) & 2, Id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PSMain(float4 Pos : SV_Position) : SV_Target0
{
    float4 c = Resolved.Load(int3(Pos.xy, 0));
    return c.a > 0 ? float4(c.rgb / c.a, c.a) : 0;
}
)";

    struct SceneConstants { float IblScale, Samples, Ambient, NoSpecular; };

    // The image based light: a sky that only varies with height (orbiting never makes it jump).
    // Each radiance mip is that sky through a GGX-sized lobe, the irradiance cube the cosine-
    // weighted sky, both integrated on the CPU once.
    XMFLOAT3 StudioSky(float Z)
    {
        const XMFLOAT3 Zenith{ 0.85f, 0.90f, 1.00f }, Horizon{ 0.62f, 0.58f, 0.54f }, Floor{ 0.10f, 0.095f, 0.09f };
        auto Mix = [](XMFLOAT3 A, XMFLOAT3 B, float T) { return XMFLOAT3{ A.x + (B.x - A.x) * T, A.y + (B.y - A.y) * T, A.z + (B.z - A.z) * T }; };
        if (Z >= 0) return Mix(Horizon, Zenith, std::pow(Z, 0.6f));
        return Mix(Horizon, Floor, std::min(1.0f, -Z * 3.0f));
    }

    // Sky(Z) filtered by a lobe around a direction of height Z: Weight(cosine to the axis).
    template <class W> std::vector<XMFLOAT3> FilteredSky(int Steps, W Weight)
    {
        constexpr int N = 1024;   // Fibonacci sphere
        std::vector<XMFLOAT3> Dirs(N);
        for (int i = 0; i < N; i++)
        {
            const float Z = 1.0f - 2.0f * (i + 0.5f) / N, R = std::sqrt(std::max(0.0f, 1.0f - Z * Z)), A = 2.39996323f * i;
            Dirs[i] = { R * std::cos(A), R * std::sin(A), Z };
        }
        std::vector<XMFLOAT3> Out(Steps);
        for (int k = 0; k < Steps; k++)
        {
            const float Z = -1.0f + 2.0f * k / (Steps - 1), X = std::sqrt(std::max(0.0f, 1.0f - Z * Z));
            XMFLOAT3 Sum{}; float WSum = 0;
            for (const auto& D : Dirs)
            {
                const float W = Weight(X * D.x + Z * D.z);
                if (W <= 0) continue;
                const XMFLOAT3 C = StudioSky(D.z);
                Sum.x += C.x * W; Sum.y += C.y * W; Sum.z += C.z * W; WSum += W;
            }
            Out[k] = WSum > 0 ? XMFLOAT3{ Sum.x / WSum, Sum.y / WSum, Sum.z / WSum } : StudioSky(Z);
        }
        return Out;
    }

    // A cube (Z up, as the preview samples it) whose texel colour is Table(texel direction's Z).
    void FillCube(size_t Size, const std::vector<XMFLOAT3>& Table, const Image* Faces)
    {
        for (int F = 0; F < 6; F++)
            for (size_t y = 0; y < Size; y++)
                for (size_t x = 0; x < Size; x++)
                {
                    const float U = 2.0f * (x + 0.5f) / Size - 1.0f, V = 2.0f * (y + 0.5f) / Size - 1.0f;
                    const XMFLOAT3 D = F == 0 ? XMFLOAT3{ 1, -V, -U } : F == 1 ? XMFLOAT3{ -1, -V, U } : F == 2 ? XMFLOAT3{ U, 1, V }
                                     : F == 3 ? XMFLOAT3{ U, -1, -V } : F == 4 ? XMFLOAT3{ U, -V, 1 } : XMFLOAT3{ -U, -V, -1 };
                    const float Z = D.z / std::sqrt(D.x * D.x + D.y * D.y + D.z * D.z);
                    const float T = (Z + 1.0f) * 0.5f * (Table.size() - 1);
                    const size_t I = std::min(Table.size() - 2, (size_t)T);
                    const float K = T - I;
                    float* Px = reinterpret_cast<float*>(Faces[F].pixels + y * Faces[F].rowPitch) + x * 4;
                    Px[0] = Table[I].x + (Table[I + 1].x - Table[I].x) * K;
                    Px[1] = Table[I].y + (Table[I + 1].y - Table[I].y) * K;
                    Px[2] = Table[I].z + (Table[I + 1].z - Table[I].z) * K;
                    Px[3] = 1.0f;
                }
    }

    bool StudioEnvironment(ID3D11Device* Dev, ComPtr<ID3D11ShaderResourceView>& Radiance, int& RadianceMips,
                           ComPtr<ID3D11ShaderResourceView>& Irradiance)
    {
        constexpr size_t Size = 64;
        ScratchImage Rad;
        if (FAILED(Rad.InitializeCube(DXGI_FORMAT_R32G32B32A32_FLOAT, Size, Size, 1, 0))) return false;
        RadianceMips = (int)Rad.GetMetadata().mipLevels;
        for (int Mip = 0; Mip < RadianceMips; Mip++)
        {
            const float Rough = RadianceMips > 1 ? (float)Mip / (RadianceMips - 1) : 0.0f;
            const float Alpha = std::max(Rough * Rough, 0.01f), Lambda = 2.0f / (Alpha * Alpha);
            const auto Table = FilteredSky(65, [Lambda](float C) { return std::exp(Lambda * (C - 1.0f)); });
            Image Faces[6];
            for (int F = 0; F < 6; F++) Faces[F] = *Rad.GetImage(Mip, F, 0);
            FillCube(Faces[0].width, Table, Faces);
        }
        ScratchImage Irr;
        if (FAILED(Irr.InitializeCube(DXGI_FORMAT_R32G32B32A32_FLOAT, 16, 16, 1, 1))) return false;
        {
            const auto Table = FilteredSky(65, [](float C) { return std::max(0.0f, C); });
            Image Faces[6];
            for (int F = 0; F < 6; F++) Faces[F] = *Irr.GetImage(0, F, 0);
            FillCube(16, Table, Faces);
        }
        return SUCCEEDED(CreateShaderResourceView(Dev, Rad.GetImages(), Rad.GetImageCount(), Rad.GetMetadata(), Radiance.GetAddressOf())) &&
               SUCCEEDED(CreateShaderResourceView(Dev, Irr.GetImages(), Irr.GetImageCount(), Irr.GetMetadata(), Irradiance.GetAddressOf()));
    }

    // Inverse of the display transform (ACES filmic, then the ~2.2 sRGB curve) for one channel:
    // what the HDR target must hold for Value to come out on screen.
    float BeforeToneMap(float Value)
    {
        const float Y = std::pow(std::clamp(Value, 0.0f, 0.999f), 2.2f);
        const float A = 2.51f - 2.43f * Y, B = 0.03f - 0.59f * Y, C = -0.14f * Y;
        return A > 1e-6f ? (-B + std::sqrt(std::max(0.0f, B * B - 4 * A * C))) / (2 * A) : Y;
    }
}

std::vector<float> InterleavePreviewVertices(const std::vector<float>& Pos, const std::vector<float>& Nrm,
                                             const std::vector<float>& UV)
{
    const size_t VertexCount = Pos.size() / 3;
    std::vector<float> Vtx(VertexCount * 8);
    float* To = Vtx.data();
    const float *P = Pos.data(), *N = Nrm.data(), *T = UV.data();
    for (size_t i = 0; i < VertexCount; i++, To += 8, P += 3, N += 3, T += 2)
    {
        To[0] = P[0]; To[1] = P[1]; To[2] = P[2];
        To[3] = N[0]; To[4] = N[1]; To[5] = N[2];
        To[6] = T[0]; To[7] = T[1];
    }
    return Vtx;
}

namespace
{
    void Mask(float (&M)[4], int Channel) { for (int c = 0; c < 4; c++) M[c] = (c == (Channel & 3)) ? 1.0f : 0.0f; }

    PreviewGpuMaterial GpuMaterial(ID3D11Device* Dev, const PreviewMaterial& S, int MaxSize)
    {
        PreviewGpuMaterial M;
        const Materials::Description& D = S.Desc;
        auto Srv = [&](const PreviewImage& Img, bool Srgb, uint32_t Flag, ComPtr<ID3D11ShaderResourceView>& Out) {
            if (Img.Empty()) return;
            Out = CreatePreviewSRV(Dev, Img, Srgb, MaxSize);
            if (Out) M.C.Flags |= Flag;
        };
        Srv(S.Albedo, true, PreviewHasAlbedo, M.Albedo);
        Srv(S.Normal, false, PreviewHasNormal, M.Normal);
        Srv(S.Roughness, false, PreviewHasRoughness, M.Roughness);
        Srv(S.Metalness, false, PreviewHasMetalness, M.Metalness);
        Srv(S.Specular, true, PreviewHasSpecular, M.Specular);
        Srv(S.Occlusion, false, PreviewHasOcclusion, M.Occlusion);
        Srv(S.Emissive, true, PreviewHasEmissive, M.Emissive);
        for (int c = 0; c < 3; c++) M.C.EmissiveColor[c] = D.EmissiveColor[c];
        Mask(M.C.RoughMask, D.Roughness.Channel);
        Mask(M.C.MetalMask, D.Metalness.Channel);
        Mask(M.C.OcclusionMask, D.Occlusion.Channel);
        M.C.RoughInvert = D.Roughness.Invert ? 1.0f : 0.0f;
        M.C.RoughValue = D.RoughnessValue;
        M.C.MetalValue = D.MetalnessValue;
        M.C.Cutoff = D.AlphaCutoff;
        M.C.NormalEncoding = (uint32_t)D.NormalEnc;
        M.C.Specular[0] = D.SpecularScale;
        M.C.Specular[1] = D.LegacySpecular ? 1.0f : 0.0f;
        if (D.NormalFlipY) M.C.Flags |= PreviewNormalFlipY;
        // the tint gradient: sampled as colour (sRGB), its offset map as data
        if (!D.Tint.Empty() && (M.Tint = CreatePreviewSRV(Dev, S.Tint, true, MaxSize)))
        {
            M.C.Flags |= PreviewHasTint;
            for (int c = 0; c < 3; c++) M.C.TintColor[c] = D.Tint.Color[c];
            M.C.TintColor[3] = D.Tint.U;
            M.C.TintParams[1] = D.Tint.Rgb ? 1.0f : 0.0f;
            Mask(M.C.TintChannel, D.Tint.Image.Channel);
            if (!D.Tint.Offset.Empty() && (M.TintOffset = CreatePreviewSRV(Dev, S.TintOffset, false, MaxSize)))
            {
                M.C.Flags |= PreviewHasTintOffset;
                M.C.TintParams[0] = D.Tint.OffsetScale;
                Mask(M.C.OffsetChannel, D.Tint.Offset.Channel);
            }
        }
        if (!D.Color.Empty() && (M.ColorMask = CreatePreviewSRV(Dev, S.ColorMask, false, MaxSize)))
        {
            M.C.Flags |= PreviewHasColorTint;
            for (int c = 0; c < 3; c++) M.C.ColorTint[c] = D.Color.Color[c];
            Mask(M.C.ColorMaskChannel, D.Color.Mask.Channel);
        }
        // two layers (the preview draws the first two); the Pad pair flags their roughness maps
        if (D.Layers.size() >= 2 && !S.LayerAlbedo[0].Empty() && !S.LayerAlbedo[1].Empty())
        {
            M.C.Flags |= PreviewHasLayers;
            for (int l = 0; l < 2; l++)
            {
                const Materials::Layer& L = D.Layers[l];
                M.LayerAlbedo[l] = CreatePreviewSRV(Dev, S.LayerAlbedo[l], true, MaxSize);
                if (!S.LayerNormal[l].Empty()) M.LayerNormal[l] = CreatePreviewSRV(Dev, S.LayerNormal[l], false, MaxSize);
                if (!S.LayerRoughness[l].Empty()) M.LayerRoughness[l] = CreatePreviewSRV(Dev, S.LayerRoughness[l], false, MaxSize);
                M.C.Pad[l] = M.LayerRoughness[l] ? 1.0f : 0.0f;
                const float Layer[4] = { L.UVScale[0], L.UVScale[1], L.UVRotation, M.LayerNormal[l] ? L.NormalScale : 0.0f };
                std::memcpy(M.C.Layer[l], Layer, sizeof(Layer));
                const float Tint[4] = { L.Tint[0], L.Tint[1], L.Tint[2], L.RoughnessValue };
                std::memcpy(M.C.LayerTint[l], Tint, sizeof(Tint));
            }
            const Materials::Layer& Top = D.Layers[1];
            if (!S.LayerMask.Empty() && (M.LayerMask = CreatePreviewSRV(Dev, S.LayerMask, false, MaxSize)))
            {
                M.C.Flags |= PreviewHasLayerMask;
                Mask(M.C.MaskChannel, Top.Mask.Channel);
                M.C.MaskScale[0] = Top.MaskUVScale[0];
                M.C.MaskScale[1] = Top.MaskUVScale[1];
            }
        }
        // light through the surface: the map as data (sampled as a value, not a colour)
        if (!D.Transmission.Empty() && (M.Transmission = CreatePreviewSRV(Dev, S.Transmission, false, MaxSize)))
        {
            M.C.Flags |= PreviewHasTransmission;
            for (int c = 0; c < 3; c++) M.C.TransmissionColor[c] = D.Transmission.Color[c];
            M.C.TransmissionColor[3] = D.Transmission.Image.Invert ? 1.0f : 0.0f;
            Mask(M.C.TransmissionMask, D.Transmission.Image.Channel);
        }
        if (!D.AlphaMap.Empty() && (M.AlphaMap = CreatePreviewSRV(Dev, S.AlphaMap, false, MaxSize)))
        {
            M.C.Flags |= PreviewHasAlphaMap;
            const float Params[4] = { D.AlphaMap.Low, D.AlphaMap.High, D.AlphaMap.UVScale[0], D.AlphaMap.UVScale[1] };
            std::memcpy(M.C.AlphaParams, Params, sizeof(Params));
            Mask(M.C.AlphaMask, D.AlphaMap.Image.Channel);
        }
        if (S.Alpha == Materials::AlphaMode::Cutout && M.Albedo) { M.C.Flags |= PreviewCutout; M.Blend = 1; }
        if (S.Alpha == Materials::AlphaMode::Blend && M.Albedo) { M.C.Flags |= PreviewBlended; M.Blend = 2; }
        return M;
    }
}

// Each triangle's layer: 0, or one more than the highest layer an earlier material has on its plane
// (same facing, within Radius * 1e-4), up to MaxLayer. A decal or a blend layer lies on the surface it
// covers (BO4's xmodel_f8983b06e342392: a rust blend surface on the painted hull, a drip decal on the
// rust, triangle for triangle): drawn at one depth they z-fight. ByMat: the triangles of each material,
// untextured first, in draw order.
static std::vector<uint8_t> OverlayLayers(const PreviewGeometry& Geo, const std::vector<std::vector<uint32_t>>& ByMat, float Radius)
{
    const size_t TriCount = Geo.Idx.size() / 3;
    std::vector<uint8_t> Layers(TriCount, 0);
    size_t Used = 0;
    for (const auto& Tris : ByMat) Used += !Tris.empty();
    if (Used < 2) return Layers;

    const size_t VertexCount = Geo.Pos.size() / 3;
    const double Step = std::max(1e-6, (double)Radius * 1e-4);
    // the plane's key: its normal (11 bits a component) and its distance (31 bits) quantised
    const auto PlaneOf = [&](uint32_t t, int64_t& Nx, int64_t& Ny, int64_t& Nz, int64_t& D) {
        uint32_t V[3];
        for (int k = 0; k < 3; k++) { V[k] = Geo.Idx[t * 3 + k]; if (V[k] >= VertexCount) return false; }
        const float* A = &Geo.Pos[V[0] * 3]; const float* B = &Geo.Pos[V[1] * 3]; const float* C = &Geo.Pos[V[2] * 3];
        const double U[3] = { (double)B[0] - A[0], (double)B[1] - A[1], (double)B[2] - A[2] };
        const double W[3] = { (double)C[0] - A[0], (double)C[1] - A[1], (double)C[2] - A[2] };
        double N[3] = { U[1] * W[2] - U[2] * W[1], U[2] * W[0] - U[0] * W[2], U[0] * W[1] - U[1] * W[0] };
        const double L = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
        if (L < 1e-12) return false;
        for (double& c : N) c /= L;
        Nx = std::llround(N[0] * 512.0); Ny = std::llround(N[1] * 512.0); Nz = std::llround(N[2] * 512.0);
        D = std::llround((N[0] * A[0] + N[1] * A[1] + N[2] * A[2]) / Step);
        return true;
    };
    const auto Key = [](int64_t Nx, int64_t Ny, int64_t Nz, int64_t D) {
        return ((uint64_t)(Nx & 0x7FF) << 53) | ((uint64_t)(Ny & 0x7FF) << 42) | ((uint64_t)(Nz & 0x7FF) << 31) | ((uint64_t)D & 0x7FFFFFFFull);
    };
    std::unordered_map<uint64_t, uint8_t> Planes;   // a plane's highest layer so far
    std::vector<uint64_t> Mine;
    for (const auto& Tris : ByMat)
    {
        Mine.clear();
        for (const uint32_t t : Tris)
        {
            int64_t Nx, Ny, Nz, D;
            if (!PlaneOf(t, Nx, Ny, Nz, D)) { Mine.push_back(~0ull); continue; }
            int Top = -1;
            for (int64_t Dd = -1; Dd <= 1 && !Planes.empty(); Dd++)
                if (const auto It = Planes.find(Key(Nx, Ny, Nz, D + Dd)); It != Planes.end()) Top = std::max(Top, (int)It->second);
            Layers[t] = (uint8_t)std::min(Top + 1, PreviewGpuMesh::MaxLayer);
            Mine.push_back(Key(Nx, Ny, Nz, D));
        }
        // the material's planes join after it is done: its own triangles side by side are no overlay
        for (size_t i = 0; i < Tris.size(); i++)
            if (Mine[i] != ~0ull) { uint8_t& L = Planes[Mine[i]]; L = std::max(L, Layers[Tris[i]]); }
    }
    return Layers;
}

PreviewGpuMesh UploadPreviewMesh(ID3D11Device* Dev, const PreviewGeometry& Geo, int MaxTextureSize)
{
    PreviewGpuMesh M;
    if (Geo.Pos.size() < 3 || Geo.Idx.empty()) return M;
    auto MakeBuffer = [&](const void* Data, UINT Bytes, UINT Bind) -> ComPtr<ID3D11Buffer> {
        D3D11_BUFFER_DESC Bd{}; Bd.ByteWidth = Bytes; Bd.Usage = D3D11_USAGE_IMMUTABLE; Bd.BindFlags = Bind;
        D3D11_SUBRESOURCE_DATA Sd{}; Sd.pSysMem = Data;
        ComPtr<ID3D11Buffer> B; Dev->CreateBuffer(&Bd, &Sd, &B); return B;
    };
    const std::vector<float> Vtx = InterleavePreviewVertices(Geo.Pos, Geo.Nrm, Geo.UV);
    M.VB = MakeBuffer(Vtx.data(), (UINT)(Vtx.size() * sizeof(float)), D3D11_BIND_VERTEX_BUFFER);

    PreviewBoundingSphere(Geo, M.Center, M.Radius);

    // Triangles grouped by material, so each draw binds one material, and within one by layer
    // (OverlayLayers): counted per (material, layer), then placed (one pass each, a map has a
    // thousand materials and millions of triangles)
    const int MatCount = (int)Geo.Materials.size();
    const size_t TriCount = Geo.Idx.size() / 3, VtxMats = Geo.VtxMat.size();
    const uint32_t* const Idx = Geo.Idx.data();
    const int32_t* const VtxMat = Geo.VtxMat.data();
    std::vector<std::vector<uint32_t>> ByMat(MatCount + 1);   // 0: no material, m + 1: material m
    for (size_t t = 0; t < TriCount; t++)
    {
        const uint32_t a = Idx[t * 3];
        const int TriMat = a < VtxMats ? VtxMat[a] : -1;
        ByMat[TriMat >= 0 && TriMat < MatCount ? TriMat + 1 : 0].push_back((uint32_t)t);
    }
    const std::vector<uint8_t> Layers = OverlayLayers(Geo, ByMat, M.Radius);
    constexpr int LayerCount = PreviewGpuMesh::MaxLayer + 1;
    std::vector<size_t> Bucket((size_t)(MatCount + 1) * LayerCount + 1, 0);   // (m + 1) * LayerCount + layer
    for (int b = 0; b <= MatCount; b++)
        for (const uint32_t t : ByMat[b]) Bucket[(size_t)b * LayerCount + Layers[t] + 1] += 3;
    for (size_t b = 1; b < Bucket.size(); b++) Bucket[b] += Bucket[b - 1];
    for (int m = -1; m < MatCount; m++)
        for (int Layer = 0; Layer < LayerCount; Layer++)
        {
            const size_t B = (size_t)(m + 1) * LayerCount + Layer;
            if (const size_t Count = Bucket[B + 1] - Bucket[B])
                M.Ranges.push_back({ m, (UINT)Bucket[B], (UINT)Count, Layer });
        }
    std::vector<uint32_t> SortedIdx(TriCount * 3);
    uint32_t* const Sorted = SortedIdx.data();
    for (int b = 0; b <= MatCount; b++)
        for (const uint32_t t : ByMat[b])
        {
            size_t& To = Bucket[(size_t)b * LayerCount + Layers[t]];
            Sorted[To] = Idx[t * 3]; Sorted[To + 1] = Idx[t * 3 + 1]; Sorted[To + 2] = Idx[t * 3 + 2];
            To += 3;
        }
    M.IB = MakeBuffer(SortedIdx.data(), (UINT)(SortedIdx.size() * sizeof(uint32_t)), D3D11_BIND_INDEX_BUFFER);

    M.Mats.resize(MatCount);
    for (int m = 0; m < MatCount; m++)
    {
        if (m < (int)Geo.MaterialMaps.size())
            M.Mats[m] = GpuMaterial(Dev, Geo.MaterialMaps[m], MaxTextureSize);
        else
        {
            const auto& T = Geo.Materials[m];
            PreviewMaterial Plain;
            Plain.Albedo = PreviewImageFromRGBA(T.Width, T.Height, T.Rgba);
            M.Mats[m] = GpuMaterial(Dev, Plain, MaxTextureSize);
        }
        if (M.Mats[m].Albedo) M.TexCount++;
    }

    M.FrameDist = (M.Radius / std::tan(XMConvertToRadians(35.0f) * 0.5f)) * 1.15f;
    M.VertexCount = (UINT)(Geo.Pos.size() / 3);
    M.TriCount = (UINT)(Geo.Idx.size() / 3);
    M.Valid = M.VB && M.IB;
    return M;
}

void PreviewBoundingSphere(const PreviewGeometry& Geo, XMVECTOR& Center, float& Radius)
{
    const size_t VertexCount = Geo.Pos.size() / 3;
    const float* P = Geo.Pos.data();
    float Mn[3] = { P[0], P[1], P[2] }, Mx[3] = { P[0], P[1], P[2] };
    for (size_t i = 0; i < VertexCount; i++, P += 3)
        for (int c = 0; c < 3; c++) { if (P[c] < Mn[c]) Mn[c] = P[c]; if (P[c] > Mx[c]) Mx[c] = P[c]; }
    const float C[3] = { (Mn[0] + Mx[0]) * 0.5f, (Mn[1] + Mx[1]) * 0.5f, (Mn[2] + Mx[2]) * 0.5f };
    Center = XMVectorSet(C[0], C[1], C[2], 0);
    float R2 = 1e-8f;
    P = Geo.Pos.data();
    for (size_t i = 0; i < VertexCount; i++, P += 3)
    {
        const float dx = P[0] - C[0], dy = P[1] - C[1], dz = P[2] - C[2];
        const float D2 = dx * dx + dy * dy + dz * dz;
        if (D2 > R2) R2 = D2;
    }
    Radius = std::sqrt(R2);
}

void FramePreviewForTurn(const PreviewGeometry& Geo, float Pitch, PreviewGpuMesh& M, bool ByCount)
{
    const size_t N = Geo.Pos.size() / 3, Tris = Geo.Idx.size() / 3;
    if (N == 0 || Tris == 0) return;
    const size_t Step = std::max<size_t>(1, Tris / 200000);
    struct Tri { float C[3]; float Area; };
    std::vector<Tri> T;
    std::vector<float> Areas;   // ByCount: the triangles' own, for the pad below
    double Total = 0;
    for (size_t t = 0; t < Tris; t += Step)
    {
        const uint32_t I[3] = { Geo.Idx[t * 3], Geo.Idx[t * 3 + 1], Geo.Idx[t * 3 + 2] };
        if (I[0] >= N || I[1] >= N || I[2] >= N) continue;
        const float* A = &Geo.Pos[I[0] * 3]; const float* B = &Geo.Pos[I[1] * 3]; const float* C = &Geo.Pos[I[2] * 3];
        const float U[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] }, V[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
        const float X[3] = { U[1] * V[2] - U[2] * V[1], U[2] * V[0] - U[0] * V[2], U[0] * V[1] - U[1] * V[0] };
        const float Area = 0.5f * std::sqrt(X[0] * X[0] + X[1] * X[1] + X[2] * X[2]) + 1e-9f;
        Tri E{ { (A[0] + B[0] + C[0]) / 3, (A[1] + B[1] + C[1]) / 3, (A[2] + B[2] + C[2]) / 3 }, ByCount ? 1.0f : Area };
        Total += E.Area;
        if (ByCount) Areas.push_back(Area);
        T.push_back(E);
    }
    if (T.empty()) return;
    float Lo[3], Hi[3];
    for (int c = 0; c < 3; c++)
    {
        std::sort(T.begin(), T.end(), [c](const Tri& X, const Tri& Y) { return X.C[c] < Y.C[c]; });
        double Acc = 0;
        size_t i = 0;
        for (; i + 1 < T.size() && (Acc += T[i].Area) < Total * 0.01; i++) {}
        Lo[c] = T[i].C[c];
        Acc = 0;
        size_t j = T.size() - 1;
        for (; j > i && (Acc += T[j].Area) < Total * 0.01; j--) {}
        Hi[c] = T[j].C[c];
    }
    // the centroids' box is a little inside the surfaces' own: half a typical triangle more (by
    // count, the median one: a few vista planes would make the mean huge)
    if (ByCount) std::nth_element(Areas.begin(), Areas.begin() + Areas.size() / 2, Areas.end());
    const float Pad = std::sqrt(ByCount ? Areas[Areas.size() / 2] : (float)(Total / T.size())) * 0.5f;
    for (int c = 0; c < 3; c++) { Lo[c] -= Pad; Hi[c] += Pad; }
    const float Half[3] = { (Hi[0] - Lo[0]) * 0.5f, (Hi[1] - Lo[1]) * 0.5f, (Hi[2] - Lo[2]) * 0.5f };
    const float Radius = std::max(1e-3f, std::sqrt(Half[0] * Half[0] + Half[1] * Half[1] + Half[2] * Half[2]));
    const float Tan = std::tan(XMConvertToRadians(35.0f) * 0.5f);
    const float Cp = std::cos(Pitch), Sp = std::sin(Pitch);
    float Need = 0;
    for (int k = 0; k < 24; k++)
    {
        const float Y = k * XM_2PI / 24;
        const float D[3] = { Cp * std::cos(Y), Cp * std::sin(Y), Sp };                 // centre to camera
        const float R[3] = { -std::sin(Y), std::cos(Y), 0 };                           // right
        const float U[3] = { -Sp * std::cos(Y), -Sp * std::sin(Y), Cp };              // up
        for (int Corner = 0; Corner < 8; Corner++)
        {
            const float P[3] = { (Corner & 1) ? Half[0] : -Half[0], (Corner & 2) ? Half[1] : -Half[1], (Corner & 4) ? Half[2] : -Half[2] };
            const float Depth = P[0] * D[0] + P[1] * D[1] + P[2] * D[2];
            const float X = std::abs(P[0] * R[0] + P[1] * R[1]), Yv = std::abs(P[0] * U[0] + P[1] * U[1] + P[2] * U[2]);
            Need = std::max(Need, Depth + std::max(X, Yv) / Tan);
        }
    }
    M.Center = XMVectorSet((Lo[0] + Hi[0]) * 0.5f, (Lo[1] + Hi[1]) * 0.5f, (Lo[2] + Hi[2]) * 0.5f, 0);
    M.Radius = Radius;
    M.FrameDist = std::max(Need * 1.06f, Radius * 0.5f);
}

PreviewCamera OrbitPreviewCamera(const PreviewGpuMesh& Mesh, float Yaw, float Pitch, float DistScale, XMFLOAT3 Pan)
{
    PreviewCamera C;
    const float cp = std::cos(Pitch), sp = std::sin(Pitch);
    const XMVECTOR Dir = XMVector3Normalize(XMVectorSet(cp * std::cos(Yaw), cp * std::sin(Yaw), sp, 0));
    const float Dist = Mesh.FrameDist * DistScale;
    const XMVECTOR Offset = XMLoadFloat3(&Pan);
    const XMVECTOR Target = XMVectorAdd(Mesh.Center, Offset);
    XMStoreFloat3(&C.Eye, XMVectorAdd(Target, XMVectorScale(Dir, Dist)));
    XMStoreFloat3(&C.Target, Target);
    C.Fov = XMConvertToRadians(35.0f);
    C.Near = Mesh.Radius * 0.02f;
    C.Far = Dist + Mesh.Radius * 3.0f + XMVectorGetX(XMVector3Length(Offset));
    return C;
}

void PanPreviewCamera(const PreviewCamera& Cam, float Dx, float Dy, float ViewH, XMFLOAT3& Pan)
{
    const XMVECTOR Eye = XMLoadFloat3(&Cam.Eye), Target = XMLoadFloat3(&Cam.Target);
    const XMVECTOR Forward = XMVector3Normalize(XMVectorSubtract(Target, Eye));
    const XMVECTOR Right = XMVector3Normalize(XMVector3Cross(Forward, XMLoadFloat3(&Cam.Up)));
    const XMVECTOR Up = XMVector3Cross(Right, Forward);
    // world units under one pixel at the target's depth
    const float PerPixel = 2.0f * XMVectorGetX(XMVector3Length(XMVectorSubtract(Target, Eye))) *
                           std::tan(Cam.Fov * 0.5f) / std::max(1.0f, ViewH);
    const XMVECTOR Move = XMVectorAdd(XMVectorScale(Right, -Dx * PerPixel), XMVectorScale(Up, Dy * PerPixel));
    XMStoreFloat3(&Pan, XMVectorAdd(XMLoadFloat3(&Pan), Move));
}

bool PreviewTarget::Resize(ID3D11Device* Dev, int NewW, int NewH)
{
    NewW = std::max(1, NewW); NewH = std::max(1, NewH);
    if (NewW == W && NewH == H && OutputSRV) return true;
    W = NewW; H = NewH;
    Hdr.Reset(); HdrResolved.Reset(); Straight.Reset(); Output.Reset(); HdrRTV.Reset(); StraightRTV.Reset(); OutputRTV.Reset();
    DSV.Reset(); HdrSRV.Reset(); StraightSRV.Reset(); OutputSRV.Reset();

    const DXGI_FORMAT HdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Samples = 4;
    UINT Q = 0;
    if (FAILED(Dev->CheckMultisampleQualityLevels(HdrFormat, Samples, &Q)) || Q == 0) Samples = 1;

    auto Tex = [&](DXGI_FORMAT F, UINT S, UINT Bind, ComPtr<ID3D11Texture2D>& Out) {
        D3D11_TEXTURE2D_DESC Td{}; Td.Width = W; Td.Height = H; Td.MipLevels = 1; Td.ArraySize = 1;
        Td.Format = F; Td.SampleDesc.Count = S; Td.Usage = D3D11_USAGE_DEFAULT; Td.BindFlags = Bind;
        return SUCCEEDED(Dev->CreateTexture2D(&Td, nullptr, &Out));
    };
    ComPtr<ID3D11Texture2D> Depth;
    if (!Tex(HdrFormat, Samples, D3D11_BIND_RENDER_TARGET, Hdr) ||
        !Tex(DXGI_FORMAT_D32_FLOAT, Samples, D3D11_BIND_DEPTH_STENCIL, Depth) ||
        !Tex(HdrFormat, 1, D3D11_BIND_SHADER_RESOURCE, HdrResolved) ||
        !Tex(HdrFormat, 1, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, Straight) ||
        !Tex(DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, Output))
        return false;
    return SUCCEEDED(Dev->CreateRenderTargetView(Hdr.Get(), nullptr, &HdrRTV)) &&
           SUCCEEDED(Dev->CreateDepthStencilView(Depth.Get(), nullptr, &DSV)) &&
           SUCCEEDED(Dev->CreateShaderResourceView(HdrResolved.Get(), nullptr, &HdrSRV)) &&
           SUCCEEDED(Dev->CreateRenderTargetView(Straight.Get(), nullptr, &StraightRTV)) &&
           SUCCEEDED(Dev->CreateShaderResourceView(Straight.Get(), nullptr, &StraightSRV)) &&
           SUCCEEDED(Dev->CreateRenderTargetView(Output.Get(), nullptr, &OutputRTV)) &&
           SUCCEEDED(Dev->CreateShaderResourceView(Output.Get(), nullptr, &OutputSRV));
}

struct PreviewRenderer::Impl
{
    ComPtr<ID3D11Device> Dev;
    std::unique_ptr<PBREffect> Effect;
    std::unique_ptr<CommonStates> States;
    std::unique_ptr<ToneMapPostProcess> ToneMap;
    ComPtr<ID3D11InputLayout> Layout;
    ComPtr<ID3D11PixelShader> PS, UnpremultiplyPS;
    ComPtr<ID3D11VertexShader> FullscreenVS;
    ComPtr<ID3D11Buffer> MaterialCB, SceneCB;
    ComPtr<ID3D11ShaderResourceView> Radiance, Irradiance;
    // no culling, pulled toward the camera by layer (PreviewGpuMesh::Range::Layer; [0] unused)
    ComPtr<ID3D11RasterizerState> Overlay[PreviewGpuMesh::MaxLayer + 1];
    int RadianceMips = 1;
    PreviewLighting Lighting;
    bool Ok = false;

    explicit Impl(ID3D11Device* D) : Dev(D)
    {
        try
        {
            Effect = std::make_unique<PBREffect>(D);
            States = std::make_unique<CommonStates>(D);
            ToneMap = std::make_unique<ToneMapPostProcess>(D);
            if (FAILED(CreateInputLayoutFromEffect<VertexPositionNormalTexture>(D, Effect.get(), Layout.GetAddressOf()))) return;
        }
        catch (...) { return; }

        ComPtr<ID3DBlob> Blob, Err;
        if (FAILED(D3DCompile(kPixelShader, std::strlen(kPixelShader), "PreviewPS", nullptr, nullptr, "PSMain", "ps_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Blob, &Err)))
        {
            if (Err) OutputDebugStringA((const char*)Err->GetBufferPointer());
            return;
        }
        if (FAILED(D->CreatePixelShader(Blob->GetBufferPointer(), Blob->GetBufferSize(), nullptr, &PS))) return;
        ComPtr<ID3DBlob> VsBlob, PsBlob;
        if (FAILED(D3DCompile(kUnpremultiplyShader, std::strlen(kUnpremultiplyShader), "Unpremultiply", nullptr, nullptr, "VSMain", "vs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &VsBlob, nullptr)) ||
            FAILED(D3DCompile(kUnpremultiplyShader, std::strlen(kUnpremultiplyShader), "Unpremultiply", nullptr, nullptr, "PSMain", "ps_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &PsBlob, nullptr)) ||
            FAILED(D->CreateVertexShader(VsBlob->GetBufferPointer(), VsBlob->GetBufferSize(), nullptr, &FullscreenVS)) ||
            FAILED(D->CreatePixelShader(PsBlob->GetBufferPointer(), PsBlob->GetBufferSize(), nullptr, &UnpremultiplyPS)))
            return;

        auto Cb = [&](UINT Bytes, ComPtr<ID3D11Buffer>& Out) {
            D3D11_BUFFER_DESC B{}; B.ByteWidth = Bytes; B.Usage = D3D11_USAGE_DEFAULT; B.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            return SUCCEEDED(D->CreateBuffer(&B, nullptr, &Out));
        };
        if (!Cb(sizeof(PreviewGpuMaterial::Constants), MaterialCB) || !Cb(sizeof(SceneConstants), SceneCB)) return;

        for (int Layer = 1; Layer <= PreviewGpuMesh::MaxLayer; Layer++)
        {
            // a D32_FLOAT depth near 1: a bias unit is 2^-24; slope-scaled for the surfaces seen edge on
            CD3D11_RASTERIZER_DESC R(D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, -64 * Layer, 0.0f, -1.0f * Layer, TRUE, FALSE, TRUE, FALSE);
            if (FAILED(D->CreateRasterizerState(&R, &Overlay[Layer]))) return;
        }
        if (!StudioEnvironment(D, Radiance, RadianceMips, Irradiance)) return;
        Effect->SetIBLTextures(Radiance.Get(), RadianceMips, Irradiance.Get());
        ToneMap->SetOperator(ToneMapPostProcess::ACESFilmic);
        ToneMap->SetTransferFunction(ToneMapPostProcess::SRGB);
        Effect->SetAlpha(1.0f);
        Ok = true;
    }
};

PreviewRenderer::PreviewRenderer(ID3D11Device* Dev) : P(std::make_unique<Impl>(Dev)) {}
PreviewRenderer::~PreviewRenderer() = default;
bool PreviewRenderer::Ok() const { return P->Ok; }
void PreviewRenderer::SetLighting(const PreviewLighting& L) { P->Lighting = L; }

void PreviewRenderer::Render(ID3D11DeviceContext* Ctx, const PreviewGpuMesh* Mesh, const PreviewCamera& Cam, PreviewTarget& T,
                             const float Background[4])
{
    if (!P->Ok || !T.HdrRTV) return;
    // The HDR target holds linear light: the background is cleared to what the display transform
    // (exposure included) turns back into the colour asked for.
    const PreviewLighting& L = P->Lighting;
    const float Unexpose = std::exp2(-L.Exposure);
    const float Clear[4] = { BeforeToneMap(Background[0]) * Unexpose, BeforeToneMap(Background[1]) * Unexpose,
                             BeforeToneMap(Background[2]) * Unexpose, Background[3] };
    P->ToneMap->SetExposure(L.Exposure);
    Ctx->ClearRenderTargetView(T.HdrRTV.Get(), Clear);
    Ctx->ClearDepthStencilView(T.DSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    if (Mesh && Mesh->Valid)
    {
        const XMVECTOR Eye = XMLoadFloat3(&Cam.Eye), At = XMLoadFloat3(&Cam.Target), Up = XMLoadFloat3(&Cam.Up);
        // CoD's world is right-handed (X forward, Y left, Z up): a right-handed view and projection,
        // as the FX preview's. A left-handed pair here showed every model and map mirrored.
        P->Effect->SetMatrices(XMMatrixIdentity(), XMMatrixLookAtRH(Eye, At, Up),
                               XMMatrixPerspectiveFovRH(Cam.Fov, (float)T.W / (float)T.H, Cam.Near, Cam.Far));
        // Three-point light riding with the camera: a warm key above-left, a cool fill low right,
        // a rim from behind. Each is a direction the light travels (from its position to the model).
        const XMVECTOR Fwd = XMVector3Normalize(XMVectorSubtract(At, Eye));
        const XMVECTOR Right = XMVector3Normalize(XMVector3Cross(Fwd, Up));   // the screen's right
        const XMVECTOR CamUp = XMVector3Cross(Right, Fwd);
        auto From = [&](float F, float R, float U) {
            return XMVector3Normalize(XMVectorNegate(XMVectorAdd(XMVectorAdd(XMVectorScale(Fwd, F), XMVectorScale(Right, R)), XMVectorScale(CamUp, U))));
        };
        const float K = L.Lights;
        P->Effect->SetLightDirection(0, From(-1.0f, -0.8f, 0.9f));
        P->Effect->SetLightDiffuseColor(0, XMVectorSet(1.30f * K, 1.24f * K, 1.15f * K, 0));
        P->Effect->SetLightDirection(1, From(-1.0f, 1.0f, -0.1f));
        P->Effect->SetLightDiffuseColor(1, XMVectorSet(0.22f * K, 0.25f * K, 0.30f * K, 0));
        P->Effect->SetLightDirection(2, From(1.0f, 0.4f, 0.8f));
        P->Effect->SetLightDiffuseColor(2, XMVectorSet(0.8f * K, 0.8f * K, 0.85f * K, 0));

        D3D11_VIEWPORT Vp{}; Vp.Width = (float)T.W; Vp.Height = (float)T.H; Vp.MaxDepth = 1.0f;
        Ctx->RSSetViewports(1, &Vp);
        ID3D11RenderTargetView* Rtv[] = { T.HdrRTV.Get() };
        Ctx->OMSetRenderTargets(1, Rtv, T.DSV.Get());
        Ctx->RSSetState(P->States->CullNone());

        P->Effect->Apply(Ctx);
        Ctx->PSSetShader(P->PS.Get(), nullptr, 0);
        Ctx->IASetInputLayout(P->Layout.Get());
        const UINT Stride = 8 * sizeof(float), Offset = 0;
        ID3D11Buffer* Vbs[] = { Mesh->VB.Get() };
        Ctx->IASetVertexBuffers(0, 1, Vbs, &Stride, &Offset);
        Ctx->IASetIndexBuffer(Mesh->IB.Get(), DXGI_FORMAT_R32_UINT, 0);
        Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const SceneConstants Scene{ 0.45f, (float)T.Samples, 0.0f, L.Specular ? 0.0f : 1.0f };
        Ctx->UpdateSubresource(P->SceneCB.Get(), 0, nullptr, &Scene, 0, 0);
        ID3D11Buffer* Cbs[] = { P->MaterialCB.Get(), P->SceneCB.Get() };
        Ctx->PSSetConstantBuffers(1, 2, Cbs);
        ID3D11SamplerState* Samplers[] = { P->States->AnisotropicWrap(), P->States->LinearClamp() };   // 16x anisotropic
        Ctx->PSSetSamplers(0, 2, Samplers);

        // Opaque first, then cutouts (their coverage comes from the shader), then blended surfaces.
        const PreviewGpuMaterial Untextured;
        for (int Pass = 0; Pass < 3; Pass++)
        {
            if (Pass == 0) { Ctx->OMSetBlendState(P->States->Opaque(), nullptr, 0xFFFFFFFF); Ctx->OMSetDepthStencilState(P->States->DepthDefault(), 0); }
            if (Pass == 2) { Ctx->OMSetBlendState(P->States->NonPremultiplied(), nullptr, 0xFFFFFFFF); Ctx->OMSetDepthStencilState(P->States->DepthRead(), 0); }
            int Layer = 0;
            for (const auto& R : Mesh->Ranges)
            {
                const PreviewGpuMaterial& M = (R.Mat >= 0 && R.Mat < (int)Mesh->Mats.size()) ? Mesh->Mats[R.Mat] : Untextured;
                if (M.Blend != Pass) continue;
                if (R.Layer != Layer)
                {
                    Layer = std::clamp(R.Layer, 0, PreviewGpuMesh::MaxLayer);
                    Ctx->RSSetState(Layer ? P->Overlay[Layer].Get() : P->States->CullNone());
                }
                Ctx->UpdateSubresource(P->MaterialCB.Get(), 0, nullptr, &M.C, 0, 0);
                ID3D11ShaderResourceView* Maps[] = { M.Albedo.Get(), M.Normal.Get(), M.Roughness.Get(), M.Emissive.Get() };
                Ctx->PSSetShaderResources(0, 4, Maps);
                ID3D11ShaderResourceView* More[] = { M.Occlusion.Get(), M.Specular.Get(), M.Metalness.Get(), M.Tint.Get(), M.TintOffset.Get(),
                                                     M.LayerAlbedo[0].Get(), M.LayerNormal[0].Get(), M.LayerRoughness[0].Get(),
                                                     M.LayerAlbedo[1].Get(), M.LayerNormal[1].Get(), M.LayerRoughness[1].Get(), M.LayerMask.Get(),
                                                     M.ColorMask.Get(), M.Transmission.Get(), M.AlphaMap.Get() };
                Ctx->PSSetShaderResources(6, 15, More);
                Ctx->DrawIndexed(R.Count, R.Start, 0);
            }
            if (Layer) Ctx->RSSetState(P->States->CullNone());
        }
    }

    if (T.Samples > 1) Ctx->ResolveSubresource(T.HdrResolved.Get(), 0, T.Hdr.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
    else Ctx->CopyResource(T.HdrResolved.Get(), T.Hdr.Get());

    D3D11_VIEWPORT Vp{}; Vp.Width = (float)T.W; Vp.Height = (float)T.H; Vp.MaxDepth = 1.0f;
    Ctx->RSSetViewports(1, &Vp);
    ID3D11ShaderResourceView* ToneMapSource = T.HdrSRV.Get();
    if (Background[3] < 1.0f)
    {
        ID3D11RenderTargetView* Straight[] = { T.StraightRTV.Get() };
        Ctx->OMSetRenderTargets(1, Straight, nullptr);
        Ctx->OMSetBlendState(P->States->Opaque(), nullptr, 0xFFFFFFFF);
        Ctx->OMSetDepthStencilState(P->States->DepthNone(), 0);
        Ctx->RSSetState(P->States->CullNone());
        Ctx->IASetInputLayout(nullptr);
        Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Ctx->VSSetShader(P->FullscreenVS.Get(), nullptr, 0);
        Ctx->PSSetShader(P->UnpremultiplyPS.Get(), nullptr, 0);
        ID3D11ShaderResourceView* Src[] = { T.HdrSRV.Get() };
        Ctx->PSSetShaderResources(0, 1, Src);
        Ctx->Draw(3, 0);
        ID3D11ShaderResourceView* NoSrc[] = { nullptr };
        Ctx->PSSetShaderResources(0, 1, NoSrc);
        ToneMapSource = T.StraightSRV.Get();
    }

    ID3D11RenderTargetView* Out[] = { T.OutputRTV.Get() };
    Ctx->OMSetRenderTargets(1, Out, nullptr);
    P->ToneMap->SetHDRSourceTexture(ToneMapSource);
    P->ToneMap->Process(Ctx);
    ID3D11ShaderResourceView* None[21] = {};
    Ctx->PSSetShaderResources(0, 21, None);
    Ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

bool PreviewRenderer::Screenshot(ID3D11Device* Dev, ID3D11DeviceContext* Ctx, const PreviewGpuMesh& Mesh, const PreviewCamera& Cam,
                                 int W, int H, const std::string& OutPngPath)
{
    ScratchImage Shot;
    return Capture(Dev, Ctx, Mesh, Cam, W, H, Shot) && SavePreviewPng(*Shot.GetImage(0, 0, 0), OutPngPath);
}

bool PreviewRenderer::Capture(ID3D11Device* Dev, ID3D11DeviceContext* Ctx, const PreviewGpuMesh& Mesh, const PreviewCamera& Cam,
                              int W, int H, ScratchImage& Out)
{
    if (!P->Ok || !Mesh.Valid || W > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || H > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) return false;
    PreviewTarget Target;
    if (!Target.Resize(Dev, W, H)) return false;
    const float Transparent[4] = { 0, 0, 0, 0 };
    Render(Ctx, &Mesh, Cam, Target, Transparent);
    return Target.Output && SUCCEEDED(CaptureTexture(Dev, Ctx, Target.Output.Get(), Out));
}

bool SavePreviewTargetPng(ID3D11Device* Dev, ID3D11DeviceContext* Ctx, const PreviewTarget& T, const std::string& OutPngPath)
{
    ScratchImage Shot;
    if (!T.Output || FAILED(CaptureTexture(Dev, Ctx, T.Output.Get(), Shot))) return false;
    return SavePreviewPng(*Shot.GetImage(0, 0, 0), OutPngPath);
}

bool RenderModelGPU(const std::vector<std::string>& SemodelPaths, const std::string& OutPngPath, int Size)
{
    if (Size < 16) Size = 16;

    PreviewGeometry Geo;
    if (!LoadPreviewGeometry(SemodelPaths, Geo)) return false;
    const UINT VertexCount = (UINT)(Geo.Pos.size() / 3);
    if (VertexCount == 0 || Geo.Idx.empty()) return false;

    // Device (hardware, then WARP so it still runs headless / on a GPU-less box)
    ComPtr<ID3D11Device> Dev;
    ComPtr<ID3D11DeviceContext> Ctx;
    const D3D_FEATURE_LEVEL Want[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT Hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, Want, 1, D3D11_SDK_VERSION, &Dev, nullptr, &Ctx);
    if (FAILED(Hr)) Hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, Want, 1, D3D11_SDK_VERSION, &Dev, nullptr, &Ctx);
    if (FAILED(Hr)) return false;

    PreviewRenderer Renderer(Dev.Get());
    const PreviewGpuMesh Mesh = UploadPreviewMesh(Dev.Get(), Geo);
    PreviewTarget Target;
    if (!Renderer.Ok() || !Mesh.Valid || !Target.Resize(Dev.Get(), Size, Size)) return false;

    // Framing robust to outliers (a map's skybox): the mean vertex as the center, the 90th
    // percentile distance as the framing radius, the true maximum only for the far plane.
    XMVECTOR CenterSum = XMVectorZero();
    for (UINT i = 0; i < VertexCount; i++)
        CenterSum = XMVectorAdd(CenterSum, XMVectorSet(Geo.Pos[i * 3], Geo.Pos[i * 3 + 1], Geo.Pos[i * 3 + 2], 0));
    const XMVECTOR Center = XMVectorScale(CenterSum, 1.0f / std::max(1u, VertexCount));
    std::vector<float> Dists(VertexCount);
    float TrueMaxRadius = 1e-4f;
    for (UINT i = 0; i < VertexCount; i++)
    {
        const XMVECTOR Pt = XMVectorSet(Geo.Pos[i * 3], Geo.Pos[i * 3 + 1], Geo.Pos[i * 3 + 2], 0);
        Dists[i] = XMVectorGetX(XMVector3Length(XMVectorSubtract(Pt, Center)));
        TrueMaxRadius = std::max(TrueMaxRadius, Dists[i]);
    }
    float Radius = TrueMaxRadius;
    if (VertexCount >= 20)
    {
        const size_t P90 = (size_t)(Dists.size() * 0.90);
        std::nth_element(Dists.begin(), Dists.begin() + P90, Dists.end());
        Radius = std::max(1e-4f, Dists[P90]);
    }

    PreviewCamera Cam;
    Cam.Fov = XMConvertToRadians(32.0f);
    const float Dist = (Radius / std::tan(Cam.Fov * 0.5f)) * 1.12f;
    const XMVECTOR Dir = XMVector3Normalize(XMVectorSet(-0.55f, -0.8f, -0.32f, 0)); // camera -> model
    XMStoreFloat3(&Cam.Eye, XMVectorSubtract(Center, XMVectorScale(Dir, Dist)));
    XMStoreFloat3(&Cam.Target, Center);
    Cam.Near = Radius * 0.05f;
    Cam.Far = std::max(Dist + Radius * 2.0f, TrueMaxRadius * 1.05f);

    const float Transparent[4] = { 0, 0, 0, 0 };
    Renderer.Render(Ctx.Get(), &Mesh, Cam, Target, Transparent);
    return SavePreviewTargetPng(Dev.Get(), Ctx.Get(), Target, OutPngPath);
}
