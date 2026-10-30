#include "stdafx.h"

#include "PreviewPost.h"
#include "ModelPreviewGPU.h"     // PreviewGpuMesh, PreviewCamera, PreviewTarget, PreviewGpuMaterial
#include "PreviewD3D.h"
#include "SettingsManager.h"

#include <directxtk/CommonStates.h>
#include <directxtk/Effects.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "ThirdParty/XeGTAO/XeGTAO.h"     // GTAOConstants, GTAOUpdateConstants (Intel, MIT)
#include "ThirdParty/SMAA/AreaTex.h"      // areaTexBytes (Jimenez et al., MIT)
#include "ThirdParty/SMAA/SearchTex.h"    // searchTexBytes

// Shaders\*.hlsl, compiled by the build into byte arrays (FxCompile in GrayWolf.vcxproj)
#include "GtaoPrefilter_cs.h"
#include "GtaoMain_cs.h"
#include "GtaoDenoise_cs.h"
#include "GtaoDenoiseLast_cs.h"

#include "PostFullscreen_vs.h"
#include "PostAccumulate_ps.h"
#include "PostResolve_ps.h"
#include "PostPremultiply_ps.h"
#include "PostAoView_ps.h"

#include "SmaaEdge_vs.h"
#include "SmaaEdge_ps.h"
#include "SmaaWeights_vs.h"
#include "SmaaWeights_ps.h"
#include "SmaaBlend_vs.h"
#include "SmaaBlend_ps.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// ---- settings

namespace
{
    int ClampMsaa(int S) { return S >= 8 ? 8 : S >= 4 ? 4 : S >= 2 ? 2 : 1; }
    float ToFloat(const std::string& S, float Default)
    {
        char* End = nullptr;
        const float V = std::strtof(S.c_str(), &End);
        return End && End != S.c_str() && std::isfinite(V) ? V : Default;
    }
    bool ToBool(const std::string& S, bool Default)
    {
        if (S == "1" || S == "true" || S == "on" || S == "yes") return true;
        if (S == "0" || S == "false" || S == "off" || S == "no") return false;
        return Default;
    }
    void Clamp(PreviewQuality& Q)
    {
        Q.AoStrength = std::clamp(Q.AoStrength, 0.0f, 4.0f);
        Q.AoRadius = std::clamp(Q.AoRadius, 0.005f, 1.0f);
        Q.Msaa = ClampMsaa(Q.Msaa);
        Q.StillSamples = std::clamp(Q.StillSamples, 1, 256);
    }
}

PreviewQuality LoadPreviewQuality()
{
    const PreviewQuality D;
    PreviewQuality Q;
    Q.Ao = ToBool(SettingsManager::GetSetting("preview_ao", D.Ao ? "true" : "false"), D.Ao);
    Q.AoStrength = ToFloat(SettingsManager::GetSetting("preview_ao_strength", ""), D.AoStrength);
    Q.AoRadius = ToFloat(SettingsManager::GetSetting("preview_ao_radius", ""), D.AoRadius);
    Q.Msaa = (int)ToFloat(SettingsManager::GetSetting("preview_msaa", ""), (float)D.Msaa);
    Q.Smaa = ToBool(SettingsManager::GetSetting("preview_smaa", D.Smaa ? "true" : "false"), D.Smaa);
    Q.StillSamples = (int)ToFloat(SettingsManager::GetSetting("preview_still_samples", ""), (float)D.StillSamples);
    Clamp(Q);
    return Q;
}

void SavePreviewQuality(const PreviewQuality& Q)
{
    SettingsManager::SetSetting("preview_ao", Q.Ao ? "true" : "false");
    SettingsManager::SetSetting("preview_ao_strength", std::to_string(Q.AoStrength));
    SettingsManager::SetSetting("preview_ao_radius", std::to_string(Q.AoRadius));
    SettingsManager::SetSetting("preview_msaa", std::to_string(Q.Msaa));
    SettingsManager::SetSetting("preview_smaa", Q.Smaa ? "true" : "false");
    SettingsManager::SetSetting("preview_still_samples", std::to_string(Q.StillSamples));
}

bool ApplyPreviewQualitySpec(PreviewQuality& Q, const std::string& Spec, std::string* Error)
{
    size_t At = 0;
    while (At < Spec.size())
    {
        size_t End = Spec.find(',', At);
        if (End == std::string::npos) End = Spec.size();
        const std::string Item = Spec.substr(At, End - At);
        At = End + 1;
        if (Item.empty()) continue;
        const size_t Eq = Item.find('=');
        const std::string Key = Item.substr(0, Eq), Value = Eq == std::string::npos ? "1" : Item.substr(Eq + 1);
        const auto Bad = [&] { if (Error) *Error = "bad quality setting '" + Item + "'"; return false; };
        const auto Number = [&](float& Out) { char* E = nullptr; const float V = std::strtof(Value.c_str(), &E); if (E == Value.c_str() || !std::isfinite(V)) return false; Out = V; return true; };
        float N = 0;
        if (Key == "ao" || Key == "smaa")
        {
            const bool B = ToBool(Value, false);
            if (B != ToBool(Value, true)) return Bad();   // neither a yes nor a no
            (Key == "ao" ? Q.Ao : Q.Smaa) = B;
        }
        else if (!Number(N)) return Bad();
        else if (Key == "ao_strength") Q.AoStrength = N;
        else if (Key == "ao_radius") Q.AoRadius = N;
        else if (Key == "msaa") Q.Msaa = (int)N;
        else if (Key == "still_samples" || Key == "accum") Q.StillSamples = (int)N;
        else return Bad();
    }
    Clamp(Q);
    return true;
}

// ---- the passes

// What the post passes keep per target (PreviewTarget::Post), made when first needed at its size.
struct PreviewPostTarget
{
    uint64_t Generation = ~0ull;
    int W = 0, H = 0;

    // the still: what it shows (Begin's key) and how many frames are in the accumulator
    std::vector<uint8_t> Key;
    int Count = 0;
    ComPtr<ID3D11RenderTargetView> AccumRTV;
    ComPtr<ID3D11ShaderResourceView> AccumSRV;

    // XeGTAO: the prepass's depth and normals, the view space depth MIPs, the AO terms and the edges
    bool AoMade = false;
    ComPtr<ID3D11DepthStencilView> DepthDSV;
    ComPtr<ID3D11ShaderResourceView> DepthSRV;
    ComPtr<ID3D11RenderTargetView> NormalRTV;
    ComPtr<ID3D11ShaderResourceView> NormalSRV;
    ComPtr<ID3D11ShaderResourceView> WorkDepthSRV;
    ComPtr<ID3D11UnorderedAccessView> WorkDepthUAV[XE_GTAO_DEPTH_MIP_LEVELS];
    ComPtr<ID3D11ShaderResourceView> AoSRV[2], EdgesSRV;
    ComPtr<ID3D11UnorderedAccessView> AoUAV[2], EdgesUAV;
    ID3D11ShaderResourceView* FinalAo = nullptr;

    // SMAA: the premultiplied input, the edges, the blend weights
    bool SmaaMade = false;
    ComPtr<ID3D11RenderTargetView> ColorRTV, EdgesRTV, BlendRTV;
    ComPtr<ID3D11ShaderResourceView> ColorSRV, SmaaEdgesSRV, BlendSRV;
};

namespace
{
    struct AoConstants
    {
        XMFLOAT4X4 View;     // transposed for HLSL
        float Params[4];     // x: 1 = the occlusion is bound, y: its power
    };
    struct PostConstants
    {
        float Metrics[4];    // 1 / width, 1 / height, width, height
        float Params[4];     // x: 1 / the frames in the accumulator
    };

    // The registers the main shader's hooks read (ShaderPrefix): high, away from the renderer's own.
    constexpr UINT AoTextureSlot = 40, AoConstantSlot = 6;

    float Halton(int Index, int Base)
    {
        float F = 1.0f, R = 0.0f;
        for (int I = Index; I > 0; I /= Base) { F /= Base; R += F * (I % Base); }
        return R;
    }

    template <class T> void Put(std::vector<uint8_t>& K, const T& V)
    {
        const auto* B = reinterpret_cast<const uint8_t*>(&V);
        K.insert(K.end(), B, B + sizeof(T));
    }
}

struct PreviewPost::Impl
{
    ComPtr<ID3D11Device> Dev;
    PreviewQuality Quality = PreviewQuality::Legacy();
    uint64_t Invalidations = 0;
    bool Ok = false;     // the post shaders and states
    bool AoOk = false;   // and the prepass, and a device that gathers R8_UINT

    ComPtr<ID3D11ComputeShader> GtaoPrefilter, GtaoMain, GtaoDenoise, GtaoDenoiseLast;
    ComPtr<ID3D11VertexShader> FullscreenVS, SmaaEdgeVS, SmaaWeightsVS, SmaaBlendVS;
    ComPtr<ID3D11PixelShader> AccumulatePS, ResolvePS, PremultiplyPS, SmaaEdgePS, SmaaWeightsPS, SmaaBlendPS, PrepassPS, AoViewPS;
    bool AoView = false;   // GRAYWOLF_PREVIEW_AO_VIEW=1: the occlusion shown instead of the image (a check)
    ComPtr<ID3D11Buffer> GtaoCB, AoCB, PostCB;
    ComPtr<ID3D11SamplerState> PointClamp, LinearClamp;
    ComPtr<ID3D11BlendState> AddBlend;
    ComPtr<ID3D11RasterizerState> Solid;
    ComPtr<ID3D11DepthStencilState> DepthWrite, NoDepth;
    ComPtr<ID3D11ShaderResourceView> AreaTex, SearchTex;

    // GRAYWOLF_PREVIEW_PROFILE=<file>: each drawn frame timed on the GPU (timestamps at the four hooks),
    // the means appended to the file when the renderer goes
    struct Profile
    {
        std::string File;
        ComPtr<ID3D11Query> Disjoint, Stamp[5];   // Begin, occlusion start and end, End start and end
        bool Open = false, Stamped[5] = {};
        double Sum[4] = {};                       // total, occlusion, scene (the rest), post
        int Frames = 0, Skipped = 0, Warmup = 0;
        std::string Config;
    } Prof;

    explicit Impl(ID3D11Device* D) : Dev(D)
    {
        auto Cs = [&](const BYTE* Code, size_t Size, ComPtr<ID3D11ComputeShader>& Out) { return SUCCEEDED(D->CreateComputeShader(Code, Size, nullptr, &Out)); };
        auto Vs = [&](const BYTE* Code, size_t Size, ComPtr<ID3D11VertexShader>& Out) { return SUCCEEDED(D->CreateVertexShader(Code, Size, nullptr, &Out)); };
        auto Ps = [&](const BYTE* Code, size_t Size, ComPtr<ID3D11PixelShader>& Out) { return SUCCEEDED(D->CreatePixelShader(Code, Size, nullptr, &Out)); };
        auto Cb = [&](UINT Bytes, ComPtr<ID3D11Buffer>& Out) { return PreviewD3D::ConstantBuffer(D, Bytes, Out); };
        if (!Cs(g_GtaoPrefilter_cs, sizeof(g_GtaoPrefilter_cs), GtaoPrefilter) || !Cs(g_GtaoMain_cs, sizeof(g_GtaoMain_cs), GtaoMain) ||
            !Cs(g_GtaoDenoise_cs, sizeof(g_GtaoDenoise_cs), GtaoDenoise) || !Cs(g_GtaoDenoiseLast_cs, sizeof(g_GtaoDenoiseLast_cs), GtaoDenoiseLast) ||
            !Vs(g_PostFullscreen_vs, sizeof(g_PostFullscreen_vs), FullscreenVS) || !Vs(g_SmaaEdge_vs, sizeof(g_SmaaEdge_vs), SmaaEdgeVS) ||
            !Vs(g_SmaaWeights_vs, sizeof(g_SmaaWeights_vs), SmaaWeightsVS) || !Vs(g_SmaaBlend_vs, sizeof(g_SmaaBlend_vs), SmaaBlendVS) ||
            !Ps(g_PostAccumulate_ps, sizeof(g_PostAccumulate_ps), AccumulatePS) || !Ps(g_PostResolve_ps, sizeof(g_PostResolve_ps), ResolvePS) ||
            !Ps(g_PostPremultiply_ps, sizeof(g_PostPremultiply_ps), PremultiplyPS) || !Ps(g_SmaaEdge_ps, sizeof(g_SmaaEdge_ps), SmaaEdgePS) ||
            !Ps(g_SmaaWeights_ps, sizeof(g_SmaaWeights_ps), SmaaWeightsPS) || !Ps(g_SmaaBlend_ps, sizeof(g_SmaaBlend_ps), SmaaBlendPS) ||
            !Ps(g_PostAoView_ps, sizeof(g_PostAoView_ps), AoViewPS) ||
            !Cb(sizeof(XeGTAO::GTAOConstants), GtaoCB) || !Cb(sizeof(AoConstants), AoCB) || !Cb(sizeof(PostConstants), PostCB))
            return;

        CD3D11_SAMPLER_DESC Point(D3D11_DEFAULT);
        Point.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        Point.AddressU = Point.AddressV = Point.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        CD3D11_SAMPLER_DESC Linear(Point);
        Linear.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        D3D11_BLEND_DESC Bd{};
        Bd.RenderTarget[0].BlendEnable = TRUE;
        Bd.RenderTarget[0].SrcBlend = Bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        Bd.RenderTarget[0].SrcBlendAlpha = Bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        Bd.RenderTarget[0].BlendOp = Bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        Bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        const CD3D11_RASTERIZER_DESC Rs(D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE);
        const CD3D11_DEPTH_STENCIL_DESC Write(D3D11_DEFAULT);
        CD3D11_DEPTH_STENCIL_DESC None(D3D11_DEFAULT);
        None.DepthEnable = FALSE;
        None.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(D->CreateSamplerState(&Point, &PointClamp)) || FAILED(D->CreateSamplerState(&Linear, &LinearClamp)) ||
            FAILED(D->CreateBlendState(&Bd, &AddBlend)) || FAILED(D->CreateRasterizerState(&Rs, &Solid)) ||
            FAILED(D->CreateDepthStencilState(&Write, &DepthWrite)) || FAILED(D->CreateDepthStencilState(&None, &NoDepth)))
            return;

        // SMAA's precomputed tables
        if (!PreviewD3D::Immutable(D, AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, areaTexBytes, AREATEX_PITCH, AreaTex) ||
            !PreviewD3D::Immutable(D, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, searchTexBytes, SEARCHTEX_PITCH, SearchTex))
            return;
        Ok = true;

        char Flag[8] = {};
        AoView = GetEnvironmentVariableA("GRAYWOLF_PREVIEW_AO_VIEW", Flag, sizeof(Flag)) > 0 && Flag[0] == '1';
        char File[1024] = {};
        if (const DWORD N = GetEnvironmentVariableA("GRAYWOLF_PREVIEW_PROFILE", File, sizeof(File)); N > 0 && N < sizeof(File))
        {
            D3D11_QUERY_DESC Qd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            bool Made = SUCCEEDED(D->CreateQuery(&Qd, &Prof.Disjoint));
            Qd.Query = D3D11_QUERY_TIMESTAMP;
            for (auto& S : Prof.Stamp) Made = Made && SUCCEEDED(D->CreateQuery(&Qd, &S));
            if (Made) Prof.File = File;
        }
    }

    ~Impl()
    {
        if (Prof.File.empty() || Prof.Frames == 0) return;
        std::ofstream Out(Prof.File, std::ios::app);
        char Line[512];
        std::snprintf(Line, sizeof(Line), "%s frames=%d skipped=%d total_ms=%.3f ao_ms=%.3f scene_ms=%.3f post_ms=%.3f\n", Prof.Config.c_str(),
                      Prof.Frames, Prof.Skipped, Prof.Sum[0] / Prof.Frames, Prof.Sum[1] / Prof.Frames, Prof.Sum[2] / Prof.Frames, Prof.Sum[3] / Prof.Frames);
        Out << Line;
    }

    void Stamp(ID3D11DeviceContext* Ctx, int I)
    {
        if (!Prof.Open || Prof.Stamped[I]) return;
        Ctx->End(Prof.Stamp[I].Get());
        Prof.Stamped[I] = true;
    }

    // The frame's timestamps read back (blocking: a profiling run only).
    void CloseProfile(ID3D11DeviceContext* Ctx, const PreviewTarget& T, const PreviewPostFrame& F)
    {
        if (!Prof.Open) return;
        for (int I = 1; I < 5; I++) Stamp(Ctx, I);
        Ctx->End(Prof.Disjoint.Get());
        Prof.Open = false;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT Dj{};
        while (Ctx->GetData(Prof.Disjoint.Get(), &Dj, sizeof(Dj), 0) == S_FALSE) {}
        UINT64 Ts[5] = {};
        for (int I = 0; I < 5; I++) while (Ctx->GetData(Prof.Stamp[I].Get(), &Ts[I], sizeof(UINT64), 0) == S_FALSE) {}
        // the first frames make the device's caches and pipelines: left out of the means
        if (Dj.Disjoint || Dj.Frequency == 0 || Prof.Warmup++ < 3) return;
        const double Ms = 1000.0 / (double)Dj.Frequency;
        Prof.Sum[0] += (double)(Ts[4] - Ts[0]) * Ms;
        Prof.Sum[1] += (double)(Ts[2] - Ts[1]) * Ms;
        Prof.Sum[2] += (double)((Ts[1] - Ts[0]) + (Ts[3] - Ts[2])) * Ms;
        Prof.Sum[3] += (double)(Ts[4] - Ts[3]) * Ms;
        Prof.Frames++;
        char C[256];
        std::snprintf(C, sizeof(C), "%dx%d msaa=%u smaa=%d ao=%d still=%d", T.W, T.H, T.Samples, Quality.Smaa ? 1 : 0, Quality.Ao && AoOk ? 1 : 0, F.Of);
        Prof.Config = C;
    }

    bool MakeAo(PreviewPostTarget& S)
    {
        if (S.AoMade) return true;
        ID3D11Device* D = Dev.Get();
        ComPtr<ID3D11Texture2D> Depth, Work;
        if (!PreviewD3D::Texture(D, S.W, S.H, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, Depth) ||
            !PreviewD3D::Texture(D, S.W, S.H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, Work, 1, XE_GTAO_DEPTH_MIP_LEVELS))
            return false;
        const CD3D11_DEPTH_STENCIL_VIEW_DESC Dsv(D3D11_DSV_DIMENSION_TEXTURE2D, DXGI_FORMAT_D32_FLOAT);
        const CD3D11_SHADER_RESOURCE_VIEW_DESC DepthSrv(D3D11_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_FLOAT, 0, 1);
        if (FAILED(D->CreateDepthStencilView(Depth.Get(), &Dsv, &S.DepthDSV)) || FAILED(D->CreateShaderResourceView(Depth.Get(), &DepthSrv, &S.DepthSRV)) ||
            FAILED(D->CreateShaderResourceView(Work.Get(), nullptr, &S.WorkDepthSRV)))
            return false;
        for (UINT Mip = 0; Mip < XE_GTAO_DEPTH_MIP_LEVELS; Mip++)
        {
            const CD3D11_UNORDERED_ACCESS_VIEW_DESC Uav(D3D11_UAV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_FLOAT, Mip);
            if (FAILED(D->CreateUnorderedAccessView(Work.Get(), &Uav, &S.WorkDepthUAV[Mip]))) return false;
        }
        if (!PreviewD3D::Target(D, S.W, S.H, DXGI_FORMAT_R10G10B10A2_UNORM, S.NormalRTV, S.NormalSRV) ||
            !PreviewD3D::Storage(D, S.W, S.H, DXGI_FORMAT_R8_UINT, S.AoUAV[0], S.AoSRV[0]) ||
            !PreviewD3D::Storage(D, S.W, S.H, DXGI_FORMAT_R8_UINT, S.AoUAV[1], S.AoSRV[1]) ||
            !PreviewD3D::Storage(D, S.W, S.H, DXGI_FORMAT_R8_UNORM, S.EdgesUAV, S.EdgesSRV))
            return false;
        S.AoMade = true;
        return true;
    }

    bool MakeSmaa(PreviewPostTarget& S)
    {
        if (S.SmaaMade) return true;
        S.SmaaMade = PreviewD3D::Target(Dev.Get(), S.W, S.H, DXGI_FORMAT_R8G8B8A8_UNORM, S.ColorRTV, S.ColorSRV) &&
                     PreviewD3D::Target(Dev.Get(), S.W, S.H, DXGI_FORMAT_R8G8_UNORM, S.EdgesRTV, S.SmaaEdgesSRV) &&
                     PreviewD3D::Target(Dev.Get(), S.W, S.H, DXGI_FORMAT_R8G8B8A8_UNORM, S.BlendRTV, S.BlendSRV);
        return S.SmaaMade;
    }

    bool MakeAccum(PreviewPostTarget& S)
    {
        return S.AccumRTV || PreviewD3D::Target(Dev.Get(), S.W, S.H, DXGI_FORMAT_R32G32B32A32_FLOAT, S.AccumRTV, S.AccumSRV);
    }

    // A full screen pass into Rtv: Vs/Ps, the shader views from t0.
    void Pass(ID3D11DeviceContext* Ctx, ID3D11RenderTargetView* Rtv, ID3D11VertexShader* Vs, ID3D11PixelShader* Ps,
              std::initializer_list<ID3D11ShaderResourceView*> Srvs, ID3D11BlendState* Blend = nullptr)
    {
        Ctx->OMSetRenderTargets(1, &Rtv, nullptr);
        Ctx->OMSetBlendState(Blend, nullptr, 0xFFFFFFFF);
        Ctx->VSSetShader(Vs, nullptr, 0);
        Ctx->PSSetShader(Ps, nullptr, 0);
        ID3D11ShaderResourceView* Views[5] = {};
        std::copy(Srvs.begin(), Srvs.end(), Views);
        Ctx->PSSetShaderResources(0, 5, Views);
        Ctx->Draw(3, 0);
        ID3D11ShaderResourceView* None[5] = {};
        Ctx->PSSetShaderResources(0, 5, None);
        Ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }
};

PreviewPost::PreviewPost(ID3D11Device* Dev) : P(std::make_unique<Impl>(Dev)) {}
PreviewPost::~PreviewPost() = default;
bool PreviewPost::Ok() const { return P->Ok; }
void PreviewPost::SetQuality(const PreviewQuality& Q) { P->Quality = Q; Clamp(P->Quality); }
const PreviewQuality& PreviewPost::Quality() const { return P->Quality; }
void PreviewPost::Invalidate() { P->Invalidations++; }

const char* PreviewPost::ShaderPrefix()
{
    return R"(
// ---- PreviewPost (PreviewPost/PreviewPost.h): what its hooks in this shader call
cbuffer PreviewPostAO : register(b6)
{
    float4x4 PreviewPostView;       // world to view (right handed)
    float4   PreviewPostAOParams;   // x: 1 = the occlusion is bound, y: its power
};
Texture2D<uint> PreviewPostAOMap : register(t40);   // XeGTAO's visibility, 0..255

// The screen space occlusion at this pixel (1: none), for the ambient and image based light.
float PreviewScreenAO(float2 Pixel)
{
    if (PreviewPostAOParams.x < 0.5) return 1;
    return pow(saturate(PreviewPostAOMap.Load(int3(Pixel, 0)) / 255.0), PreviewPostAOParams.y);
}

// Lagarde and de Rousiers, "Moving Frostbite to PBR" (2014): the specular lobe's occlusion from the ambient one.
float PreviewSpecularOcclusion(float NdotV, float ao, float roughness)
{
    return saturate(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
}
// ---- end PreviewPost
)";
}

bool PreviewPost::CompilePrepass(const char* MainShaderSource)
{
    // XeGTAO's input: the depth, and the normal in its view space (x right, y up, z away from the eye),
    // of the opaque and cutout surfaces (the layers over them add nothing)
    static const char* Suffix = R"(
// ---- PreviewPost: the depth and normal prepass XeGTAO reads
float4 PreviewPrepassPS(PSIn i) : SV_Target0
{
    if (Flags & 128)
    {
        float a = (Flags & 1) ? AlbedoMap.Sample(Surface, i.TexCoord).a : 1;
        if (Flags & 65536)
            a = saturate((dot(AlphaMap.Sample(Surface, i.TexCoord * AlphaParams.zw), AlphaMask) - AlphaParams.x) / max(AlphaParams.y - AlphaParams.x, 1e-4));
        clip(a - Cutoff);
    }
    float3 N = normalize(i.NormalWS);
    if (dot(N, EyePosition - i.PositionWS.xyz) < 0) N = -N;
    float3 n = normalize(mul(N, (float3x3)PreviewPostView));
    return float4(float3(n.x, n.y, -n.z) * 0.5 + 0.5, 1);
}
)";
    if (!P->Ok) return false;
    const std::string Source = std::string(ShaderPrefix()) + MainShaderSource + Suffix;
    ComPtr<ID3DBlob> Blob, Err;
    if (FAILED(D3DCompile(Source.data(), Source.size(), "PreviewPrepass", nullptr, nullptr, "PreviewPrepassPS", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Blob, &Err)) ||
        FAILED(P->Dev->CreatePixelShader(Blob->GetBufferPointer(), Blob->GetBufferSize(), nullptr, &P->PrepassPS)))
    {
        if (Err) OutputDebugStringA((const char*)Err->GetBufferPointer());
        return false;
    }
    // XeGTAO writes its AO terms and edges through typed UAVs (R8_UINT, R8_UNORM)
    UINT Uint = 0, Unorm = 0;
    P->AoOk = SUCCEEDED(P->Dev->CheckFormatSupport(DXGI_FORMAT_R8_UINT, &Uint)) && (Uint & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) &&
              SUCCEEDED(P->Dev->CheckFormatSupport(DXGI_FORMAT_R8_UNORM, &Unorm)) && (Unorm & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW);
    return P->AoOk;
}

PreviewPostFrame PreviewPost::Begin(ID3D11DeviceContext* Ctx, const PreviewGpuMesh* Mesh, const PreviewCamera& Cam, PreviewTarget& T,
                                    const float Background[4], const void* Lighting, size_t LightingBytes)
{
    Impl& I = *P;
    PreviewPostFrame F;
    const PreviewQuality& Q = I.Quality;
    // the target's samples a pixel (PreviewTarget::Resize falls back to fewer when the device has not as many)
    if (T.WantSamples != (UINT)Q.Msaa && T.W > 0)
    {
        const int W = T.W, H = T.H;
        T.WantSamples = (UINT)Q.Msaa;
        T.W = 0;
        T.Resize(I.Dev.Get(), W, H);
    }
    if (!T.Post) T.Post = std::make_shared<PreviewPostTarget>();
    PreviewPostTarget& S = *T.Post;
    if (S.Generation != T.Generation || S.W != T.W || S.H != T.H)
    {
        S = PreviewPostTarget{};
        S.Generation = T.Generation; S.W = T.W; S.H = T.H;
    }

    // What the still shows: when any of it changes, the still starts again.
    std::vector<uint8_t> Key;
    Key.reserve(256 + LightingBytes);
    Put(Key, Mesh);
    if (Mesh)
    {
        Put(Key, Mesh->VB.Get()); Put(Key, Mesh->IB.Get()); Put(Key, Mesh->VertexCount); Put(Key, Mesh->TriCount);
        Put(Key, Mesh->Radius); Put(Key, Mesh->Mats.size()); Put(Key, Mesh->Valid);
    }
    Put(Key, Cam);
    Put(Key, T.W); Put(Key, T.H); Put(Key, T.Generation); Put(Key, T.Samples);
    for (int c = 0; c < 4; c++) Put(Key, Background[c]);
    if (Lighting && LightingBytes)
        Key.insert(Key.end(), static_cast<const uint8_t*>(Lighting), static_cast<const uint8_t*>(Lighting) + LightingBytes);
    Put(Key, Q.Ao); Put(Key, Q.AoStrength); Put(Key, Q.AoRadius); Put(Key, Q.Msaa); Put(Key, Q.Smaa); Put(Key, Q.StillSamples);
    Put(Key, I.Invalidations);
    if (Key != S.Key) { S.Key = std::move(Key); S.Count = 0; }

    F.Of = I.Ok ? std::max(1, Q.StillSamples) : 1;
    if (F.Of > 1 && S.Count >= F.Of)
    {
        F.Skip = true;
        I.Prof.Skipped++;
        return F;
    }
    F.Index = F.Of > 1 ? S.Count : 0;
    if (F.Index > 0)
    {
        // a Halton (2, 3) point in the pixel, centred: the frames cover it evenly
        F.Jitter[0] = Halton(F.Index + 1, 2) - 0.5f;
        F.Jitter[1] = Halton(F.Index + 1, 3) - 0.5f;
    }
    if (!I.Prof.File.empty())
    {
        I.Prof.Open = true;
        std::fill(std::begin(I.Prof.Stamped), std::end(I.Prof.Stamped), false);
        Ctx->Begin(I.Prof.Disjoint.Get());
        I.Stamp(Ctx, 0);
    }
    return F;
}

XMMATRIX PreviewPost::Jitter(const PreviewPostFrame& F, FXMMATRIX Proj, const PreviewTarget& T) const
{
    if ((F.Jitter[0] == 0.0f && F.Jitter[1] == 0.0f) || T.W <= 0 || T.H <= 0) return Proj;
    // clip space x' = x + dx * w: the image moved by the jitter's pixels (y down on screen)
    return XMMatrixMultiply(Proj, XMMatrixTranslation(2.0f * F.Jitter[0] / T.W, -2.0f * F.Jitter[1] / T.H, 0.0f));
}

void PreviewPost::Occlusion(ID3D11DeviceContext* Ctx, const PreviewPostFrame& F, const PreviewGpuMesh& Mesh, FXMMATRIX View,
                            const PreviewCamera& Cam, PreviewTarget& T, PBREffect& Effect, ID3D11InputLayout* Layout,
                            ID3D11Buffer* MaterialCB, CommonStates& States)
{
    Impl& I = *P;
    const PreviewQuality& Q = I.Quality;
    if (!I.Ok || !T.Post) return;
    PreviewPostTarget& S = *T.Post;
    const bool On = I.AoOk && Q.Ao && Q.AoStrength > 0.0f && Mesh.Valid && I.MakeAo(S);

    AoConstants C{};
    XMStoreFloat4x4(&C.View, XMMatrixTranspose(View));
    C.Params[0] = On ? 1.0f : 0.0f;
    C.Params[1] = Q.AoStrength;
    Ctx->UpdateSubresource(I.AoCB.Get(), 0, nullptr, &C, 0, 0);
    ID3D11Buffer* AoCb[] = { I.AoCB.Get() };
    Ctx->PSSetConstantBuffers(AoConstantSlot, 1, AoCb);
    ID3D11ShaderResourceView* NoAo[] = { nullptr };
    Ctx->PSSetShaderResources(AoTextureSlot, 1, NoAo);
    S.FinalAo = nullptr;
    if (!On) return;
    I.Stamp(Ctx, 1);

    // the prepass: 1x depth and view space normals, the effect's vertex stage and the main shader's material
    const D3D11_VIEWPORT Vp{ 0, 0, (float)S.W, (float)S.H, 0, 1 };
    Effect.Apply(Ctx);
    Ctx->PSSetShader(I.PrepassPS.Get(), nullptr, 0);
    Ctx->IASetInputLayout(Layout);
    const UINT Stride = 8 * sizeof(float), Offset = 0;
    ID3D11Buffer* Vbs[] = { Mesh.VB.Get() };
    Ctx->IASetVertexBuffers(0, 1, Vbs, &Stride, &Offset);
    Ctx->IASetIndexBuffer(Mesh.IB.Get(), DXGI_FORMAT_R32_UINT, 0);
    Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float Facing[4] = { 0.5f, 0.5f, 0.0f, 1.0f };   // a normal facing the eye
    Ctx->ClearRenderTargetView(S.NormalRTV.Get(), Facing);
    Ctx->ClearDepthStencilView(S.DepthDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11RenderTargetView* Rtv[] = { S.NormalRTV.Get() };
    Ctx->OMSetRenderTargets(1, Rtv, S.DepthDSV.Get());
    Ctx->RSSetViewports(1, &Vp);
    Ctx->RSSetState(I.Solid.Get());
    Ctx->OMSetDepthStencilState(I.DepthWrite.Get(), 0);
    Ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ID3D11SamplerState* Smp[] = { States.AnisotropicWrap() };
    Ctx->PSSetSamplers(0, 1, Smp);
    ID3D11Buffer* MatCb[] = { MaterialCB };
    Ctx->PSSetConstantBuffers(1, 1, MatCb);
    const PreviewGpuMaterial Untextured;
    for (const auto& R : Mesh.Ranges)
    {
        const PreviewGpuMaterial& M = (R.Mat >= 0 && R.Mat < (int)Mesh.Mats.size()) ? Mesh.Mats[R.Mat] : Untextured;
        if (M.Blend == 2 || R.Layer > 0) continue;
        Ctx->UpdateSubresource(MaterialCB, 0, nullptr, &M.C, 0, 0);
        ID3D11ShaderResourceView* Albedo[] = { M.Albedo.Get() };
        ID3D11ShaderResourceView* Alpha[] = { M.AlphaMap.Get() };
        Ctx->PSSetShaderResources(0, 1, Albedo);
        Ctx->PSSetShaderResources(20, 1, Alpha);
        Ctx->DrawIndexed(R.Count, R.Start, 0);
    }
    Ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* NoSrv[1] = {};
    Ctx->PSSetShaderResources(0, 1, NoSrv);
    Ctx->PSSetShaderResources(20, 1, NoSrv);

    // XeGTAO: an interactive frame takes two denoise passes over a fixed noise; a still's later frames
    // take one, each with a new noise the accumulation averages
    const bool Still = F.Of > 1 && F.Index > 0;
    XeGTAO::GTAOSettings Gs;
    Gs.DenoisePasses = Still ? 1 : 2;
    Gs.Radius = std::max(1e-4f, Q.AoRadius * Mesh.Radius);
    // XeGTAO's view space looks down +z: a left-handed projection of the same frustum (the depths match)
    XMFLOAT4X4 Proj;
    XMStoreFloat4x4(&Proj, XMMatrixPerspectiveFovLH(Cam.Fov, (float)S.W / (float)S.H, Cam.Near, Cam.Far));
    XeGTAO::GTAOConstants G{};
    XeGTAO::GTAOUpdateConstants(G, S.W, S.H, Gs, &Proj.m[0][0], true, Still ? (unsigned)F.Index : 0u);
    Ctx->UpdateSubresource(I.GtaoCB.Get(), 0, nullptr, &G, 0, 0);
    ID3D11Buffer* GCb[] = { I.GtaoCB.Get() };
    Ctx->CSSetConstantBuffers(0, 1, GCb);
    ID3D11SamplerState* Point[] = { I.PointClamp.Get() };
    Ctx->CSSetSamplers(0, 1, Point);
    ID3D11UnorderedAccessView* NoUav[XE_GTAO_DEPTH_MIP_LEVELS] = {};
    ID3D11ShaderResourceView* NoCs[2] = {};
    const auto Run = [&](ID3D11ComputeShader* Cs, std::initializer_list<ID3D11ShaderResourceView*> Srvs,
                         std::initializer_list<ID3D11UnorderedAccessView*> Uavs, UINT X, UINT Y) {
        Ctx->CSSetShader(Cs, nullptr, 0);
        Ctx->CSSetShaderResources(0, (UINT)Srvs.size(), Srvs.begin());
        Ctx->CSSetUnorderedAccessViews(0, (UINT)Uavs.size(), Uavs.begin(), nullptr);
        Ctx->Dispatch(X, Y, 1);
        Ctx->CSSetUnorderedAccessViews(0, (UINT)Uavs.size(), NoUav, nullptr);
        Ctx->CSSetShaderResources(0, (UINT)Srvs.size(), NoCs);
    };
    const UINT W = (UINT)S.W, H = (UINT)S.H;
    Run(I.GtaoPrefilter.Get(), { S.DepthSRV.Get() },
        { S.WorkDepthUAV[0].Get(), S.WorkDepthUAV[1].Get(), S.WorkDepthUAV[2].Get(), S.WorkDepthUAV[3].Get(), S.WorkDepthUAV[4].Get() },
        (W + 15) / 16, (H + 15) / 16);
    Run(I.GtaoMain.Get(), { S.WorkDepthSRV.Get(), S.NormalSRV.Get() }, { S.AoUAV[0].Get(), S.EdgesUAV.Get() },
        (W + XE_GTAO_NUMTHREADS_X - 1) / XE_GTAO_NUMTHREADS_X, (H + XE_GTAO_NUMTHREADS_Y - 1) / XE_GTAO_NUMTHREADS_Y);
    int From = 0;
    for (int Pass = 0; Pass < Gs.DenoisePasses; Pass++)
    {
        const bool Last = Pass + 1 == Gs.DenoisePasses;
        Run(Last ? I.GtaoDenoiseLast.Get() : I.GtaoDenoise.Get(), { S.AoSRV[From].Get(), S.EdgesSRV.Get() }, { S.AoUAV[1 - From].Get() },
            ((W + 1) / 2 + XE_GTAO_NUMTHREADS_X - 1) / XE_GTAO_NUMTHREADS_X, (H + XE_GTAO_NUMTHREADS_Y - 1) / XE_GTAO_NUMTHREADS_Y);
        From = 1 - From;
    }
    Ctx->CSSetShader(nullptr, nullptr, 0);
    S.FinalAo = S.AoSRV[From].Get();

    ID3D11ShaderResourceView* Ao[] = { S.FinalAo };
    Ctx->PSSetShaderResources(AoTextureSlot, 1, Ao);
    I.Stamp(Ctx, 2);
}

void PreviewPost::End(ID3D11DeviceContext* Ctx, const PreviewPostFrame& F, PreviewTarget& T)
{
    Impl& I = *P;
    const PreviewQuality& Q = I.Quality;
    if (!I.Ok || !T.Post || !T.OutputRTV) return;
    PreviewPostTarget& S = *T.Post;
    I.Stamp(Ctx, 1); I.Stamp(Ctx, 2);   // no occlusion this frame: none timed
    I.Stamp(Ctx, 3);

    ID3D11ShaderResourceView* NoAo[] = { nullptr };
    Ctx->PSSetShaderResources(AoTextureSlot, 1, NoAo);
    const bool AoView = I.AoView && S.FinalAo;
    const bool Smaa = Q.Smaa && !AoView && I.MakeSmaa(S);
    const bool Accumulate = F.Of > 1 && I.MakeAccum(S);
    if (Smaa || Accumulate || AoView)
    {
        const D3D11_VIEWPORT Vp{ 0, 0, (float)S.W, (float)S.H, 0, 1 };
        Ctx->RSSetViewports(1, &Vp);
        Ctx->RSSetState(I.Solid.Get());
        Ctx->OMSetDepthStencilState(I.NoDepth.Get(), 0);
        Ctx->IASetInputLayout(nullptr);
        Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const PostConstants C{ { 1.0f / S.W, 1.0f / S.H, (float)S.W, (float)S.H }, { 1.0f / (float)(F.Index + 1), 0, 0, 0 } };
        Ctx->UpdateSubresource(I.PostCB.Get(), 0, nullptr, &C, 0, 0);
        ID3D11Buffer* Cb[] = { I.PostCB.Get() };
        Ctx->VSSetConstantBuffers(0, 1, Cb);
        Ctx->PSSetConstantBuffers(0, 1, Cb);
        ID3D11SamplerState* Smp[] = { I.LinearClamp.Get(), I.PointClamp.Get() };
        Ctx->PSSetSamplers(0, 2, Smp);

        if (AoView) I.Pass(Ctx, T.OutputRTV.Get(), I.FullscreenVS.Get(), I.AoViewPS.Get(), { S.FinalAo, S.DepthSRV.Get() });
        if (Smaa)
        {
            // premultiplied input, luma edges, blend weights, then Output blended with its neighbours
            const float Zero[4] = {};
            I.Pass(Ctx, S.ColorRTV.Get(), I.FullscreenVS.Get(), I.PremultiplyPS.Get(), { T.OutputSRV.Get() });
            Ctx->ClearRenderTargetView(S.EdgesRTV.Get(), Zero);
            I.Pass(Ctx, S.EdgesRTV.Get(), I.SmaaEdgeVS.Get(), I.SmaaEdgePS.Get(), { S.ColorSRV.Get() });
            Ctx->ClearRenderTargetView(S.BlendRTV.Get(), Zero);
            I.Pass(Ctx, S.BlendRTV.Get(), I.SmaaWeightsVS.Get(), I.SmaaWeightsPS.Get(), { nullptr, S.SmaaEdgesSRV.Get(), I.AreaTex.Get(), I.SearchTex.Get() });
            I.Pass(Ctx, T.OutputRTV.Get(), I.SmaaBlendVS.Get(), I.SmaaBlendPS.Get(), { S.ColorSRV.Get(), nullptr, nullptr, nullptr, S.BlendSRV.Get() });
        }
        if (Accumulate)
        {
            if (F.Index == 0)
            {
                const float Zero[4] = {};
                Ctx->ClearRenderTargetView(S.AccumRTV.Get(), Zero);
            }
            I.Pass(Ctx, S.AccumRTV.Get(), I.FullscreenVS.Get(), I.AccumulatePS.Get(), { T.OutputSRV.Get() }, I.AddBlend.Get());
            // the first frame is already the mean of one
            if (F.Index > 0) I.Pass(Ctx, T.OutputRTV.Get(), I.FullscreenVS.Get(), I.ResolvePS.Get(), { S.AccumSRV.Get() });
            S.Count = F.Index + 1;
        }
        Ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    }
    I.Stamp(Ctx, 4);
    I.CloseProfile(Ctx, T, F);
}
