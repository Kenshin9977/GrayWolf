#include "stdafx.h"

#include "FxPreviewRender.h"

#include "FxPreviewGeometry.h"
#include "ModelPreview.h"      // LoadPreviewGeometry: a model element's mesh
#include "PreviewTextures.h"
#include "DirectXTex.h"
#include "ExportNaming.h"
#include "Strings.h"
#include "AppLog.h"

#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <map>
#include <future>
#include <mutex>
#include <unordered_map>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;
namespace fs = std::filesystem;

namespace FxPreview
{
namespace
{
    const char kShader[] = R"(
cbuffer Scene : register(b0) { float4x4 ViewProj; float4 Params; };
Texture2D Tex : register(t0);
SamplerState Smp : register(s0);
struct VSIn { float3 Pos : POSITION; float2 Uv : TEXCOORD0; float4 Col : COLOR0; };
struct PSIn { float4 Pos : SV_POSITION; float2 Uv : TEXCOORD0; float4 Col : COLOR0; };
PSIn VSMain(VSIn I) { PSIn O; O.Pos = mul(float4(I.Pos, 1), ViewProj); O.Uv = I.Uv; O.Col = I.Col; return O; }
// Params.x: 1 = premultiplied blending (the colour times its alpha); Params.y: the brightness
// (an emissive's)
float4 PSMain(PSIn I) : SV_Target
{
    float4 C = Tex.Sample(Smp, I.Uv) * I.Col;
    C.rgb *= Params.y;
    // past white an emissive bleeds to white as a camera's exposure would show it (fire's core
    // yellow-white, not a clipped orange)
    float Peak = max(C.r, max(C.g, C.b));
    if (Peak > 1) C.rgb = lerp(C.rgb / Peak, float3(1, 1, 1), saturate((Peak - 1) / (Peak + 1)));
    if (Params.x > 0) C.rgb *= C.a;
    return C;
}
float4 PSFlat(PSIn I) : SV_Target { return I.Col; }
// a model element: its mesh placed by World, its colour map lit by one light and an ambient
cbuffer Model : register(b1) { float4x4 World; float4 Tint; };
struct MIn { float3 Pos : POSITION; float3 Nrm : NORMAL; float2 Uv : TEXCOORD0; };
struct MOut { float4 Pos : SV_POSITION; float3 Nrm : NORMAL; float2 Uv : TEXCOORD0; };
MOut VSModel(MIn I)
{
    MOut O;
    O.Pos = mul(mul(float4(I.Pos, 1), World), ViewProj);
    O.Nrm = mul(float4(I.Nrm, 0), World).xyz;
    O.Uv = I.Uv;
    return O;
}
float4 PSModel(MOut I) : SV_Target
{
    float4 A = Tex.Sample(Smp, I.Uv) * Tint;
    float L = 0.45 + 0.55 * saturate(dot(normalize(I.Nrm), normalize(float3(0.4, 0.3, 0.85))));
    return float4(A.rgb * L, 1);
}
)";

    struct ModelConstants { XMFLOAT4X4 World; float Tint[4]; };

    // an exported model on the GPU: pos3 nrm3 uv2, its triangles by material
    struct GpuModel
    {
        ComPtr<ID3D11Buffer> VB, IB;
        struct Range { ComPtr<ID3D11ShaderResourceView> Srv; UINT Start, Count; };
        std::vector<Range> Ranges;
        bool Ok = false;
    };

    struct Constants { XMFLOAT4X4 ViewProj; float Params[4]; };

    using Geometry::Vertex;
    using Geometry::ToLinear;
}

// ---- Target

bool Target::Resize(ID3D11Device* Dev, int NewW, int NewH)
{
    NewW = std::max(1, NewW);
    NewH = std::max(1, NewH);
    if (NewW == W && NewH == H && OutputSRV) return true;
    W = NewW;
    H = NewH;
    Color.Reset(); Output.Reset(); RTV.Reset(); DSV.Reset(); OutputSRV.Reset();
    Samples = 4;
    UINT Q = 0;
    if (FAILED(Dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, Samples, &Q)) || Q == 0) Samples = 1;
    const auto Tex = [&](DXGI_FORMAT F, UINT S, UINT Bind, ComPtr<ID3D11Texture2D>& Out) {
        D3D11_TEXTURE2D_DESC Td{};
        Td.Width = W; Td.Height = H; Td.MipLevels = 1; Td.ArraySize = 1;
        Td.Format = F; Td.SampleDesc.Count = S; Td.Usage = D3D11_USAGE_DEFAULT; Td.BindFlags = Bind;
        return SUCCEEDED(Dev->CreateTexture2D(&Td, nullptr, &Out));
    };
    ComPtr<ID3D11Texture2D> Depth;
    if (!Tex(DXGI_FORMAT_R8G8B8A8_TYPELESS, Samples, D3D11_BIND_RENDER_TARGET, Color) ||
        !Tex(DXGI_FORMAT_D32_FLOAT, Samples, D3D11_BIND_DEPTH_STENCIL, Depth) ||
        !Tex(DXGI_FORMAT_R8G8B8A8_TYPELESS, 1, D3D11_BIND_SHADER_RESOURCE, Output))
        return false;
    D3D11_RENDER_TARGET_VIEW_DESC Rd{};
    Rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    Rd.ViewDimension = Samples > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC Sd{};
    Sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;   // the sRGB-encoded bytes as they are: what a UNORM back buffer shows
    Sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    Sd.Texture2D.MipLevels = 1;
    return SUCCEEDED(Dev->CreateRenderTargetView(Color.Get(), &Rd, &RTV)) &&
           SUCCEEDED(Dev->CreateDepthStencilView(Depth.Get(), nullptr, &DSV)) &&
           SUCCEEDED(Dev->CreateShaderResourceView(Output.Get(), &Sd, &OutputSRV));
}

bool CaptureTarget(ID3D11Device* Dev, ID3D11DeviceContext* Ctx, const Target& T, ScratchImage& Out)
{
    if (!T.Output) return false;
    D3D11_TEXTURE2D_DESC Td{};
    T.Output->GetDesc(&Td);
    Td.Usage = D3D11_USAGE_STAGING; Td.BindFlags = 0; Td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D11Texture2D> Staging;
    if (FAILED(Dev->CreateTexture2D(&Td, nullptr, &Staging))) return false;
    Ctx->CopyResource(Staging.Get(), T.Output.Get());
    D3D11_MAPPED_SUBRESOURCE M{};
    if (FAILED(Ctx->Map(Staging.Get(), 0, D3D11_MAP_READ, 0, &M))) return false;
    bool Ok = SUCCEEDED(Out.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, T.W, T.H, 1, 1));
    if (Ok)
    {
        const Image* I = Out.GetImage(0, 0, 0);
        for (int Y = 0; Y < T.H; Y++)
            std::memcpy(I->pixels + (size_t)Y * I->rowPitch, (const uint8_t*)M.pData + (size_t)Y * M.RowPitch, (size_t)T.W * 4);
    }
    Ctx->Unmap(Staging.Get(), 0);
    return Ok;
}

// ---- Renderer

struct Renderer::Impl
{
    ComPtr<ID3D11Device> Dev;
    ComPtr<ID3D11VertexShader> VS;
    ComPtr<ID3D11PixelShader> PS, Flat;
    ComPtr<ID3D11InputLayout> Layout;
    ComPtr<ID3D11Buffer> CB, VB;
    UINT VBCapacity = 0;
    ComPtr<ID3D11BlendState> Blends[4];       // by Blend
    ComPtr<ID3D11DepthStencilState> DepthWrite, DepthTest;
    ComPtr<ID3D11RasterizerState> NoCull;
    ComPtr<ID3D11SamplerState> Sampler, WrapSampler;
    ComPtr<ID3D11ShaderResourceView> Dot;   // the soft dot: no image found, lights
    std::shared_ptr<const TextureIndex> Textures;
    // the images and models read, by file: Prepare fills them from a loader thread too
    std::mutex LoadLock;
    std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> ByPath;
    std::unordered_map<std::string, std::shared_ptr<GpuModel>> ModelsByPath;
    int TextureMax = 0;   // images start at their first mip within this (0: whole)
    std::unordered_map<const Material*, ID3D11ShaderResourceView*> ByMaterial;
    Geometry::Batch Geo;   // the frame being drawn
    size_t Missing = 0;
    bool Ok = false;
    // model elements: their meshes by model name (a stand-in where the model is not exported)
    ComPtr<ID3D11VertexShader> ModelVS;
    ComPtr<ID3D11PixelShader> ModelPS;
    ComPtr<ID3D11InputLayout> ModelLayout;
    ComPtr<ID3D11Buffer> ModelCB;
    ComPtr<ID3D11ShaderResourceView> White;
    std::unordered_map<std::string, std::shared_ptr<GpuModel>> Models;
    std::shared_ptr<GpuModel> StandIn;

    std::shared_ptr<GpuModel> Upload(const std::vector<float>& Vtx, const std::vector<uint32_t>& Idx,
                                     std::vector<GpuModel::Range> Ranges)
    {
        auto M = std::make_shared<GpuModel>();
        if (Vtx.empty() || Idx.empty()) return M;
        D3D11_BUFFER_DESC Bd{};
        Bd.Usage = D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA Sd{};
        Bd.ByteWidth = (UINT)(Vtx.size() * sizeof(float)); Bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; Sd.pSysMem = Vtx.data();
        if (FAILED(Dev->CreateBuffer(&Bd, &Sd, &M->VB))) return M;
        Bd.ByteWidth = (UINT)(Idx.size() * sizeof(uint32_t)); Bd.BindFlags = D3D11_BIND_INDEX_BUFFER; Sd.pSysMem = Idx.data();
        if (FAILED(Dev->CreateBuffer(&Bd, &Sd, &M->IB))) return M;
        M->Ranges = std::move(Ranges);
        M->Ok = true;
        return M;
    }

    // a model element's mesh, read once: the export's model of that name, else a small stand-in
    std::shared_ptr<GpuModel> ModelOf(const std::string& Name)
    {
        if (const auto It = Models.find(Name); It != Models.end()) return It->second;
        const std::string Path = Textures ? Textures->FindModel(Name) : std::string();
        std::shared_ptr<GpuModel> M = Path.empty() ? nullptr : ModelFile(Path);
        if (!M || !M->Ok)
        {
            Missing++;
            M = StandIn;
        }
        Models[Name] = M;
        return M;
    }

    // a model file's mesh, read once (any thread)
    std::shared_ptr<GpuModel> ModelFile(const std::string& Path)
    {
        {
            std::lock_guard<std::mutex> Guard(LoadLock);
            if (const auto It = ModelsByPath.find(Path); It != ModelsByPath.end()) return It->second;
        }
        std::shared_ptr<GpuModel> M;
        PreviewGeometry Geo;
        if (LoadPreviewGeometry({ Path }, Geo) && !Geo.Idx.empty())
        {
            // the triangles by the material of their first vertex
            const size_t Mats = std::max(Geo.MaterialMaps.size(), Geo.Materials.size());
            std::vector<std::vector<uint32_t>> ByMat(Mats + 1);
            for (size_t T = 0; T + 2 < Geo.Idx.size(); T += 3)
            {
                const int Mat = Geo.Idx[T] < Geo.VtxMat.size() ? Geo.VtxMat[Geo.Idx[T]] : -1;
                auto& L = ByMat[Mat >= 0 && (size_t)Mat < Mats ? Mat : Mats];
                L.insert(L.end(), Geo.Idx.begin() + T, Geo.Idx.begin() + T + 3);
            }
            std::vector<uint32_t> Idx;
            std::vector<GpuModel::Range> Ranges;
            for (size_t K = 0; K < ByMat.size(); K++)
            {
                if (ByMat[K].empty()) continue;
                GpuModel::Range R{ White, (UINT)Idx.size(), (UINT)ByMat[K].size() };
                if (K < Geo.MaterialMaps.size() && !Geo.MaterialMaps[K].Albedo.Empty())
                    R.Srv = CreatePreviewSRV(Dev.Get(), Geo.MaterialMaps[K].Albedo, true);
                else if (K < Geo.Materials.size() && !Geo.Materials[K].Rgba.empty())
                    R.Srv = CreatePreviewSRV(Dev.Get(), PreviewImageFromRGBA(Geo.Materials[K].Width, Geo.Materials[K].Height, Geo.Materials[K].Rgba), true);
                if (!R.Srv) R.Srv = White;
                Idx.insert(Idx.end(), ByMat[K].begin(), ByMat[K].end());
                Ranges.push_back(R);
            }
            M = Upload(InterleavePreviewVertices(Geo.Pos, Geo.Nrm, Geo.UV), Idx, std::move(Ranges));
        }
        std::lock_guard<std::mutex> Guard(LoadLock);
        return ModelsByPath.emplace(Path, M).first->second;
    }

    // an image file's view, read once (any thread): with a mask, its alpha from the mask (WithAlphaMask,
    // Black Ops 4); with an opacity or ramp image, those applied (WithMaps, Black Ops 6/7)
    ComPtr<ID3D11ShaderResourceView> ImageFile(const std::string& Path, const std::string& Mask = std::string(),
                                               const std::string& Opacity = std::string(), bool Invert = false,
                                               const std::string& Ramp = std::string())
    {
        const std::string Key = Mask.empty() && Opacity.empty() && Ramp.empty()
            ? Path : Path + "|" + Mask + "|" + Opacity + (Invert ? "|1|" : "|0|") + Ramp;
        {
            std::lock_guard<std::mutex> Guard(LoadLock);
            if (const auto It = ByPath.find(Key); It != ByPath.end()) return It->second;
        }
        ComPtr<ID3D11ShaderResourceView> Srv;
        PreviewImage Img;
        if (PreparePreviewImage(Path, PreviewTexUse::Color, -1.0f, Img, nullptr, TextureMax))
        {
            if (!Mask.empty()) WithAlphaMask(Img, Path, Mask);
            if (!Opacity.empty() || !Ramp.empty()) Img = WithMaps(Img, Opacity, Invert, Ramp);
            Srv = CreatePreviewSRV(Dev.Get(), Img, true, TextureMax);
        }
        std::lock_guard<std::mutex> Guard(LoadLock);
        return ByPath.emplace(Key, Srv).first->second;
    }

    // a material's colour image as drawn: its mask, opacity and ramp images found beside it
    ComPtr<ID3D11ShaderResourceView> MaterialImage(const std::string& Path, const Material& Mat, const TextureIndex& Index)
    {
        const auto Find = [&](const std::string& Name) { return Name.empty() ? std::string() : Index.FindImage(Name); };
        if (!Mat.Frames.empty())
        {
            std::vector<std::string> Paths;
            for (const auto& F : Mat.Frames) Paths.push_back(Find(F));
            if (auto Srv = FramesImage(Paths)) return Srv;
        }
        return ImageFile(Path, Find(Mat.AlphaMask), Find(Mat.Opacity), Mat.OpacityInverted, Find(Mat.Ramp));
    }

    // an animated material's frames side by side in one image (an atlas of one row), each at the
    // first's size, read once; null when a frame is missing or does not decode
    ComPtr<ID3D11ShaderResourceView> FramesImage(const std::vector<std::string>& Paths)
    {
        std::string Key = "frames";
        for (const auto& P : Paths) Key += "|" + P;
        {
            std::lock_guard<std::mutex> Guard(LoadLock);
            if (const auto It = ByPath.find(Key); It != ByPath.end()) return It->second;
        }
        ComPtr<ID3D11ShaderResourceView> Srv;
        int W = 0, H = 0;
        std::vector<uint8_t> Strip;
        bool Ok = !Paths.empty();
        for (size_t K = 0; K < Paths.size() && Ok; K++)
        {
            PreviewImage Img;
            int Fw = 0, Fh = 0;
            std::vector<uint8_t> Px;
            Ok = !Paths[K].empty() && PreparePreviewImage(Paths[K], PreviewTexUse::Color, -1.0f, Img, nullptr, TextureMax) &&
                 PreviewImageToRGBA8(Img, Fw, Fh, Px) && Fw > 0 && Fh > 0;
            if (!Ok) break;
            if (K == 0)
            {
                // the strip within 8192 texels: frames shrunk when they are many and large
                const int Step = std::max(1, (int)((Fw * Paths.size() + 8191) / 8192));
                W = std::max(1, Fw / Step), H = std::max(1, Fh / Step);
                Strip.assign((size_t)W * Paths.size() * H * 4, 0);
            }
            // nearest texel of the frame for each of its cell's
            for (int Y = 0; Y < H; Y++)
                for (int X = 0; X < W; X++)
                {
                    const uint8_t* S = &Px[(((size_t)Y * Fh / H) * Fw + (size_t)X * Fw / W) * 4];
                    std::copy(S, S + 4, &Strip[(((size_t)Y * Paths.size() * W) + K * W + X) * 4]);
                }
        }
        if (Ok) Srv = CreatePreviewSRV(Dev.Get(), PreviewImageFromRGBA((int)(W * Paths.size()), H, Strip), true, TextureMax);
        std::lock_guard<std::mutex> Guard(LoadLock);
        return ByPath.emplace(Key, Srv).first->second;
    }

    explicit Impl(ID3D11Device* D) : Dev(D)
    {
        ComPtr<ID3DBlob> Vs, Ps, Fs, Err;
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "FxPreview", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &Vs, &Err)) ||
            FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "FxPreview", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &Ps, &Err)) ||
            FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "FxPreview", nullptr, nullptr, "PSFlat", "ps_5_0", 0, 0, &Fs, &Err)))
            return;
        if (FAILED(Dev->CreateVertexShader(Vs->GetBufferPointer(), Vs->GetBufferSize(), nullptr, &VS)) ||
            FAILED(Dev->CreatePixelShader(Ps->GetBufferPointer(), Ps->GetBufferSize(), nullptr, &PS)) ||
            FAILED(Dev->CreatePixelShader(Fs->GetBufferPointer(), Fs->GetBufferSize(), nullptr, &Flat)))
            return;
        const D3D11_INPUT_ELEMENT_DESC In[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(Dev->CreateInputLayout(In, 3, Vs->GetBufferPointer(), Vs->GetBufferSize(), &Layout))) return;
        D3D11_BUFFER_DESC Bd{};
        Bd.ByteWidth = sizeof(Constants); Bd.Usage = D3D11_USAGE_DYNAMIC; Bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(Dev->CreateBuffer(&Bd, nullptr, &CB))) return;

        // Alpha, Additive, Premultiplied, Opaque
        const D3D11_BLEND Src[4] = { D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_ONE };
        const D3D11_BLEND Dst[4] = { D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ZERO };
        for (int K = 0; K < 4; K++)
        {
            D3D11_BLEND_DESC B{};
            auto& T = B.RenderTarget[0];
            T.BlendEnable = K != 3;
            T.SrcBlend = Src[K]; T.DestBlend = Dst[K]; T.BlendOp = D3D11_BLEND_OP_ADD;
            // the alpha kept as coverage (a transparent background composites)
            T.SrcBlendAlpha = D3D11_BLEND_ONE; T.DestBlendAlpha = K == 1 ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
            T.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            T.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(Dev->CreateBlendState(&B, &Blends[K]))) return;
        }
        D3D11_DEPTH_STENCIL_DESC Ds{};
        Ds.DepthEnable = TRUE; Ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; Ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(Dev->CreateDepthStencilState(&Ds, &DepthWrite))) return;
        Ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(Dev->CreateDepthStencilState(&Ds, &DepthTest))) return;
        D3D11_RASTERIZER_DESC Rs{};
        Rs.FillMode = D3D11_FILL_SOLID; Rs.CullMode = D3D11_CULL_NONE; Rs.DepthClipEnable = TRUE; Rs.MultisampleEnable = TRUE;
        Rs.AntialiasedLineEnable = TRUE;
        if (FAILED(Dev->CreateRasterizerState(&Rs, &NoCull))) return;
        D3D11_SAMPLER_DESC Sm{};
        Sm.Filter = D3D11_FILTER_ANISOTROPIC; Sm.MaxAnisotropy = 4;
        Sm.AddressU = Sm.AddressV = Sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        Sm.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(Dev->CreateSamplerState(&Sm, &Sampler))) return;
        Sm.AddressU = Sm.AddressV = Sm.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        if (FAILED(Dev->CreateSamplerState(&Sm, &WrapSampler))) return;

        // the soft dot: white, its alpha falling off from the centre
        const int N = 64;
        std::vector<uint8_t> Px((size_t)N * N * 4);
        for (int Y = 0; Y < N; Y++)
            for (int X = 0; X < N; X++)
            {
                const float Dx = (X + 0.5f) / N * 2 - 1, Dy = (Y + 0.5f) / N * 2 - 1;
                const float D = std::sqrt(Dx * Dx + Dy * Dy), A = std::clamp(1 - D, 0.0f, 1.0f);
                uint8_t* P = &Px[((size_t)Y * N + X) * 4];
                P[0] = P[1] = P[2] = 255;
                P[3] = (uint8_t)(A * A * 255);
            }
        Dot = CreatePreviewSRV(Dev.Get(), PreviewImageFromRGBA(N, N, Px), true);
        White = CreatePreviewSRV(Dev.Get(), PreviewImageFromRGBA(4, 4, std::vector<uint8_t>(64, 255)), true);

        // the model elements' pipeline
        ComPtr<ID3DBlob> Mv, Mp;
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "FxPreview", nullptr, nullptr, "VSModel", "vs_5_0", 0, 0, &Mv, &Err)) ||
            FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "FxPreview", nullptr, nullptr, "PSModel", "ps_5_0", 0, 0, &Mp, &Err)) ||
            FAILED(Dev->CreateVertexShader(Mv->GetBufferPointer(), Mv->GetBufferSize(), nullptr, &ModelVS)) ||
            FAILED(Dev->CreatePixelShader(Mp->GetBufferPointer(), Mp->GetBufferSize(), nullptr, &ModelPS)))
            return;
        const D3D11_INPUT_ELEMENT_DESC MIn[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(Dev->CreateInputLayout(MIn, 3, Mv->GetBufferPointer(), Mv->GetBufferSize(), &ModelLayout))) return;
        Bd.ByteWidth = sizeof(ModelConstants);
        if (FAILED(Dev->CreateBuffer(&Bd, nullptr, &ModelCB))) return;
        // the stand-in: an octahedron of radius 1.5, a chip of debris
        {
            const float R = 1.5f;
            const float Pts[6][3] = { { R, 0, 0 }, { -R, 0, 0 }, { 0, R, 0 }, { 0, -R, 0 }, { 0, 0, R }, { 0, 0, -R } };
            const int Faces[8][3] = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 }, { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 } };
            std::vector<float> Vtx;
            std::vector<uint32_t> Idx;
            for (const auto& F : Faces)
            {
                const XMVECTOR A = XMLoadFloat3((const XMFLOAT3*)Pts[F[0]]), B = XMLoadFloat3((const XMFLOAT3*)Pts[F[1]]),
                               C = XMLoadFloat3((const XMFLOAT3*)Pts[F[2]]);
                XMFLOAT3 Nm;
                XMStoreFloat3(&Nm, XMVector3Normalize(XMVector3Cross(XMVectorSubtract(B, A), XMVectorSubtract(C, A))));
                for (int K = 0; K < 3; K++)
                {
                    Idx.push_back((uint32_t)(Vtx.size() / 8));
                    Vtx.insert(Vtx.end(), { Pts[F[K]][0], Pts[F[K]][1], Pts[F[K]][2], Nm.x, Nm.y, Nm.z, 0.5f, 0.5f });
                }
            }
            StandIn = Upload(Vtx, Idx, { GpuModel::Range{ White, 0, (UINT)Idx.size() } });
        }
        Ok = Dot != nullptr && White != nullptr && StandIn && StandIn->Ok;
    }

    // the colour image's alpha from the mask's red (Material::AlphaMask), the mask read at the
    // colour's size; an HDR colour map (BC6H, float) stays float. Unchanged when either does not decode.
    static void WithAlphaMask(PreviewImage& Img, const std::string& ColorPath, const std::string& MaskPath)
    {
        PreviewImage MaskImg;
        int Mw = 0, Mh = 0;
        std::vector<uint8_t> Mask;
        if (!PreparePreviewImage(MaskPath, PreviewTexUse::Data, -1.0f, MaskImg) || !PreviewImageToRGBA8(MaskImg, Mw, Mh, Mask) || Mw <= 0 || Mh <= 0)
            return;
        const auto MaskAt = [&](size_t X, size_t Y, size_t W, size_t H) { return Mask[((Y * Mh / H) * Mw + X * Mw / W) * 4]; };
        // the colour file's own pixels: the prepared image of an HDR one is already 8-bit
        ScratchImage Src;
        TexMetadata M{};
        if (ExportNaming::Lower(fs::path(ColorPath).extension().string()) == ".dds" &&
            SUCCEEDED(GetMetadataFromDDSFile(Strings::ToUnicodeString(ColorPath).c_str(), DDS_FLAGS_NONE, M)) &&
            (M.format == DXGI_FORMAT_BC6H_UF16 || M.format == DXGI_FORMAT_BC6H_SF16 || M.format == DXGI_FORMAT_R16G16B16A16_FLOAT) &&
            SUCCEEDED(LoadFromDDSFile(Strings::ToUnicodeString(ColorPath).c_str(), DDS_FLAGS_NONE, &M, Src)))
        {
            ScratchImage Top, Float;
            if (FAILED(Top.InitializeFromImage(*Src.GetImage(0, 0, 0)))) return;
            const HRESULT Hr = IsCompressed(M.format)
                ? Decompress(*Top.GetImage(0, 0, 0), DXGI_FORMAT_R32G32B32A32_FLOAT, Float)
                : Convert(*Top.GetImage(0, 0, 0), DXGI_FORMAT_R32G32B32A32_FLOAT, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, Float);
            if (FAILED(Hr)) return;
            // at most 1024 texels a side: a preview's particle never needs more
            if (const Image* Big = Float.GetImage(0, 0, 0); std::max(Big->width, Big->height) > 1024)
            {
                const size_t K = (std::max(Big->width, Big->height) + 1023) / 1024;
                ScratchImage Small;
                if (FAILED(Resize(*Big, std::max<size_t>(1, Big->width / K), std::max<size_t>(1, Big->height / K), TEX_FILTER_TRIANGLE | TEX_FILTER_FORCE_NON_WIC, Small))) return;
                Float = std::move(Small);
            }
            // the alpha from the mask; the colour scaled so that the brightest 10% of what the mask
            // shows is 1 (Black Ops 4's fire is stored in the tens of thousands)
            const Image* F = Float.GetImage(0, 0, 0);
            std::vector<float> Peaks;
            for (size_t Y = 0; Y < F->height; Y++)
                for (size_t X = 0; X < F->width; X++)
                {
                    float* P = reinterpret_cast<float*>(F->pixels + Y * F->rowPitch) + X * 4;
                    P[3] = MaskAt(X, Y, F->width, F->height) / 255.0f;
                    if (P[3] > 0.1f) Peaks.push_back(std::max(P[0], std::max(P[1], P[2])));
                }
            if (!Peaks.empty())
            {
                auto Nth = Peaks.begin() + (ptrdiff_t)(Peaks.size() * 9 / 10);
                std::nth_element(Peaks.begin(), Nth, Peaks.end());
                if (const float Scale = *Nth > 1.0f ? 1.0f / *Nth : 1.0f; Scale != 1.0f)
                    for (size_t Y = 0; Y < F->height; Y++)
                        for (size_t X = 0; X < F->width; X++)
                        {
                            float* P = reinterpret_cast<float*>(F->pixels + Y * F->rowPitch) + X * 4;
                            P[0] *= Scale, P[1] *= Scale, P[2] *= Scale;
                        }
            }
            ScratchImage Half, Mips;
            if (FAILED(Convert(*F, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, Half)) ||
                FAILED(GenerateMipMaps(*Half.GetImage(0, 0, 0), TEX_FILTER_BOX | TEX_FILTER_FORCE_NON_WIC, 0, Mips)))
                return;
            Img = PreviewImage{};
            Img.Width = (int)F->width, Img.Height = (int)F->height;
            Img.Image = std::make_shared<ScratchImage>(std::move(Mips));
            return;
        }
        int W = 0, H = 0;
        std::vector<uint8_t> Rgba;
        if (!PreviewImageToRGBA8(Img, W, H, Rgba) || W <= 0 || H <= 0) return;
        for (int Y = 0; Y < H; Y++)
            for (int X = 0; X < W; X++) Rgba[((size_t)Y * W + X) * 4 + 3] = MaskAt(X, Y, W, H);
        Img = PreviewImageFromRGBA(W, H, Rgba);
    }

    // Img with its alpha from another image (Invert: 1 - its red) and its grey (red) coloured through a
    // ramp's middle row, the grey kept as the alpha (Black Ops 6/7's materials); Img as it was when one
    // does not read
    static PreviewImage WithMaps(const PreviewImage& Img, const std::string& Opacity, bool Invert, const std::string& Ramp)
    {
        int W = 0, H = 0;
        std::vector<uint8_t> Px;
        if (!PreviewImageToRGBA8(Img, W, H, Px) || W <= 0 || H <= 0) return Img;
        const auto Load = [](const std::string& P, int& Wo, int& Ho, std::vector<uint8_t>& Out) {
            PreviewImage I;
            return !P.empty() && PreparePreviewImage(P, PreviewTexUse::Data, -1.0f, I) && PreviewImageToRGBA8(I, Wo, Ho, Out) && Wo > 0 && Ho > 0;
        };
        int Wr = 0, Hr = 0, Wa = 0, Ha = 0;
        std::vector<uint8_t> R, A;
        const bool HasRamp = Load(Ramp, Wr, Hr, R), HasAlpha = Load(Opacity, Wa, Ha, A);
        for (int Y = 0; Y < H; Y++)
            for (int X = 0; X < W; X++)
            {
                uint8_t* P = &Px[((size_t)Y * W + X) * 4];
                if (HasRamp)
                {
                    const uint8_t L = P[0];
                    const uint8_t* C = &R[((size_t)(Hr / 2) * Wr + (size_t)L * (Wr - 1) / 255) * 4];
                    P[0] = C[0], P[1] = C[1], P[2] = C[2], P[3] = L;
                }
                if (HasAlpha)
                {
                    const uint8_t V = A[((size_t)((int64_t)Y * Ha / H) * Wa + (size_t)((int64_t)X * Wa / W)) * 4];
                    P[3] = Invert ? (uint8_t)(255 - V) : V;
                }
            }
        return PreviewImageFromRGBA(W, H, Px);
    }

    ID3D11ShaderResourceView* TextureOf(const Material* M)
    {
        if (!M) return Dot.Get();
        if (const auto It = ByMaterial.find(M); It != ByMaterial.end()) return It->second ? It->second : Dot.Get();
        ID3D11ShaderResourceView* Srv = nullptr;
        const std::string Path = Textures ? Textures->Find(*M) : std::string();
        if (!Path.empty() && Textures) Srv = MaterialImage(Path, *M, *Textures).Get();   // kept in ByPath
        if (!Srv) Missing++;
        ByMaterial[M] = Srv;
        return Srv ? Srv : Dot.Get();
    }

    // the images the geometry draws with: a material's by TextureOf
    Geometry::Images GeometryImages()
    {
        return { Dot.Get(), White.Get(), [this](const Material* M) { return TextureOf(M); } };
    }

    // the grid's lines, then the items' vertices in drawing order (each item's First moved to
    // where it lands) into VB, grown when they do not fit; false when it cannot grow
    bool UploadFrame(ID3D11DeviceContext* Ctx, const std::vector<Vertex>& Lines)
    {
        const size_t Total = Geo.Verts.size() + Lines.size();
        if (Total > VBCapacity)
        {
            VB.Reset();
            D3D11_BUFFER_DESC Bd{};
            VBCapacity = (UINT)std::max<size_t>(Total + Total / 2, 4096);
            Bd.ByteWidth = VBCapacity * sizeof(Vertex);
            Bd.Usage = D3D11_USAGE_DYNAMIC; Bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; Bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(Dev->CreateBuffer(&Bd, nullptr, &VB))) { VBCapacity = 0; return false; }
        }
        D3D11_MAPPED_SUBRESOURCE Map{};
        if (Total > 0 && SUCCEEDED(Ctx->Map(VB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Map)))
        {
            Vertex* Dst = (Vertex*)Map.pData;
            if (!Lines.empty()) std::memcpy(Dst, Lines.data(), Lines.size() * sizeof(Vertex));
            // the quads in drawing order
            size_t At = Lines.size();
            for (Geometry::Item& It : Geo.Items)
            {
                std::memcpy(Dst + At, Geo.Verts.data() + It.First, It.Count * sizeof(Vertex));
                It.First = (uint32_t)At;
                At += It.Count;
            }
            Ctx->Unmap(VB.Get(), 0);
        }
        return true;
    }

    void SetConstants(ID3D11DeviceContext* Ctx, const XMMATRIX& ViewProj, float Premultiply, float Bright)
    {
        D3D11_MAPPED_SUBRESOURCE M{};
        if (FAILED(Ctx->Map(CB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &M))) return;
        Constants* C = (Constants*)M.pData;
        XMStoreFloat4x4(&C->ViewProj, XMMatrixTranspose(ViewProj));
        C->Params[0] = Premultiply;
        C->Params[1] = Bright;
        C->Params[2] = C->Params[3] = 0;
        Ctx->Unmap(CB.Get(), 0);
    }

    // T cleared to the background and bound, the particles' pipeline set
    void BeginPass(ID3D11DeviceContext* Ctx, Target& T, const DrawOptions& O, const XMMATRIX& ViewProj)
    {
        float Bg[4] = { ToLinear(O.Background[0]), ToLinear(O.Background[1]), ToLinear(O.Background[2]), O.Background[3] };
        Ctx->ClearRenderTargetView(T.RTV.Get(), Bg);
        Ctx->ClearDepthStencilView(T.DSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        ID3D11RenderTargetView* Rtv = T.RTV.Get();
        Ctx->OMSetRenderTargets(1, &Rtv, T.DSV.Get());
        D3D11_VIEWPORT Vp{ 0, 0, (float)T.W, (float)T.H, 0, 1 };
        Ctx->RSSetViewports(1, &Vp);
        Ctx->RSSetState(NoCull.Get());
        UseParticlePipeline(Ctx);
        ID3D11Buffer* Cb = CB.Get();
        Ctx->VSSetConstantBuffers(0, 1, &Cb);
        Ctx->PSSetConstantBuffers(0, 1, &Cb);
        ID3D11SamplerState* Smp = Sampler.Get();
        Ctx->PSSetSamplers(0, 1, &Smp);
        SetConstants(Ctx, ViewProj, 0, 1);
    }

    // the particles' input layout, vertex buffer and vertex shader
    void UseParticlePipeline(ID3D11DeviceContext* Ctx)
    {
        Ctx->IASetInputLayout(Layout.Get());
        const UINT Stride = sizeof(Vertex), Offset = 0;
        ID3D11Buffer* Vb = VB.Get();
        Ctx->IASetVertexBuffers(0, 1, &Vb, &Stride, &Offset);
        Ctx->VSSetShader(VS.Get(), nullptr, 0);
    }

    // the grid's lines, flat, depth tested
    void DrawGrid(ID3D11DeviceContext* Ctx, UINT Count)
    {
        Ctx->OMSetBlendState(Blends[(int)Blend::Alpha].Get(), nullptr, 0xffffffff);
        Ctx->OMSetDepthStencilState(DepthTest.Get(), 0);
        Ctx->PSSetShader(Flat.Get(), nullptr, 0);
        Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        Ctx->Draw(Count, 0);
    }

    // model elements: opaque, depth written, under the particles; the particles' pipeline set back after
    void DrawModels(ID3D11DeviceContext* Ctx, const std::vector<const Particle*>& Pts)
    {
        Ctx->OMSetBlendState(Blends[(int)Blend::Opaque].Get(), nullptr, 0xffffffff);
        Ctx->OMSetDepthStencilState(DepthWrite.Get(), 0);
        Ctx->IASetInputLayout(ModelLayout.Get());
        Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Ctx->VSSetShader(ModelVS.Get(), nullptr, 0);
        Ctx->PSSetShader(ModelPS.Get(), nullptr, 0);
        ID3D11Buffer* Cbs[2] = { CB.Get(), ModelCB.Get() };
        Ctx->VSSetConstantBuffers(0, 2, Cbs);
        Ctx->PSSetConstantBuffers(0, 2, Cbs);
        for (const Particle* Pt : Pts)
        {
            const std::shared_ptr<GpuModel> M = ModelOf(*Pt->Model);
            if (!M || !M->Ok) continue;
            // the element's axes and position, its visual scale (0: as modelled)
            const float S = Pt->Scale > 0 ? Pt->Scale : 1.0f;
            const XMMATRIX World(Pt->Axis[0][0] * S, Pt->Axis[0][1] * S, Pt->Axis[0][2] * S, 0,
                                 Pt->Axis[1][0] * S, Pt->Axis[1][1] * S, Pt->Axis[1][2] * S, 0,
                                 Pt->Axis[2][0] * S, Pt->Axis[2][1] * S, Pt->Axis[2][2] * S, 0,
                                 Pt->Pos[0], Pt->Pos[1], Pt->Pos[2], 1);
            D3D11_MAPPED_SUBRESOURCE Mc{};
            if (FAILED(Ctx->Map(ModelCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Mc))) continue;
            ModelConstants* C = (ModelConstants*)Mc.pData;
            XMStoreFloat4x4(&C->World, XMMatrixTranspose(World));
            const bool Stand = M == StandIn;
            C->Tint[0] = Stand ? 0.5f : 1.0f, C->Tint[1] = Stand ? 0.45f : 1.0f, C->Tint[2] = Stand ? 0.4f : 1.0f, C->Tint[3] = 1;
            Ctx->Unmap(ModelCB.Get(), 0);
            const UINT MStride = 8 * sizeof(float), MOffset = 0;
            ID3D11Buffer* Mvb = M->VB.Get();
            Ctx->IASetVertexBuffers(0, 1, &Mvb, &MStride, &MOffset);
            Ctx->IASetIndexBuffer(M->IB.Get(), DXGI_FORMAT_R32_UINT, 0);
            for (const auto& R : M->Ranges)
            {
                ID3D11ShaderResourceView* Srv = R.Srv.Get();
                Ctx->PSSetShaderResources(0, 1, &Srv);
                Ctx->DrawIndexed(R.Count, R.Start, 0);
            }
        }
        // back to the particles' pipeline
        UseParticlePipeline(Ctx);
    }

    // the items, sorted: runs of the same texture, blending, brightness and wrapping in one draw
    void DrawItems(ID3D11DeviceContext* Ctx, const XMMATRIX& ViewProj)
    {
        Ctx->OMSetDepthStencilState(DepthTest.Get(), 0);
        Ctx->PSSetShader(PS.Get(), nullptr, 0);
        Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        float Premul = -1, Bright = -1;
        bool Wrapped = false;
        const auto& Items = Geo.Items;
        for (size_t K = 0; K < Items.size();)
        {
            const Geometry::Item& First = Items[K];
            size_t E = K + 1;
            UINT Count = First.Count;
            while (E < Items.size() && Items[E].Srv == First.Srv && Items[E].Mode == First.Mode && Items[E].Bright == First.Bright &&
                   Items[E].Wrap == First.Wrap)
                Count += Items[E++].Count;
            if (First.Wrap != Wrapped)
            {
                ID3D11SamplerState* Ss = First.Wrap ? WrapSampler.Get() : Sampler.Get();
                Ctx->PSSetSamplers(0, 1, &Ss);
                Wrapped = First.Wrap;
            }
            const float Want = First.Mode == (int)Blend::Premultiplied ? 1.0f : 0.0f;
            if (Want != Premul || First.Bright != Bright)
            {
                SetConstants(Ctx, ViewProj, Want, First.Bright);
                Premul = Want;
                Bright = First.Bright;
            }
            Ctx->OMSetBlendState(Blends[std::clamp(First.Mode, 0, 3)].Get(), nullptr, 0xffffffff);
            ID3D11ShaderResourceView* Srv = First.Srv;
            Ctx->PSSetShaderResources(0, 1, &Srv);
            Ctx->Draw(Count, First.First);
            K = E;
        }
    }

    // the target unbound, resolved into its Output
    static void EndPass(ID3D11DeviceContext* Ctx, Target& T)
    {
        ID3D11ShaderResourceView* None = nullptr;
        Ctx->PSSetShaderResources(0, 1, &None);
        Ctx->OMSetRenderTargets(0, nullptr, nullptr);
        Ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        Ctx->OMSetDepthStencilState(nullptr, 0);
        if (T.Samples > 1) Ctx->ResolveSubresource(T.Output.Get(), 0, T.Color.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        else Ctx->CopyResource(T.Output.Get(), T.Color.Get());
    }
};

Renderer::Renderer(ID3D11Device* Dev) : P(std::make_unique<Impl>(Dev)) {}
Renderer::~Renderer() = default;
bool Renderer::Ok() const { return P->Ok; }
size_t Renderer::QuadsDrawn() const { return P->Geo.Quads; }
size_t Renderer::TexturesMissing() const { return P->Missing; }

void Renderer::SetTextureLimit(int MaxSize) { P->TextureMax = MaxSize; }

void Renderer::Prepare(const Frame& F, const TextureIndex& Index)
{
    std::vector<const Material*> Mats;
    for (const Particle& Pt : F.Particles)
    {
        if (Pt.Mat) Mats.push_back(Pt.Mat);
        if (Pt.Model) if (const std::string Path = Index.FindModel(*Pt.Model); !Path.empty()) P->ModelFile(Path);
    }
    for (const TrailStrip& S : F.Trails) if (S.Mat) Mats.push_back(S.Mat);
    std::sort(Mats.begin(), Mats.end());
    Mats.erase(std::unique(Mats.begin(), Mats.end()), Mats.end());
    for (const Material* M : Mats)
        if (const std::string Path = Index.Find(*M); !Path.empty())
            P->MaterialImage(Path, *M, Index);
}

void Renderer::ForgetMaterials() { P->ByMaterial.clear(); }

void Renderer::SetTextures(std::shared_ptr<const TextureIndex> Index)
{
    if (P->Textures == Index) return;
    P->Textures = std::move(Index);
    P->ByMaterial.clear();
    P->Models.clear();
    P->Missing = 0;
}

void Renderer::Render(ID3D11DeviceContext* Ctx, const Frame& F, const PreviewCamera& Cam, Target& T, const DrawOptions& O)
{
    if (!P->Ok || !T.RTV) return;
    Impl& I = *P;
    I.Geo.Clear();

    const XMVECTOR Eye = XMLoadFloat3(&Cam.Eye), At = XMLoadFloat3(&Cam.Target), WorldUp = XMLoadFloat3(&Cam.Up);
    Geometry::View V;
    V.Eye = Eye;
    V.Forward = XMVector3Normalize(XMVectorSubtract(At, Eye));
    V.Right = XMVector3Normalize(XMVector3Cross(V.Forward, WorldUp));
    V.Up = XMVector3Cross(V.Right, V.Forward);
    V.Fov = Cam.Fov;
    const XMMATRIX View = XMMatrixLookAtRH(Eye, At, WorldUp);
    const XMMATRIX Proj = XMMatrixPerspectiveFovRH(Cam.Fov, (float)T.W / (float)std::max(1, T.H), Cam.Near, Cam.Far);
    const XMMATRIX ViewProj = XMMatrixMultiply(View, Proj);

    // ---- the geometry: the particles as quads (the models apart, drawn first), the trails, sorted
    std::vector<const Particle*> ModelPts;
    const Geometry::Images Img = I.GeometryImages();
    I.Geo.AddParticles(F, V, O, Img, ModelPts);
    I.Geo.AddTrails(F, V, Img);
    I.Geo.SortBackToFront();
    std::vector<Vertex> Lines;
    if (O.Grid) Lines = Geometry::GridLines(O, XMVectorGetX(XMVector3Length(XMVectorSubtract(At, Eye))));

    // ---- upload and draw: the grid, the models, the particles
    if (!I.UploadFrame(Ctx, Lines)) return;
    const size_t Total = I.Geo.Verts.size() + Lines.size();
    I.BeginPass(Ctx, T, O, ViewProj);
    if (!Lines.empty() && Total > 0) I.DrawGrid(Ctx, (UINT)Lines.size());
    if (!ModelPts.empty() && I.ModelVS) I.DrawModels(Ctx, ModelPts);
    if (!I.Geo.Items.empty() && Total > 0) I.DrawItems(Ctx, ViewProj);
    Impl::EndPass(Ctx, T);
}

// ---- Scene

bool Scene::Load(const std::string& JsonPath, const std::vector<std::string>& ExtraFolders, std::string& Error, const SimOptions& Options)
{
    Lib = std::make_shared<Library>();
    {
        // not an effect: nothing indexed
        Effect Probe;
        if (!LoadEffect(JsonPath, Probe, Error)) return false;
    }
    std::vector<std::string> Folders = GameFolders(JsonPath);
    Folders.insert(Folders.end(), ExtraFolders.begin(), ExtraFolders.end());
    // the effects of the other folders too (a common zone's, which a map's effects start)
    for (size_t K = 1; K < Folders.size(); K++) Lib->AddSearchDir(Folders[K] + "\\fx");
    // the images of these folders, indexed once: the effects of one export shown one after the other
    // share their index. Past a minute it is indexed again on a worker (an export may have added
    // images), the old one used meanwhile: a game's index takes seconds
    {
        struct Slot
        {
            std::chrono::steady_clock::time_point At;
            std::shared_ptr<const TextureIndex> Index;
            std::future<std::shared_ptr<const TextureIndex>> Next;   // the refresh, when one runs
        };
        static std::mutex Lock;
        static std::map<std::string, Slot> Indexes;
        std::string Key;
        for (const auto& F : Folders) Key += F + "|";
        const auto Build = [Folders, Key] {
            AppLog::Timer T("fx image index " + Key, spdlog::level::info, 1000);
            auto Index = std::make_shared<TextureIndex>();
            for (const auto& F : Folders) Index->AddFolder(F);
            return std::shared_ptr<const TextureIndex>(std::move(Index));
        };
        std::lock_guard<std::mutex> Guard(Lock);
        Slot& S = Indexes[Key];
        const auto Now = std::chrono::steady_clock::now();
        if (S.Next.valid() && S.Next.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            S.Index = S.Next.get();
            S.At = Now;
        }
        if (!S.Index)
        {
            S.Index = Build();
            S.At = Now;
        }
        else if (Now - S.At > std::chrono::seconds(60) && !S.Next.valid())
            S.Next = std::async(std::launch::async, Build);
        Textures = S.Index;
    }
    // the materials the effects only name, completed from the material kind's files
    Lib->SetMaterials(Textures);
    Fx = Lib->LoadRoot(JsonPath, Error);
    if (!Fx) return false;
    Sim = std::make_unique<Simulator>(Lib, Fx);
    Sim->SetOptions(Options);
    Frame();
    return true;
}

void Scene::Frame()
{
    if (!Sim) return;
    DurationMsec = Sim->DurationMsec();
    // the particles over a run, sampled: their middle and the 90th percentile of their reach
    std::vector<std::array<float, 4>> Pts;   // position, size
    FxPreview::Frame F;
    // early times weigh more: a burst then its long fade, the burst framed
    for (int K = 1; K <= 12; K++)
    {
        const double At = std::min(DurationMsec, 4000.0) * (K / 12.0) * (K / 12.0);
        Sim->Evaluate(At, F);
        for (const Particle& P : F.Particles)
        {
            // a light by its glow, a spot light by half its cone
            const float Reach = std::min(P.LightRadius > 0 ? P.LightRadius : std::max(P.Size[0], 8.0f), 400.0f);
            if (P.Type == ElemType::Light) Pts.push_back({ P.Pos[0], P.Pos[1], P.Pos[2], std::clamp(Reach * 0.1f, 2.0f, 24.0f) });
            else if (P.Type == ElemType::SpotLight)
                Pts.push_back({ P.Pos[0] + P.Axis[0][0] * Reach * 0.5f, P.Pos[1] + P.Axis[0][1] * Reach * 0.5f,
                                P.Pos[2] + P.Axis[0][2] * Reach * 0.5f, Reach * 0.5f });
            else if (P.Mat && P.Mat->LightSprite)
                Pts.push_back({ P.Pos[0], P.Pos[1], P.Pos[2], std::clamp(std::max(P.Size[0], P.Size[1]) * 0.15f, 2.0f, 24.0f) });
            else if (P.Type != ElemType::LensFlare && P.Type != ElemType::Decal && !(P.Mat && P.Mat->Distortion))
                Pts.push_back({ P.Pos[0], P.Pos[1], P.Pos[2], std::max(P.Size[0], P.Size[1]) });
        }
        for (const TrailStrip& S : F.Trails)
            for (const auto& Q : S.Points) Pts.push_back({ Q.Pos[0], Q.Pos[1], Q.Pos[2], Q.Width });
    }
    if (Pts.empty())
    {
        for (int K = 0; K < 3; K++) Center[K] = Fx->BoundsCenter[K];
        Radius = Fx->BoundsKnown ? std::max({ Fx->BoundsHalf[0], Fx->BoundsHalf[1], Fx->BoundsHalf[2] }) : 32.0f;
        return;
    }
    // the median on each axis: robust to a few far particles
    for (int A = 0; A < 3; A++)
    {
        std::vector<float> V;
        V.reserve(Pts.size());
        for (const auto& P : Pts) V.push_back(P[A]);
        std::nth_element(V.begin(), V.begin() + V.size() / 2, V.end());
        Center[A] = V[V.size() / 2];
    }
    // the reach that holds 90% of what is drawn, each particle weighed by its area: a few
    // sparks flying far do not push the camera away from the fireball
    std::vector<std::pair<float, float>> D;   // distance + size, weight
    D.reserve(Pts.size());
    double Total = 0;
    for (const auto& P : Pts)
    {
        const float Dx = P[0] - Center[0], Dy = P[1] - Center[1], Dz = P[2] - Center[2];
        const float W = std::max(1.0f, P[3] * P[3]);
        D.push_back({ std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz) + P[3], W });
        Total += W;
    }
    std::sort(D.begin(), D.end());
    double Sum = 0;
    Radius = D.back().first;
    for (const auto& [Dist, W] : D)
        if ((Sum += W) >= Total * 0.9)
        {
            Radius = Dist;
            break;
        }
    Radius = std::max(2.0f, Radius);
}

PreviewCamera OrbitCamera(const Scene& S, float Yaw, float Pitch, float DistScale, const float Pan[3])
{
    PreviewGpuMesh Frame;
    Frame.Center = XMVectorSet(S.Center[0], S.Center[1], S.Center[2], 0);
    Frame.Radius = S.Radius;
    Frame.FrameDist = S.Radius / std::tan(XMConvertToRadians(35.0f) * 0.5f) * 1.05f;
    PreviewCamera C = OrbitPreviewCamera(Frame, Yaw, Pitch, DistScale, Pan ? XMFLOAT3(Pan[0], Pan[1], Pan[2]) : XMFLOAT3());
    C.Near = std::max(0.05f, S.Radius * 0.01f);
    C.Far = C.Far + S.Radius * 20.0f;
    return C;
}

bool RenderAt(ID3D11DeviceContext* Ctx, Renderer& R, Scene& S, double Msec, Target& T, int W, int H, const DrawOptions& O)
{
    if (!S.Sim || !R.Ok()) return false;
    ComPtr<ID3D11Device> Dev;
    Ctx->GetDevice(&Dev);
    if (!T.Resize(Dev.Get(), W, H)) return false;
    R.SetTextures(S.Textures);
    FxPreview::Frame F;
    S.Sim->Evaluate(Msec, F);
    DrawOptions D = O;
    D.GridCenter[0] = S.Center[0];
    D.GridCenter[1] = S.Center[1];
    D.GridCenter[2] = S.Sim->GroundZ();
    R.Render(Ctx, F, OrbitCamera(S, 0.9f, 0.32f, 1.0f), T, D);
    return true;
}
}
