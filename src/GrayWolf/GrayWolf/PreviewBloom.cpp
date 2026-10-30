#include "stdafx.h"

#include "PreviewBloom.h"
#include "PreviewD3D.h"

#include <directxtk/PostProcess.h>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    // The bright part of the HDR image, at half its size: four bilinear taps over 4 x 4 pixels, a soft
    // knee below the threshold, the alpha left out (the bloom adds light, never coverage)
    const char* kExtractShader = R"(
cbuffer Params : register(b0)
{
    float2 SrcTexel;
    float  Threshold, Knee;
};
Texture2D<float4> Src : register(t0);
SamplerState Linear : register(s0);
struct VSOut { float4 Pos : SV_Position; float2 UV : TEXCOORD0; };
VSOut VSMain(uint Id : SV_VertexID)
{
    VSOut o;
    o.UV = float2((Id << 1) & 2, Id & 2);
    o.Pos = float4(o.UV * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 PSMain(VSOut i) : SV_Target0
{
    float3 c = 0.25 * (Src.Sample(Linear, i.UV + SrcTexel * float2(-1, -1)).rgb + Src.Sample(Linear, i.UV + SrcTexel * float2(1, -1)).rgb +
                       Src.Sample(Linear, i.UV + SrcTexel * float2(-1, 1)).rgb + Src.Sample(Linear, i.UV + SrcTexel * float2(1, 1)).rgb);
    c = min(c, Threshold * 64);   // a lone very bright pixel stays a glint, not a flare
    float br = max(c.r, max(c.g, c.b));
    float soft = clamp(br - Threshold + Knee, 0, 2 * Knee);
    soft = soft * soft / (4 * Knee + 1e-5);
    return float4(c * (max(soft, br - Threshold) / max(br, 1e-5)), 0);
}
)";

    struct ExtractParams { float SrcTexel[2], Threshold, Knee; };

    constexpr int kLevels = 4;   // 1/2, 1/4, 1/8, 1/16 of the image
}

struct PreviewBloom::Impl
{
    ComPtr<ID3D11Device> Dev;
    std::unique_ptr<BasicPostProcess> Basic;
    std::unique_ptr<DualPostProcess> Dual;
    ComPtr<ID3D11VertexShader> VS;
    ComPtr<ID3D11PixelShader> ExtractPS;
    ComPtr<ID3D11Buffer> CB;
    ComPtr<ID3D11SamplerState> Linear;
    ComPtr<ID3D11RasterizerState> NoCull;
    struct Target
    {
        ComPtr<ID3D11RenderTargetView> Rtv;
        ComPtr<ID3D11ShaderResourceView> Srv;
    };
    struct Level { Target T[2]; int W = 0, H = 0; };   // T[0]: the level, T[1]: its scratch
    Level L[kLevels];
    Target Out;
    int W = 0, H = 0;
    bool Ok = false;

    bool Make(int Width, int Height, Target& T)
    {
        return PreviewD3D::Target(Dev.Get(), Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT, T.Rtv, T.Srv);
    }

    bool Resize(int Width, int Height)
    {
        if (Width == W && Height == H && Out.Srv) return true;
        W = H = 0;
        for (int k = 0; k < kLevels; k++)
        {
            L[k].W = std::max(1, Width >> (k + 1));
            L[k].H = std::max(1, Height >> (k + 1));
            if (!Make(L[k].W, L[k].H, L[k].T[0]) || !Make(L[k].W, L[k].H, L[k].T[1])) return false;
        }
        if (!Make(Width, Height, Out)) return false;
        W = Width; H = Height;
        return true;
    }

    static void Viewport(ID3D11DeviceContext* Ctx, int Width, int Height)
    {
        D3D11_VIEWPORT Vp{};
        Vp.Width = (float)Width; Vp.Height = (float)Height; Vp.MaxDepth = 1.0f;
        Ctx->RSSetViewports(1, &Vp);
    }
    // Into T (Width x Height), nothing left bound as a source
    static void Into(ID3D11DeviceContext* Ctx, const Target& T, int Width, int Height)
    {
        ID3D11ShaderResourceView* None[2] = {};
        Ctx->PSSetShaderResources(0, 2, None);
        ID3D11RenderTargetView* Rtv[] = { T.Rtv.Get() };
        Ctx->OMSetRenderTargets(1, Rtv, nullptr);
        Viewport(Ctx, Width, Height);
    }
};

PreviewBloom::PreviewBloom(ID3D11Device* Dev) : P(std::make_unique<Impl>())
{
    P->Dev = Dev;
    try
    {
        P->Basic = std::make_unique<BasicPostProcess>(Dev);
        P->Dual = std::make_unique<DualPostProcess>(Dev);
    }
    catch (...) { return; }
    ComPtr<ID3DBlob> Vs, Ps;
    if (FAILED(D3DCompile(kExtractShader, std::strlen(kExtractShader), "PreviewBloom", nullptr, nullptr, "VSMain", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Vs, nullptr)) ||
        FAILED(D3DCompile(kExtractShader, std::strlen(kExtractShader), "PreviewBloom", nullptr, nullptr, "PSMain", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Ps, nullptr)) ||
        FAILED(Dev->CreateVertexShader(Vs->GetBufferPointer(), Vs->GetBufferSize(), nullptr, &P->VS)) ||
        FAILED(Dev->CreatePixelShader(Ps->GetBufferPointer(), Ps->GetBufferSize(), nullptr, &P->ExtractPS)))
        return;
    CD3D11_SAMPLER_DESC S(D3D11_DEFAULT);
    S.AddressU = S.AddressV = S.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    CD3D11_RASTERIZER_DESC R(D3D11_DEFAULT);
    R.CullMode = D3D11_CULL_NONE;
    P->Ok = PreviewD3D::ConstantBuffer(Dev, sizeof(ExtractParams), P->CB) && SUCCEEDED(Dev->CreateSamplerState(&S, &P->Linear)) &&
            SUCCEEDED(Dev->CreateRasterizerState(&R, &P->NoCull));
}

PreviewBloom::~PreviewBloom() = default;

ID3D11ShaderResourceView* PreviewBloom::Apply(ID3D11DeviceContext* Ctx, ID3D11ShaderResourceView* Src, int W, int H, float Strength,
                                             float Exposure)
{
    if (!P->Ok || Strength <= 0.0f || !Src || W < 2 || H < 2 || !P->Resize(W, H)) return Src;
    auto& L = P->L;

    // the bright part, at half size; the threshold where the tone map's curve passes ~0.8
    const float Threshold = std::exp2(-Exposure);
    const ExtractParams Params{ { 1.0f / W, 1.0f / H }, Threshold, Threshold * 0.5f };
    Ctx->UpdateSubresource(P->CB.Get(), 0, nullptr, &Params, 0, 0);
    Impl::Into(Ctx, L[0].T[0], L[0].W, L[0].H);
    Ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    Ctx->OMSetDepthStencilState(nullptr, 0);
    Ctx->RSSetState(P->NoCull.Get());
    Ctx->IASetInputLayout(nullptr);
    Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Ctx->VSSetShader(P->VS.Get(), nullptr, 0);
    Ctx->PSSetShader(P->ExtractPS.Get(), nullptr, 0);
    ID3D11Buffer* Cbs[] = { P->CB.Get() };
    Ctx->PSSetConstantBuffers(0, 1, Cbs);
    ID3D11SamplerState* Samplers[] = { P->Linear.Get() };
    Ctx->PSSetSamplers(0, 1, Samplers);
    Ctx->PSSetShaderResources(0, 1, &Src);
    Ctx->Draw(3, 0);

    // halved three times, each level blurred across then down
    for (int k = 0; k < kLevels; k++)
    {
        if (k > 0)
        {
            Impl::Into(Ctx, L[k].T[0], L[k].W, L[k].H);
            P->Basic->SetEffect(BasicPostProcess::DownScale_2x2);
            P->Basic->SetSourceTexture(L[k - 1].T[0].Srv.Get());
            P->Basic->Process(Ctx);
        }
        P->Basic->SetEffect(BasicPostProcess::BloomBlur);
        for (int Pass = 0; Pass < 2; Pass++)
        {
            Impl::Into(Ctx, L[k].T[1 - Pass], L[k].W, L[k].H);
            P->Basic->SetSourceTexture(L[k].T[Pass].Srv.Get());
            P->Basic->SetBloomBlurParameters(Pass == 0, 2.5f, 1.0f);
            P->Basic->Process(Ctx);
        }
    }
    // the levels added up, smallest first, then over the image
    P->Dual->SetEffect(DualPostProcess::Merge);
    for (int k = kLevels - 1; k > 0; k--)
    {
        Impl::Into(Ctx, L[k - 1].T[1], L[k - 1].W, L[k - 1].H);
        P->Dual->SetSourceTexture(L[k - 1].T[0].Srv.Get());
        P->Dual->SetSourceTexture2(L[k].T[0].Srv.Get());
        P->Dual->SetMergeParameters(1.0f, 1.0f);
        P->Dual->Process(Ctx);
        std::swap(L[k - 1].T[0], L[k - 1].T[1]);
    }
    Impl::Into(Ctx, P->Out, W, H);
    P->Dual->SetSourceTexture(Src);
    P->Dual->SetSourceTexture2(L[0].T[0].Srv.Get());
    P->Dual->SetMergeParameters(1.0f, Strength / kLevels);
    P->Dual->Process(Ctx);

    ID3D11ShaderResourceView* None[2] = {};
    Ctx->PSSetShaderResources(0, 2, None);
    Ctx->OMSetRenderTargets(0, nullptr, nullptr);
    return P->Out.Srv.Get();
}
