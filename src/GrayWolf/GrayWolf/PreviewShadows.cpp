#include "stdafx.h"

#include "PreviewShadows.h"
#include "ModelPreviewGPU.h"   // PreviewGpuMesh, PreviewGpuMaterial, PreviewCamera
#include "PreviewD3D.h"
#include "SettingsManager.h"
#include "ImGuiTheme.h"        // Theme::Pill

#include "imgui.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    // The constants both the model's pixel shader and the ground read (b4); mirrored in kShadowHlsl.
    struct ShadowConstants
    {
        XMFLOAT4X4 ShadowViewProj{};
        XMFLOAT4X4 ContactViewProj{};
        float A[4]{};      // 1 = shadows on, the coarser texel (world), the map's depth range (world), tan(light's angular radius)
        float B[4]{};      // uv per world unit across and up, the kernel's least and largest radius (world)
        float C[4]{};      // normal offset (world), constant bias (depth), largest receiver slope (depth per uv), -
        float Key[4]{};    // towards the key light
        float GroundA[4]{};   // ground z, shadow opacity, contact opacity, 1 = ground on
        float GroundB[4]{};   // contact blur radius (contact uv), the fade's start and end radius (world), -
        float GroundC[4]{};   // the model's centre xy, the ground's half size, -
    };

    // the caster pass: position through the light's view, alpha test for cut-out and blended surfaces
    struct CasterConstants
    {
        XMFLOAT4X4 ViewProj{};
        float AlphaParams[4]{};
        float AlphaMask[4]{};
        float Cutoff = 0;
        uint32_t Flags = 0;
        float Pad[2]{};
    };

    const char* kShadowHlsl = R"(
cbuffer PreviewShadowConstants : register(b4)
{
    float4x4 ShadowViewProj;
    float4x4 ContactViewProj;
    float4 ShadowA;
    float4 ShadowB;
    float4 ShadowC;
    float4 ShadowKey;
    float4 GroundA;
    float4 GroundB;
    float4 GroundC;
};
Texture2D<float> ShadowMap  : register(t30);
Texture2D<float> ContactMap : register(t31);
SamplerComparisonState ShadowCompare : register(s6);
SamplerState ShadowLinear : register(s7);   // border 1: nothing there

// light 0's visibility at this pixel, set by the pixel shader before the lighting
static float KeyShadow = 1;

// Jimenez's interleaved gradient noise: the kernel's rotation per pixel
float ShadowNoise(float2 Px) { return frac(52.9829189 * frac(dot(Px, float2(0.06711056, 0.00583715)))); }
// Vogel's spiral: k of n points spread evenly over the unit disc
float2 ShadowDisk(int k, int n, float Phi)
{
    float r = sqrt((k + 0.5) / n), a = k * 2.39996323 + Phi;
    return r * float2(cos(a), sin(a));
}
float3 ShadowCoord(float3 P)
{
    float4 c = mul(float4(P, 1), ShadowViewProj);
    return float3(c.xy * float2(0.5, -0.5) + 0.5, c.z);
}
// the receiver's depth per uv on its own plane (from its screen derivatives), so each tap of a wide
// kernel compares against the surface under it, not the pixel's centre: no acne on slopes
float2 ReceiverSlope(float3 C)
{
    float3 dx = ddx(C), dy = ddy(C);
    float det = dx.x * dy.y - dx.y * dy.x;
    if (abs(det) < 1e-14) return 0;
    float2 g = float2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / det;
    float l = length(g);
    return l > ShadowC.z ? g * (ShadowC.z / l) : g;
}

// 1 lit .. 0 in the key light's shadow, at P (world), N its normal toward the viewer, Px the pixel.
// Offset: a surface of the model (pushed off itself along N); the ground casts nothing, needs none.
float PreviewShadowAt(float3 P, float3 N, float2 Px, bool Offset)
{
    float NdotL = dot(N, ShadowKey.xyz);
    if (Offset) P += N * ShadowC.x * (0.5 + sqrt(saturate(1 - NdotL * NdotL)));
    float3 C = ShadowCoord(P);
    float2 Slope = ReceiverSlope(C);
    if (ShadowA.x < 0.5 || any(C.xy < 0) || any(C.xy > 1)) return 1;
    // past the far plane (the ground under the model) only what the map holds can shadow: no tap
    // compares beyond its cleared depth
    const float Far = 1 - 1e-5;
    float Z = min(C.z, Far) - ShadowC.y;
    float Phi = ShadowNoise(Px) * 6.2831853;
    // the blockers: looked for as wide as a penumbra could get from here to the map's near plane
    float Search = clamp(ShadowA.w * C.z * ShadowA.z, ShadowB.z, ShadowB.w);
    float Sum = 0, Count = 0;
    [unroll] for (int k = 0; k < 8; k++)   // 8 gathers, 32 texels
    {
        float2 o = ShadowDisk(k, 8, Phi) * Search * ShadowB.xy;
        float4 d = ShadowMap.GatherRed(ShadowLinear, C.xy + o);
        float4 Blocks = d < min(Z + dot(Slope, o), Far);
        Sum += dot(d, Blocks); Count += dot(Blocks, 1);
    }
    // nothing in the way: lit; everything: the narrower kernel below would find the same
    if (Count == 0) return 1;
    if (Count == 32) return 0;
    // the penumbra a light of that angular size gives over the distance to them (PCSS)
    float Radius = clamp(ShadowA.w * (C.z - Sum / Count) * ShadowA.z, ShadowB.z, ShadowB.w);
    float Lit = 0;
    [unroll] for (int j = 0; j < 20; j++)
    {
        float2 o = ShadowDisk(j, 20, Phi + 1.7) * Radius * ShadowB.xy;
        Lit += ShadowMap.SampleCmpLevelZero(ShadowCompare, C.xy + o, min(Z + dot(Slope, o), Far));
    }
    return Lit / 20;
}
)";

    // The casters (the model into a depth map) and the ground (after the model), behind kShadowHlsl.
    const char* kPassesHlsl = R"(
cbuffer Caster : register(b0)
{
    float4x4 CasterViewProj;
    float4 AlphaParams, AlphaMask;
    float Cutoff; uint Flags; float2 CasterPad;
};
cbuffer GroundView : register(b5) { float4x4 CameraViewProj; };
Texture2D<float4> Albedo   : register(t0);
Texture2D<float4> AlphaMap : register(t1);
SamplerState Surface : register(s0);

struct CasterOut { float4 Pos : SV_Position; float2 UV : TEXCOORD0; };
CasterOut CasterVS(float3 P : POSITION, float2 UV : TEXCOORD0)
{
    CasterOut o;
    o.Pos = mul(float4(P, 1), CasterViewProj);
    o.UV = UV;
    return o;
}
// the preview's own alpha (its pixel shader's first lines): cut where it would be
void CasterAlphaPS(CasterOut i)
{
    float a = Albedo.Sample(Surface, i.UV).a;
    if (Flags & 65536)
        a = saturate((dot(AlphaMap.Sample(Surface, i.UV * AlphaParams.zw), AlphaMask) - AlphaParams.x) / max(AlphaParams.y - AlphaParams.x, 1e-4));
    clip(a - Cutoff);
}

struct GroundOut { float4 Pos : SV_Position; float3 World : TEXCOORD0; };
GroundOut GroundVS(uint Id : SV_VertexID)
{
    float2 c = float2(Id & 1, Id >> 1) * 2 - 1;
    GroundOut o;
    o.World = float3(GroundC.xy + c * GroundC.z, GroundA.x);
    o.Pos = mul(float4(o.World, 1), CameraViewProj);
    return o;
}
// how much the model hangs over this point of the ground, blurred: 1 touching .. 0 at the contact height
float GroundContact(float3 P, float Phi)
{
    float4 c = mul(float4(P, 1), ContactViewProj);
    float2 uv = c.xy * float2(0.5, -0.5) + 0.5;
    float Occ = 0;
    [unroll] for (int k = 0; k < 16; k++)
    {
        float h = ContactMap.SampleLevel(ShadowLinear, uv + ShadowDisk(k, 16, Phi) * GroundB.x, 0);
        Occ += (1 - h) * (1 - h);
    }
    return Occ / 16;
}
// The shadow catcher: black at the coverage of the shadow, so the target keeps colour * (1 - a) and its
// alpha gains a (the blend state): the background, or a transparent PNG's alpha, darkened.
float4 GroundPS(GroundOut i) : SV_Target0
{
    float Lit = PreviewShadowAt(i.World, float3(0, 0, 1), i.Pos.xy, false);
    float Shade = (1 - Lit) * saturate(ShadowKey.z * 4);   // a light from above only
    float Contact = GroundContact(i.World, ShadowNoise(i.Pos.xy + 23) * 6.2831853);
    float Fade = 1 - smoothstep(GroundB.y, GroundB.z, distance(i.World.xy, GroundC.xy));
    float a = 1 - (1 - Shade * GroundA.y) * (1 - Contact * GroundA.z);
    return float4(0, 0, 0, a * Fade);
}
)";

    bool Compile(const std::string& Src, const char* Entry, const char* Target, ComPtr<ID3DBlob>& Out)
    {
        ComPtr<ID3DBlob> Err;
        if (SUCCEEDED(D3DCompile(Src.data(), Src.size(), "PreviewShadows", nullptr, nullptr, Entry, Target,
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Out, &Err)))
            return true;
        if (Err) OutputDebugStringA((const char*)Err->GetBufferPointer());
        return false;
    }

    XMFLOAT4X4 Transposed(FXMMATRIX M)
    {
        XMFLOAT4X4 F{};
        XMStoreFloat4x4(&F, XMMatrixTranspose(M));
        return F;
    }

    constexpr float kDefaultSoftness = 0.5f;
}

PreviewShadowSettings LoadPreviewShadowSettings()
{
    PreviewShadowSettings S;
    S.Shadows = SettingsManager::GetSetting("preview_shadows", "true") != "false";
    S.Ground = SettingsManager::GetSetting("preview_ground", "true") != "false";
    S.Softness = SettingsManager::GetSettingFloat("preview_shadow_softness", kDefaultSoftness, 0.0f, 1.0f);
    return S;
}

void SavePreviewShadowSettings(const PreviewShadowSettings& S)
{
    SettingsManager::SetSetting("preview_shadows", S.Shadows ? "true" : "false");
    SettingsManager::SetSetting("preview_ground", S.Ground ? "true" : "false");
    SettingsManager::SetSetting("preview_shadow_softness", std::to_string(S.Softness));
}

bool DrawPreviewShadowControls(PreviewShadowSettings& S)
{
    bool Changed = false;
    ImGui::SameLine(0, ImGui::GetFontSize() * 0.4f);
    if (Theme::Pill("Shadows", S.Shadows)) { S.Shadows = !S.Shadows; Changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The key light's soft shadow, on the model and on the ground");
    if (S.Shadows)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
        ImGui::SliderFloat("##shadowsoftness", &S.Softness, 0.0f, 1.0f, "soft %.2f");
        Changed |= ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The key light's size: sharp shadows at 0, wide soft ones at 1 (right-click resets)");
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { S.Softness = kDefaultSoftness; Changed = true; }
    }
    ImGui::SameLine();
    if (Theme::Pill("Ground", S.Ground)) { S.Ground = !S.Ground; Changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A ground under the model that shows only its shadow and where it touches");
    return Changed;
}

const char* PreviewShadowHlsl() { return kShadowHlsl; }

void PreviewMeshBox(const std::vector<float>& Pos, XMFLOAT3& Min, XMFLOAT3& Max)
{
    Min = { FLT_MAX, FLT_MAX, FLT_MAX };
    Max = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (size_t i = 0; i + 2 < Pos.size(); i += 3)
    {
        Min.x = std::min(Min.x, Pos[i]); Min.y = std::min(Min.y, Pos[i + 1]); Min.z = std::min(Min.z, Pos[i + 2]);
        Max.x = std::max(Max.x, Pos[i]); Max.y = std::max(Max.y, Pos[i + 1]); Max.z = std::max(Max.z, Pos[i + 2]);
    }
    if (Min.x > Max.x) Min = Max = {};
}

struct PreviewShadows::Impl
{
    static constexpr UINT MapSize = 2048, ContactSize = 512;
    ComPtr<ID3D11DepthStencilView> MapDSV, ContactDSV;
    ComPtr<ID3D11ShaderResourceView> MapSRV, ContactSRV;
    ComPtr<ID3D11VertexShader> CasterVS, GroundVS;
    ComPtr<ID3D11PixelShader> CasterAlphaPS, GroundPS;
    ComPtr<ID3D11InputLayout> Layout;
    ComPtr<ID3D11Buffer> CasterCB, ShadowCB, GroundCB;
    ComPtr<ID3D11RasterizerState> CasterRS, ContactRS, GroundRS;
    ComPtr<ID3D11SamplerState> Compare, Linear, Surface;
    ComPtr<ID3D11BlendState> Catcher;
    ComPtr<ID3D11DepthStencilState> DepthWrite, DepthTest;
    ShadowConstants C{};
    bool Ground = false;
    bool Ok = false;

    explicit Impl(ID3D11Device* Dev)
    {
        auto Depth = [&](UINT Size, ComPtr<ID3D11DepthStencilView>& Dsv, ComPtr<ID3D11ShaderResourceView>& Srv) {
            ComPtr<ID3D11Texture2D> Tex;
            D3D11_DEPTH_STENCIL_VIEW_DESC Dd{}; Dd.Format = DXGI_FORMAT_D32_FLOAT; Dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            D3D11_SHADER_RESOURCE_VIEW_DESC Sd{}; Sd.Format = DXGI_FORMAT_R32_FLOAT; Sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            Sd.Texture2D.MipLevels = 1;
            return PreviewD3D::Texture(Dev, (int)Size, (int)Size, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, Tex) &&
                   SUCCEEDED(Dev->CreateDepthStencilView(Tex.Get(), &Dd, &Dsv)) &&
                   SUCCEEDED(Dev->CreateShaderResourceView(Tex.Get(), &Sd, &Srv));
        };
        if (!Depth(MapSize, MapDSV, MapSRV) || !Depth(ContactSize, ContactDSV, ContactSRV)) return;

        const std::string Src = std::string(kShadowHlsl) + kPassesHlsl;
        ComPtr<ID3DBlob> CasterVsBlob, AlphaBlob, GroundVsBlob, GroundPsBlob;
        if (!Compile(Src, "CasterVS", "vs_5_0", CasterVsBlob) || !Compile(Src, "CasterAlphaPS", "ps_5_0", AlphaBlob) ||
            !Compile(Src, "GroundVS", "vs_5_0", GroundVsBlob) || !Compile(Src, "GroundPS", "ps_5_0", GroundPsBlob))
            return;
        if (FAILED(Dev->CreateVertexShader(CasterVsBlob->GetBufferPointer(), CasterVsBlob->GetBufferSize(), nullptr, &CasterVS)) ||
            FAILED(Dev->CreatePixelShader(AlphaBlob->GetBufferPointer(), AlphaBlob->GetBufferSize(), nullptr, &CasterAlphaPS)) ||
            FAILED(Dev->CreateVertexShader(GroundVsBlob->GetBufferPointer(), GroundVsBlob->GetBufferSize(), nullptr, &GroundVS)) ||
            FAILED(Dev->CreatePixelShader(GroundPsBlob->GetBufferPointer(), GroundPsBlob->GetBufferSize(), nullptr, &GroundPS)))
            return;
        // the preview's vertex records: pos3, nrm3, uv2
        const D3D11_INPUT_ELEMENT_DESC Elements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(Dev->CreateInputLayout(Elements, 2, CasterVsBlob->GetBufferPointer(), CasterVsBlob->GetBufferSize(), &Layout))) return;

        using PreviewD3D::ConstantBuffer;
        if (!ConstantBuffer(Dev, sizeof(CasterConstants), CasterCB) || !ConstantBuffer(Dev, sizeof(ShadowConstants), ShadowCB) ||
            !ConstantBuffer(Dev, sizeof(XMFLOAT4X4), GroundCB))
            return;

        // the casters: both faces (the exports' winding varies), not clipped at the near and far planes
        // (clamped: a caster just outside still shadows), a little slope bias for the edge-on ones
        CD3D11_RASTERIZER_DESC Rs(D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0.0f, 1.0f, FALSE, FALSE, FALSE, FALSE);
        if (FAILED(Dev->CreateRasterizerState(&Rs, &CasterRS))) return;
        Rs.SlopeScaledDepthBias = 0.0f; Rs.DepthClipEnable = TRUE;   // the contact map: nothing above its height
        if (FAILED(Dev->CreateRasterizerState(&Rs, &ContactRS))) return;
        Rs.DepthClipEnable = FALSE;   // the ground: whole even past the camera's far plane
        if (FAILED(Dev->CreateRasterizerState(&Rs, &GroundRS))) return;

        D3D11_SAMPLER_DESC Sd{};
        Sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        Sd.AddressU = Sd.AddressV = Sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        Sd.BorderColor[0] = Sd.BorderColor[1] = Sd.BorderColor[2] = Sd.BorderColor[3] = 1.0f;
        Sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        Sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(Dev->CreateSamplerState(&Sd, &Compare))) return;
        Sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; Sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        if (FAILED(Dev->CreateSamplerState(&Sd, &Linear))) return;
        Sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        Sd.AddressU = Sd.AddressV = Sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        if (FAILED(Dev->CreateSamplerState(&Sd, &Surface))) return;

        // colour * (1 - a), alpha + a * (1 - alpha): the shadow over what is there, or as a PNG's alpha
        D3D11_BLEND_DESC Bd{};
        auto& Rt = Bd.RenderTarget[0];
        Rt.BlendEnable = TRUE;
        Rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; Rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; Rt.BlendOp = D3D11_BLEND_OP_ADD;
        Rt.SrcBlendAlpha = D3D11_BLEND_ONE; Rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; Rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        Rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(Dev->CreateBlendState(&Bd, &Catcher))) return;

        CD3D11_DEPTH_STENCIL_DESC Ds(D3D11_DEFAULT);
        if (FAILED(Dev->CreateDepthStencilState(&Ds, &DepthWrite))) return;
        Ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; Ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(Dev->CreateDepthStencilState(&Ds, &DepthTest))) return;
        Ok = true;
    }

    // Every range of Mesh into Dsv as ViewProj sees it; cut-out and blended ones through their alpha.
    void DrawCasters(ID3D11DeviceContext* Ctx, const PreviewGpuMesh& Mesh, ID3D11DepthStencilView* Dsv, UINT Size,
                     FXMMATRIX ViewProj, ID3D11RasterizerState* Rs)
    {
        Ctx->OMSetRenderTargets(0, nullptr, Dsv);
        Ctx->ClearDepthStencilView(Dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT Vp{}; Vp.Width = Vp.Height = (float)Size; Vp.MaxDepth = 1.0f;
        Ctx->RSSetViewports(1, &Vp);
        Ctx->RSSetState(Rs);
        CasterConstants K{};
        K.ViewProj = Transposed(ViewProj);
        Ctx->UpdateSubresource(CasterCB.Get(), 0, nullptr, &K, 0, 0);
        const PreviewGpuMaterial Untextured;
        ID3D11PixelShader* Bound = nullptr;
        Ctx->PSSetShader(nullptr, nullptr, 0);
        for (const auto& R : Mesh.Ranges)
        {
            const PreviewGpuMaterial& M = (R.Mat >= 0 && R.Mat < (int)Mesh.Mats.size()) ? Mesh.Mats[R.Mat] : Untextured;
            ID3D11PixelShader* Ps = M.Blend != 0 && M.Albedo ? CasterAlphaPS.Get() : nullptr;
            if (Ps)
            {
                std::memcpy(K.AlphaParams, M.C.AlphaParams, sizeof(K.AlphaParams));
                std::memcpy(K.AlphaMask, M.C.AlphaMask, sizeof(K.AlphaMask));
                K.Cutoff = M.Blend == 1 ? M.C.Cutoff : 0.5f;   // a blended surface: its more opaque half
                K.Flags = M.C.Flags;
                Ctx->UpdateSubresource(CasterCB.Get(), 0, nullptr, &K, 0, 0);
                ID3D11ShaderResourceView* Maps[] = { M.Albedo.Get(), M.AlphaMap.Get() };
                Ctx->PSSetShaderResources(0, 2, Maps);
            }
            if (Ps != Bound) { Ctx->PSSetShader(Ps, nullptr, 0); Bound = Ps; }
            Ctx->DrawIndexed(R.Count, R.Start, 0);
        }
    }
};

PreviewShadows::PreviewShadows(ID3D11Device* Dev) : P(std::make_unique<Impl>(Dev)) {}
PreviewShadows::~PreviewShadows() = default;
bool PreviewShadows::Ok() const { return P->Ok; }

void PreviewShadows::RenderMaps(ID3D11DeviceContext* Ctx, const PreviewGpuMesh& Mesh, FXMVECTOR KeyDir,
                                const PreviewShadowSettings& S, float LightScale)
{
    Impl& I = *P;
    if (!I.Ok) return;
    ID3D11ShaderResourceView* NoMaps[2] = {};
    Ctx->PSSetShaderResources(30, 2, NoMaps);   // last frame's, before they are depth targets again
    ShadowConstants& C = I.C;
    C = {};
    I.Ground = false;
    // a map (framed by FramePreviewForTurn: no fit points) is a scene, not a model on a ground
    const bool Model = Mesh.Valid && !Mesh.FitPoints.empty() && Mesh.BoxMin.x < Mesh.BoxMax.x;
    const bool Shadows = Model && S.Shadows && LightScale > 0.0f, Ground = Model && S.Ground;
    if (Shadows || Ground)
    {
        const XMVECTOR Lo = XMLoadFloat3(&Mesh.BoxMin), Hi = XMLoadFloat3(&Mesh.BoxMax);
        const XMVECTOR Center = XMVectorScale(XMVectorAdd(Lo, Hi), 0.5f);
        const float Size = std::max(1e-3f, XMVectorGetX(XMVector3Length(XMVectorSubtract(Hi, Lo))));
        const float GroundZ = Mesh.BoxMin.z;
        const UINT Stride = 8 * sizeof(float), Offset = 0;
        ID3D11Buffer* Vbs[] = { Mesh.VB.Get() };
        Ctx->IASetVertexBuffers(0, 1, Vbs, &Stride, &Offset);
        Ctx->IASetIndexBuffer(Mesh.IB.Get(), DXGI_FORMAT_R32_UINT, 0);
        Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Ctx->IASetInputLayout(I.Layout.Get());
        Ctx->VSSetShader(I.CasterVS.Get(), nullptr, 0);
        ID3D11Buffer* Cbs[] = { I.CasterCB.Get() };
        Ctx->VSSetConstantBuffers(0, 1, Cbs);
        Ctx->PSSetConstantBuffers(0, 1, Cbs);
        ID3D11SamplerState* Smp[] = { I.Surface.Get() };
        Ctx->PSSetSamplers(0, 1, Smp);
        Ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        Ctx->OMSetDepthStencilState(I.DepthWrite.Get(), 0);

        if (Shadows)
        {
            // the light's view, fitted to the box's corners as it sees them (a long weapon gets a long map)
            const XMVECTOR Dir = XMVector3Normalize(KeyDir);
            const XMVECTOR Up = std::abs(XMVectorGetZ(Dir)) > 0.99f ? XMVectorSet(1, 0, 0, 0) : XMVectorSet(0, 0, 1, 0);
            const XMMATRIX View = XMMatrixLookAtRH(XMVectorSubtract(Center, XMVectorScale(Dir, Size)), Center, Up);
            float Mn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, Mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            for (int k = 0; k < 8; k++)
            {
                const XMVECTOR Corner = XMVectorSelect(Lo, Hi, XMVectorSelectControl(k & 1, (k >> 1) & 1, (k >> 2) & 1, 0));
                XMFLOAT3 V{};
                XMStoreFloat3(&V, XMVector3TransformCoord(Corner, View));
                for (int c = 0; c < 3; c++) { Mn[c] = std::min(Mn[c], (&V.x)[c]); Mx[c] = std::max(Mx[c], (&V.x)[c]); }
            }
            const float Pad = 0.04f * std::max(Mx[0] - Mn[0], Mx[1] - Mn[1]) + 1e-3f;
            for (int c = 0; c < 3; c++) { Mn[c] -= Pad; Mx[c] += Pad; }
            const float W = Mx[0] - Mn[0], H = Mx[1] - Mn[1], Range = Mx[2] - Mn[2];
            // right-handed: what the light sees is at -z
            const XMMATRIX Proj = XMMatrixOrthographicOffCenterRH(Mn[0], Mx[0], Mn[1], Mx[1], -Mx[2], -Mn[2]);
            const XMMATRIX ViewProj = XMMatrixMultiply(View, Proj);
            I.DrawCasters(Ctx, Mesh, I.MapDSV.Get(), Impl::MapSize, ViewProj, I.CasterRS.Get());

            const float Texel = std::max(W, H) / Impl::MapSize;
            // the key light's angular radius: 0.4 to 10 degrees
            const float Angle = XMConvertToRadians(0.4f + 9.6f * S.Softness * S.Softness);
            C.ShadowViewProj = Transposed(ViewProj);
            const float A[4] = { 1.0f, Texel, Range, std::tan(Angle) };
            const float B[4] = { 1.0f / W, 1.0f / H, Texel * 1.25f, Texel * 64.0f };
            // the receiver's slope limited to ~85 degrees (an edge-on surface would push its taps off)
            const float Cc[4] = { Texel * 1.0f, Texel * 0.35f / Range, 11.0f * std::max(W, H) / Range, 0.0f };
            std::memcpy(C.A, A, sizeof(A)); std::memcpy(C.B, B, sizeof(B)); std::memcpy(C.C, Cc, sizeof(Cc));
            XMFLOAT3 L{};
            XMStoreFloat3(&L, XMVectorNegate(Dir));
            const float Key[4] = { L.x, L.y, L.z, 0.0f };
            std::memcpy(C.Key, Key, sizeof(Key));
        }
        if (Ground)
        {
            // the model seen from just under the ground, up to the contact height: how high it is over each point
            const float Height = 0.12f * Size, Blur = 0.5f * Height;
            const float Hx = (Mesh.BoxMax.x - Mesh.BoxMin.x) * 0.5f + Blur + Height, Hy = (Mesh.BoxMax.y - Mesh.BoxMin.y) * 0.5f + Blur + Height;
            const float Cx = XMVectorGetX(Center), Cy = XMVectorGetY(Center);
            const float Below = 1e-3f * Size;
            const XMMATRIX View = XMMatrixLookAtRH(XMVectorSet(Cx, Cy, GroundZ - Below, 0), XMVectorSet(Cx, Cy, GroundZ + 1.0f, 0), XMVectorSet(0, 1, 0, 0));
            const XMMATRIX ViewProj = XMMatrixMultiply(View, XMMatrixOrthographicOffCenterRH(-Hx, Hx, -Hy, Hy, 0.0f, Below + Height));
            I.DrawCasters(Ctx, Mesh, I.ContactDSV.Get(), Impl::ContactSize, ViewProj, I.ContactRS.Get());
            C.ContactViewProj = Transposed(ViewProj);
            const float Opacity = 0.62f * std::min(1.0f, LightScale);
            const float GA[4] = { GroundZ, Opacity, 0.55f, 1.0f };
            const float GB[4] = { Blur / (2.0f * std::max(Hx, Hy)), 0.9f * Size, 1.6f * Size, 0.0f };
            const float GC[4] = { Cx, Cy, 1.7f * Size, 0.0f };
            std::memcpy(C.GroundA, GA, sizeof(GA)); std::memcpy(C.GroundB, GB, sizeof(GB)); std::memcpy(C.GroundC, GC, sizeof(GC));
            I.Ground = true;
        }
        Ctx->OMSetRenderTargets(0, nullptr, nullptr);
        Ctx->PSSetShader(nullptr, nullptr, 0);
        ID3D11ShaderResourceView* NoSrv[2] = {};
        Ctx->PSSetShaderResources(0, 2, NoSrv);
    }
    Ctx->UpdateSubresource(I.ShadowCB.Get(), 0, nullptr, &C, 0, 0);
}

void PreviewShadows::Bind(ID3D11DeviceContext* Ctx)
{
    Impl& I = *P;
    if (!I.Ok) return;
    ID3D11Buffer* Cbs[] = { I.ShadowCB.Get() };
    Ctx->PSSetConstantBuffers(4, 1, Cbs);
    ID3D11ShaderResourceView* Maps[] = { I.MapSRV.Get(), I.ContactSRV.Get() };
    Ctx->PSSetShaderResources(30, 2, Maps);
    ID3D11SamplerState* Smp[] = { I.Compare.Get(), I.Linear.Get() };
    Ctx->PSSetSamplers(6, 2, Smp);
}

void PreviewShadows::DrawGround(ID3D11DeviceContext* Ctx, const PreviewCamera& Cam, float Aspect)
{
    Impl& I = *P;
    if (!I.Ok) return;
    // not from under it: the ground is seen from above only
    if (I.Ground && Cam.Eye.z > I.C.GroundA[0])
    {
        const XMMATRIX ViewProj = XMMatrixMultiply(
            XMMatrixLookAtRH(XMLoadFloat3(&Cam.Eye), XMLoadFloat3(&Cam.Target), XMLoadFloat3(&Cam.Up)),
            XMMatrixPerspectiveFovRH(Cam.Fov, Aspect, Cam.Near, Cam.Far));
        const XMFLOAT4X4 Vp = Transposed(ViewProj);
        Ctx->UpdateSubresource(I.GroundCB.Get(), 0, nullptr, &Vp, 0, 0);
        Bind(Ctx);
        ID3D11Buffer* Cbs[] = { I.ShadowCB.Get(), I.GroundCB.Get() };
        Ctx->VSSetConstantBuffers(4, 2, Cbs);
        Ctx->IASetInputLayout(nullptr);
        Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        Ctx->VSSetShader(I.GroundVS.Get(), nullptr, 0);
        Ctx->PSSetShader(I.GroundPS.Get(), nullptr, 0);
        Ctx->RSSetState(I.GroundRS.Get());
        Ctx->OMSetBlendState(I.Catcher.Get(), nullptr, 0xFFFFFFFF);
        Ctx->OMSetDepthStencilState(I.DepthTest.Get(), 0);
        Ctx->Draw(4, 0);
    }
    ID3D11ShaderResourceView* NoMaps[2] = {};
    Ctx->PSSetShaderResources(30, 2, NoMaps);
}
