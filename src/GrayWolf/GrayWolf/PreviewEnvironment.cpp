#include "stdafx.h"

#include "PreviewEnvironment.h"
#include "ModelPreviewGPU.h"   // PreviewLighting
#include "PreviewD3D.h"
#include "CliOptions.h"
#include "CliSupport.h"        // CliReport
#include "GameCommand.h"
#include "Strings.h"

#include "DirectXTex.h"
#include <directxtk/Effects.h>
#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <DirectXMath.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace DirectX;
namespace fs = std::filesystem;

namespace
{
    constexpr float Pi = 3.14159265358979f;

    // The mean luminance of the procedural studio sky over the sphere (StudioSky, ModelPreviewGPU.cpp:
    // its upper half 0.780, its lower half 0.177): a baked environment is scaled to it, so the preview's
    // image based light keeps its strength from one environment to the next.
    constexpr float kStudioMeanLuminance = 0.4787f;

    float Luminance(const float* C) { return 0.2126f * C[0] + 0.7152f * C[1] + 0.0722f * C[2]; }

    fs::path EnvironmentDir()
    {
        wchar_t Exe[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, Exe, (DWORD)std::size(Exe));
        return fs::path(Exe).parent_path() / L"environments";
    }

    std::string DisplayName(std::string Id)
    {
        for (char& C : Id) if (C == '_') C = ' ';
        if (!Id.empty()) Id[0] = (char)std::toupper((unsigned char)Id[0]);
        return Id;
    }

    // The split-sum table (Karis 2013): for x = n.v, y = roughness, the scale and bias of F0 in the
    // GGX specular's integral over the hemisphere (Smith G, k = alpha / 2).
    float RadicalInverse(uint32_t B)
    {
        B = (B << 16u) | (B >> 16u);
        B = ((B & 0x55555555u) << 1u) | ((B & 0xAAAAAAAAu) >> 1u);
        B = ((B & 0x33333333u) << 2u) | ((B & 0xCCCCCCCCu) >> 2u);
        B = ((B & 0x0F0F0F0Fu) << 4u) | ((B & 0xF0F0F0F0u) >> 4u);
        B = ((B & 0x00FF00FFu) << 8u) | ((B & 0xFF00FF00u) >> 8u);
        return (float)B * 2.3283064365386963e-10f;
    }
    // A GGX half vector about +Z for the Hammersley point I of Count
    XMFLOAT3 GgxHalf(uint32_t I, uint32_t Count, float Alpha)
    {
        const float U1 = (I + 0.5f) / Count, U2 = RadicalInverse(I);
        const float Phi = 2 * Pi * U1;
        const float CosT = std::sqrt((1 - U2) / (1 + (Alpha * Alpha - 1) * U2)), SinT = std::sqrt(std::max(0.0f, 1 - CosT * CosT));
        return { SinT * std::cos(Phi), SinT * std::sin(Phi), CosT };
    }

    constexpr int kBrdfSize = 32;
    std::vector<XMFLOAT2> BrdfTable()
    {
        constexpr uint32_t Count = 512;
        std::vector<XMFLOAT2> T(kBrdfSize * kBrdfSize);
        for (int j = 0; j < kBrdfSize; j++)
            for (int i = 0; i < kBrdfSize; i++)
            {
                const float NdotV = (i + 0.5f) / kBrdfSize, Rough = (j + 0.5f) / kBrdfSize, Alpha = Rough * Rough, K = Alpha * 0.5f;
                const float V[3] = { std::sqrt(1 - NdotV * NdotV), 0, NdotV };
                const auto G1 = [K](float X) { return X / (X * (1 - K) + K); };
                float A = 0, B = 0;
                for (uint32_t s = 0; s < Count; s++)
                {
                    const XMFLOAT3 H = GgxHalf(s, Count, Alpha);
                    const float VdotH = V[0] * H.x + V[2] * H.z;
                    const float NdotL = 2 * VdotH * H.z - V[2];
                    if (NdotL <= 0) continue;
                    const float Vis = G1(NdotL) * G1(NdotV) * std::max(VdotH, 0.0f) / (std::max(H.z, 1e-4f) * NdotV);
                    const float Fc = std::pow(1 - std::max(VdotH, 0.0f), 5.0f);
                    A += (1 - Fc) * Vis;
                    B += Fc * Vis;
                }
                T[j * kBrdfSize + i] = { A / Count, B / Count };
            }
        return T;
    }

    const char* kHlsl = R"(
// PreviewEnvironment.cpp: the image based light
cbuffer Environment : register(b3)
{
    float2 EnvTurn;           // cos, sin of the environment's turn about Z
    float  EnvMipScale;       // the radiance cube's mips - 1: roughness 1 is its last
    float  EnvMultiScatter;   // 1: GGX's multiple scattering made up for
};
Texture2D<float2> EnvBrdfTable : register(t24);

float3 EnvDir(float3 d) { return float3(EnvTurn.x * d.x - EnvTurn.y * d.y, EnvTurn.y * d.x + EnvTurn.x * d.y, d.z); }
// the split sum's scale and bias of F0
float2 EnvBrdf(SamplerState s, float NdotV, float rough) { return EnvBrdfTable.SampleLevel(s, float2(NdotV, rough), 0); }
// what a light's single-scattering GGX lobe is multiplied by for the energy it loses (Kulla-Conty, as Filament)
float3 EnvEnergy(float3 f0, float2 ab) { return EnvMultiScatter > 0.5 ? 1 + f0 * (1 / max(ab.x + ab.y, 1e-3) - 1) : 1; }
// the environment's diffuse and specular light: split sum, Fdez-Aguera 2019's multiple scattering
float3 EnvImageLight(TextureCube<float3> rad, TextureCube<float3> irr, SamplerState s, float3 V, float3 N,
                     float3 cDiff, float3 cSpec, float rough, float2 ab, float scale, float ambient,
                     float diffuseOcclusion, float specularOcclusion)
{
    // the occlusions (the material's and the screen's, PreviewPost) dim the light the environment brings
    float3 diffuseLight = (irr.Sample(s, EnvDir(N)) * scale + ambient) * diffuseOcclusion;
    float3 specLight = rad.SampleLevel(s, EnvDir(reflect(-V, N)), rough * EnvMipScale) * scale * specularOcclusion;
    float3 FssEss = cSpec * ab.x + ab.y;
    if (EnvMultiScatter < 0.5) return FssEss * specLight + cDiff * diffuseLight;
    float Ess = ab.x + ab.y, Ems = 1 - Ess;
    float3 Favg = cSpec + (1 - cSpec) / 21;
    float3 FmsEms = Ems * FssEss * Favg / (1 - Ems * Favg);
    return FssEss * specLight + FmsEms * diffuseLight + cDiff * saturate(1 - FssEss - FmsEms) * diffuseLight;
}
)";

    struct EnvConstants { float Turn[2], MipScale, MultiScatter; };
}

const char* PreviewEnvironmentHlsl() { return kHlsl; }

const std::vector<PreviewEnvironmentInfo>& PreviewEnvironments()
{
    static const std::vector<PreviewEnvironmentInfo> List = [] {
        std::vector<PreviewEnvironmentInfo> L{ { kStudioEnvironment, "Studio sky" } };
        std::error_code Ec;
        std::vector<PreviewEnvironmentInfo> Found;
        const fs::path Dir = EnvironmentDir();
        for (auto It = fs::directory_iterator(Dir, Ec); !Ec && It != fs::directory_iterator(); It.increment(Ec))
        {
            const std::string File = It->path().filename().string();
            const std::string Suffix = "_radiance.dds";
            if (File.size() <= Suffix.size() || _stricmp(File.c_str() + File.size() - Suffix.size(), Suffix.c_str()) != 0) continue;
            const std::string Id = File.substr(0, File.size() - Suffix.size());
            if (Id == kStudioEnvironment || !fs::exists(Dir / (Id + "_irradiance.dds"), Ec)) continue;
            Found.push_back({ Id, DisplayName(Id) });
        }
        std::sort(Found.begin(), Found.end(), [](const auto& A, const auto& B) { return A.Name < B.Name; });
        L.insert(L.end(), Found.begin(), Found.end());
        return L;
    }();
    return List;
}

struct PreviewEnvironment::Impl
{
    ComPtr<ID3D11Device> Dev;
    ComPtr<ID3D11ShaderResourceView> StudioRad, StudioIrr, Rad, Irr, Brdf;
    ComPtr<ID3D11Buffer> CB;
    int StudioMips = 1, Mips = 1;
    std::string Current;   // the environment set on the effect ("": none yet)
    bool Ok = false;

    bool Load(const std::string& Id)
    {
        if (Id == kStudioEnvironment) return false;
        const fs::path Dir = EnvironmentDir();
        const auto Cube = [&](const char* Kind, ComPtr<ID3D11ShaderResourceView>& Out, int& Levels) {
            ScratchImage Img;
            TexMetadata Meta{};
            const fs::path File = Dir / (Id + Kind);
            if (FAILED(LoadFromDDSFile(File.c_str(), DDS_FLAGS_NONE, &Meta, Img)) || !Meta.IsCubemap()) return false;
            Levels = (int)Meta.mipLevels;
            return SUCCEEDED(CreateShaderResourceView(Dev.Get(), Img.GetImages(), Img.GetImageCount(), Meta, Out.ReleaseAndGetAddressOf()));
        };
        int IrrMips = 1;
        if (Cube("_radiance.dds", Rad, Mips) && Cube("_irradiance.dds", Irr, IrrMips)) return true;
        spdlog::warn("preview environment {}: cannot read its cubes in {}", Id, Dir.string());
        return false;
    }
};

PreviewEnvironment::PreviewEnvironment(ID3D11Device* Dev, ID3D11ShaderResourceView* StudioRadiance, int StudioMips,
                                       ID3D11ShaderResourceView* StudioIrradiance)
    : P(std::make_unique<Impl>())
{
    P->Dev = Dev;
    P->StudioRad = StudioRadiance;
    P->StudioIrr = StudioIrradiance;
    P->StudioMips = StudioMips;
    if (!Dev || !PreviewD3D::ConstantBuffer(Dev, sizeof(EnvConstants), P->CB)) return;

    // the BRDF table: 32 x 32 float pairs, a few milliseconds
    const std::vector<XMFLOAT2> Table = BrdfTable();
    if (!PreviewD3D::Immutable(Dev, kBrdfSize, kBrdfSize, DXGI_FORMAT_R32G32_FLOAT, Table.data(), (UINT)(kBrdfSize * sizeof(XMFLOAT2)), P->Brdf)) return;
    P->Ok = true;
}

PreviewEnvironment::~PreviewEnvironment() = default;
bool PreviewEnvironment::Ok() const { return P->Ok; }
ID3D11ShaderResourceView* PreviewEnvironment::Radiance() const { return P->Rad ? P->Rad.Get() : P->StudioRad.Get(); }
int PreviewEnvironment::RadianceMips() const { return P->Rad ? P->Mips : P->StudioMips; }

void PreviewEnvironment::Apply(ID3D11DeviceContext* Ctx, PBREffect& Effect, const PreviewLighting& L)
{
    if (!P->Ok) return;
    if (L.Environment != P->Current)
    {
        P->Current = L.Environment;
        if (!P->Load(L.Environment))
        {
            P->Rad = P->StudioRad;
            P->Irr = P->StudioIrr;
            P->Mips = P->StudioMips;
        }
        Effect.SetIBLTextures(P->Rad.Get(), P->Mips, P->Irr.Get());
    }
    const float Turn = XMConvertToRadians(L.EnvRotation);
    const EnvConstants C{ { std::cos(Turn), std::sin(Turn) }, (float)std::max(0, P->Mips - 1), 1.0f };
    Ctx->UpdateSubresource(P->CB.Get(), 0, nullptr, &C, 0, 0);
    ID3D11Buffer* Cbs[] = { P->CB.Get() };
    Ctx->PSSetConstantBuffers(3, 1, Cbs);   // b3: b4 is the shadows', b6 the occlusion's
    ID3D11ShaderResourceView* Srv[] = { P->Brdf.Get() };
    Ctx->PSSetShaderResources(24, 1, Srv);
}

// ---- baking an environment from an HDR panorama

namespace
{
    // The direction of a texel of cube face F (U, V in -1..1), D3D's face order: as FillCube
    // (ModelPreviewGPU.cpp), the preview samples its cubes with world directions (Z up)
    XMFLOAT3 CubeDir(int F, float U, float V)
    {
        const XMFLOAT3 D = F == 0 ? XMFLOAT3{ 1, -V, -U } : F == 1 ? XMFLOAT3{ -1, -V, U } : F == 2 ? XMFLOAT3{ U, 1, V }
                         : F == 3 ? XMFLOAT3{ U, -1, -V } : F == 4 ? XMFLOAT3{ U, -V, 1 } : XMFLOAT3{ -U, -V, -1 };
        const float L = 1.0f / std::sqrt(D.x * D.x + D.y * D.y + D.z * D.z);
        return { D.x * L, D.y * L, D.z * L };
    }
    void CubeFace(const XMFLOAT3& D, int& F, float& U, float& V)
    {
        const float Ax = std::abs(D.x), Ay = std::abs(D.y), Az = std::abs(D.z);
        if (Ax >= Ay && Ax >= Az) { F = D.x > 0 ? 0 : 1; U = (D.x > 0 ? -D.z : D.z) / Ax; V = -D.y / Ax; }
        else if (Ay >= Az) { F = D.y > 0 ? 2 : 3; U = D.x / Ay; V = (D.y > 0 ? D.z : -D.z) / Ay; }
        else { F = D.z > 0 ? 4 : 5; U = (D.z > 0 ? D.x : -D.x) / Az; V = -D.y / Az; }
    }

    // An RGBA float image read bilinearly
    struct Plane
    {
        const uint8_t* Px = nullptr;
        size_t W = 0, H = 0, Pitch = 0;
        const float* At(size_t X, size_t Y) const { return reinterpret_cast<const float*>(Px + Y * Pitch) + X * 4; }
        XMVECTOR Bilinear(float X, float Y, bool WrapX) const
        {
            X -= 0.5f; Y -= 0.5f;
            const float Fx = std::floor(X), Fy = std::floor(Y), Tx = X - Fx, Ty = Y - Fy;
            const auto Col = [&](long long I) {
                if (WrapX) { I %= (long long)W; if (I < 0) I += W; }
                return (size_t)std::clamp<long long>(I, 0, (long long)W - 1);
            };
            const auto Row = [&](long long J) { return (size_t)std::clamp<long long>(J, 0, (long long)H - 1); };
            const size_t X0 = Col((long long)Fx), X1 = Col((long long)Fx + 1), Y0 = Row((long long)Fy), Y1 = Row((long long)Fy + 1);
            const XMVECTOR A = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(At(X0, Y0))), B = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(At(X1, Y0)));
            const XMVECTOR C = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(At(X0, Y1))), D = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(At(X1, Y1)));
            return XMVectorLerp(XMVectorLerp(A, B, Tx), XMVectorLerp(C, D, Tx), Ty);
        }
    };
    Plane PlaneOf(const Image& I) { return { I.pixels, I.width, I.height, I.rowPitch }; }

    // A direction's colour in an equirectangular panorama, Z up: +X its middle column, +Y to the left
    XMVECTOR SampleEquirect(const Plane& P, const XMFLOAT3& D)
    {
        const float Phi = std::atan2(D.y, D.x), Theta = std::acos(std::clamp(D.z, -1.0f, 1.0f));
        float U = 0.5f - Phi / (2 * Pi);
        U -= std::floor(U);
        return P.Bilinear(U * P.W, Theta / Pi * P.H, true);
    }

    // A cube with every mip, sampled at a fractional mip
    struct MipCube
    {
        std::vector<std::array<Plane, 6>> Mips;
        XMVECTOR At(const XMFLOAT3& D, int Mip) const
        {
            int F; float U, V;
            CubeFace(D, F, U, V);
            const Plane& P = Mips[Mip][F];
            return P.Bilinear((U + 1) * 0.5f * P.W, (V + 1) * 0.5f * P.H, false);
        }
        XMVECTOR Sample(const XMFLOAT3& D, float Lod) const
        {
            Lod = std::clamp(Lod, 0.0f, (float)(Mips.size() - 1));
            const int M0 = (int)Lod, M1 = std::min(M0 + 1, (int)Mips.size() - 1);
            const XMVECTOR A = At(D, M0);
            return M1 == M0 ? A : XMVectorLerp(A, At(D, M1), Lod - M0);
        }
    };

    // Each face's rows on threads of their own
    template <class Fn> void ForRows(size_t Rows, Fn Row)
    {
        const unsigned N = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
        std::atomic<size_t> Next{ 0 };
        std::vector<std::thread> T;
        for (unsigned t = 0; t < N; t++)
            T.emplace_back([&] { for (size_t R; (R = Next++) < Rows;) Row(R); });
        for (auto& Th : T) Th.join();
    }

    void Store(const Image& I, size_t X, size_t Y, XMVECTOR C)
    {
        // non-negative, within half float (BC6H unsigned)
        C = XMVectorClamp(C, XMVectorZero(), XMVectorReplicate(60000.0f));
        C = XMVectorSetW(C, 1.0f);
        XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(I.pixels + Y * I.rowPitch) + X, C);
    }

    bool SaveBc6h(const ScratchImage& Img, const fs::path& File, ID3D11Device* Dev, std::string& How)
    {
        ScratchImage Bc;
        HRESULT Hr = E_FAIL;
        if (Dev)
        {
            Hr = Compress(Dev, Img.GetImages(), Img.GetImageCount(), Img.GetMetadata(), DXGI_FORMAT_BC6H_UF16, TEX_COMPRESS_DEFAULT, 1.0f, Bc);
            How = "gpu";
        }
        if (FAILED(Hr))
        {
            Hr = Compress(Img.GetImages(), Img.GetImageCount(), Img.GetMetadata(), DXGI_FORMAT_BC6H_UF16, TEX_COMPRESS_PARALLEL, TEX_THRESHOLD_DEFAULT, Bc);
            How = "cpu";
        }
        return SUCCEEDED(Hr) && SUCCEEDED(SaveToDDSFile(Bc.GetImages(), Bc.GetImageCount(), Bc.GetMetadata(), DDS_FLAGS_NONE, File.c_str()));
    }
}

bool BakePreviewEnvironment(const std::string& HdrPath, const std::string& OutDir, const std::string& Id, int Size, std::string& Report)
{
    const auto T0 = std::chrono::steady_clock::now();
    const auto Fail = [&](const std::string& Why) { Report += "result=error\nreason=" + Why + "\n"; return false; };
    if (Size < 16 || (Size & (Size - 1))) return Fail("--size must be a power of two, 16 or more");

    // the panorama, as RGBA float
    ScratchImage Hdr, HdrFloat;
    TexMetadata Meta{};
    if (FAILED(LoadFromHDRFile(Strings::ToUnicodeString(HdrPath).c_str(), &Meta, Hdr))) return Fail("cannot read " + HdrPath);
    const Image* Pano = Hdr.GetImage(0, 0, 0);
    if (Meta.format != DXGI_FORMAT_R32G32B32A32_FLOAT)
    {
        if (FAILED(Convert(*Pano, DXGI_FORMAT_R32G32B32A32_FLOAT, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, HdrFloat))) return Fail("cannot convert the panorama");
        Pano = HdrFloat.GetImage(0, 0, 0);
    }
    const Plane Src = PlaneOf(*Pano);

    // its mean luminance over the sphere (each row by its solid angle), the scale to the studio sky's
    double Sum = 0, WSum = 0;
    for (size_t y = 0; y < Src.H; y++)
    {
        const double W = std::sin(Pi * (y + 0.5) / Src.H);
        double Row = 0;
        for (size_t x = 0; x < Src.W; x++) Row += Luminance(Src.At(x, y));
        Sum += W * Row; WSum += W * Src.W;
    }
    const float Mean = (float)(Sum / std::max(WSum, 1e-9));
    if (!(Mean > 0)) return Fail("the panorama is black");
    const float Scale = kStudioMeanLuminance / Mean;

    // the panorama as a cube twice the output's size, every mip
    const size_t SrcSize = (size_t)Size * 2;
    ScratchImage SrcCube, SrcMips;
    if (FAILED(SrcCube.InitializeCube(DXGI_FORMAT_R32G32B32A32_FLOAT, SrcSize, SrcSize, 1, 1))) return Fail("out of memory");
    for (int F = 0; F < 6; F++)
    {
        const Image& Face = *SrcCube.GetImage(0, F, 0);
        ForRows(SrcSize, [&](size_t y) {
            for (size_t x = 0; x < SrcSize; x++)
            {
                const XMFLOAT3 D = CubeDir(F, 2.0f * (x + 0.5f) / SrcSize - 1, 2.0f * (y + 0.5f) / SrcSize - 1);
                Store(Face, x, y, XMVectorScale(SampleEquirect(Src, D), Scale));
            }
        });
    }
    if (FAILED(GenerateMipMaps(SrcCube.GetImages(), SrcCube.GetImageCount(), SrcCube.GetMetadata(), TEX_FILTER_BOX, 0, SrcMips)))
        return Fail("cannot make the source cube's mips");
    MipCube Cube;
    for (size_t M = 0; M < SrcMips.GetMetadata().mipLevels; M++)
    {
        std::array<Plane, 6> Faces;
        for (int F = 0; F < 6; F++) Faces[F] = PlaneOf(*SrcMips.GetImage(M, F, 0));
        Cube.Mips.push_back(Faces);
    }

    // Radiance: mip 0 the mirror (the source halved), mip m GGX of roughness m / (mips - 1) by
    // filtered importance sampling (Krivanek and Colbert 2008: each sample at the source mip its
    // solid angle covers), down to 8 x 8
    int Levels = 1;
    for (int S = Size; S > 8; S /= 2) Levels++;
    ScratchImage Rad;
    if (FAILED(Rad.InitializeCube(DXGI_FORMAT_R32G32B32A32_FLOAT, Size, Size, 1, Levels))) return Fail("out of memory");
    const float TexelAngle = 4 * Pi / (6.0f * SrcSize * SrcSize);
    for (int M = 0; M < Levels; M++)
    {
        const float Rough = (float)M / (Levels - 1), Alpha = std::max(Rough * Rough, 1e-4f);
        struct Tap { XMFLOAT3 H; float Lod; };
        std::vector<Tap> Taps;
        if (M > 0)
        {
            constexpr uint32_t Count = 512;
            for (uint32_t s = 0; s < Count; s++)
            {
                const XMFLOAT3 H = GgxHalf(s, Count, Alpha);
                const float A2 = Alpha * Alpha, Dn = H.z * H.z * (A2 - 1) + 1;
                const float Pdf = A2 / (Pi * Dn * Dn) / 4;   // D(h) n.h / (4 v.h), n = v
                const float Lod = std::max(0.0f, 0.5f * std::log2((1.0f / (Count * Pdf)) / TexelAngle) + 1.0f);
                Taps.push_back({ H, Lod });
            }
        }
        for (int F = 0; F < 6; F++)
        {
            const Image& Face = *Rad.GetImage(M, F, 0);
            const size_t S = Face.width;
            ForRows(S, [&](size_t y) {
                for (size_t x = 0; x < S; x++)
                {
                    const XMFLOAT3 N = CubeDir(F, 2.0f * (x + 0.5f) / S - 1, 2.0f * (y + 0.5f) / S - 1);
                    if (M == 0) { Store(Face, x, y, Cube.Sample(N, 1.0f)); continue; }
                    // a frame about N
                    const XMVECTOR Nv = XMLoadFloat3(&N);
                    const XMVECTOR Up = std::abs(N.z) < 0.999f ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(1, 0, 0, 0);
                    const XMVECTOR Tx = XMVector3Normalize(XMVector3Cross(Up, Nv)), Ty = XMVector3Cross(Nv, Tx);
                    XMVECTOR Acc = XMVectorZero();
                    float W = 0;
                    for (const Tap& T : Taps)
                    {
                        const XMVECTOR H = XMVectorAdd(XMVectorAdd(XMVectorScale(Tx, T.H.x), XMVectorScale(Ty, T.H.y)), XMVectorScale(Nv, T.H.z));
                        const XMVECTOR L = XMVectorSubtract(XMVectorScale(H, 2 * T.H.z), Nv);
                        const float NdotL = XMVectorGetX(XMVector3Dot(Nv, L));
                        if (NdotL <= 0) continue;
                        XMFLOAT3 Ld;
                        XMStoreFloat3(&Ld, L);
                        Acc = XMVectorAdd(Acc, XMVectorScale(Cube.Sample(Ld, T.Lod), NdotL));
                        W += NdotL;
                    }
                    Store(Face, x, y, W > 0 ? XMVectorScale(Acc, 1.0f / W) : Cube.Sample(N, 1.0f));
                }
            });
        }
    }

    // Irradiance over pi, from the source's spherical harmonics (Ramamoorthi and Hanrahan 2001),
    // read at its 32 x 32 mip
    float Sh[9][3] = {};
    {
        size_t M = 0;
        while (M + 1 < Cube.Mips.size() && Cube.Mips[M][0].W > 32) M++;
        for (int F = 0; F < 6; F++)
        {
            const Plane& P = Cube.Mips[M][F];
            for (size_t y = 0; y < P.H; y++)
                for (size_t x = 0; x < P.W; x++)
                {
                    const float U = 2.0f * (x + 0.5f) / P.W - 1, V = 2.0f * (y + 0.5f) / P.H - 1;
                    const XMFLOAT3 D = CubeDir(F, U, V);
                    const float Omega = (4.0f / (P.W * P.H)) / std::pow(1 + U * U + V * V, 1.5f);
                    const float Y[9] = { 0.282095f, 0.488603f * D.y, 0.488603f * D.z, 0.488603f * D.x, 1.092548f * D.x * D.y,
                                         1.092548f * D.y * D.z, 0.315392f * (3 * D.z * D.z - 1), 1.092548f * D.x * D.z,
                                         0.546274f * (D.x * D.x - D.y * D.y) };
                    const float* C = P.At(x, y);
                    for (int k = 0; k < 9; k++)
                        for (int c = 0; c < 3; c++) Sh[k][c] += C[c] * Y[k] * Omega;
                }
        }
        const float Band[9] = { Pi, 2 * Pi / 3, 2 * Pi / 3, 2 * Pi / 3, Pi / 4, Pi / 4, Pi / 4, Pi / 4, Pi / 4 };
        for (int k = 0; k < 9; k++)
            for (int c = 0; c < 3; c++) Sh[k][c] *= Band[k] / Pi;
    }
    constexpr size_t IrrSize = 32;
    ScratchImage Irr;
    if (FAILED(Irr.InitializeCube(DXGI_FORMAT_R32G32B32A32_FLOAT, IrrSize, IrrSize, 1, 1))) return Fail("out of memory");
    for (int F = 0; F < 6; F++)
    {
        const Image& Face = *Irr.GetImage(0, F, 0);
        for (size_t y = 0; y < IrrSize; y++)
            for (size_t x = 0; x < IrrSize; x++)
            {
                const XMFLOAT3 D = CubeDir(F, 2.0f * (x + 0.5f) / IrrSize - 1, 2.0f * (y + 0.5f) / IrrSize - 1);
                const float Y[9] = { 0.282095f, 0.488603f * D.y, 0.488603f * D.z, 0.488603f * D.x, 1.092548f * D.x * D.y,
                                     1.092548f * D.y * D.z, 0.315392f * (3 * D.z * D.z - 1), 1.092548f * D.x * D.z,
                                     0.546274f * (D.x * D.x - D.y * D.y) };
                float C[3] = {};
                for (int k = 0; k < 9; k++)
                    for (int c = 0; c < 3; c++) C[c] += Sh[k][c] * Y[k];
                Store(Face, x, y, XMVectorSet(C[0], C[1], C[2], 1));
            }
    }

    // BC6H, on the GPU when there is one
    std::error_code Ec;
    fs::create_directories(fs::path(Strings::ToUnicodeString(OutDir)), Ec);
    const fs::path Base = fs::path(Strings::ToUnicodeString(OutDir));
    ComPtr<ID3D11Device> Dev;
    const D3D_FEATURE_LEVEL Want[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, Want, 1, D3D11_SDK_VERSION, &Dev, nullptr, nullptr);
    std::string How;
    const fs::path RadFile = Base / Strings::ToUnicodeString(Id + "_radiance.dds"), IrrFile = Base / Strings::ToUnicodeString(Id + "_irradiance.dds");
    if (!SaveBc6h(Rad, RadFile, Dev.Get(), How)) return Fail("cannot write " + RadFile.string());
    if (!SaveBc6h(Irr, IrrFile, Dev.Get(), How)) return Fail("cannot write " + IrrFile.string());

    Report += std::format("result=ok\nradiance={} ({} bytes, {} x {}, {} mips)\nirradiance={} ({} bytes)\n"
                          "panorama={} x {}\nmean_luminance={:.4f}\nscale={:.5f}\ncompress={}\ntime_ms={}\n",
                          RadFile.string(), fs::file_size(RadFile, Ec), Size, Size, Levels, IrrFile.string(), fs::file_size(IrrFile, Ec),
                          Src.W, Src.H, Mean, Scale, How,
                          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - T0).count());
    return true;
}

// ---- bake-environment <in.hdr> --out <dir> [--id name] [--size px]

namespace
{
    std::string BakeId;
    int BakeSize = 256;

    int RunBakeEnvironment(const CliOptions& Options, const std::string& Input)
    {
        const std::string Id = BakeId.empty() ? fs::path(Input).stem().string() : BakeId;
        std::string Report;
        const bool Ok = BakePreviewEnvironment(Input, Options.Out.empty() ? std::string(".") : Options.Out, Id, BakeSize, Report);
        CliReport(Options, Report);
        return Ok ? 0 : 5;
    }

    void AddBakeEnvironmentOptions(CLI::App& Sub, CliOptions&)
    {
        Sub.add_option("--id", BakeId, "The environment's id, its files' prefix (default: the file's name)");
        Sub.add_option("--size", BakeSize, "The radiance cube's face in pixels, a power of two (256)");
    }

    const GameCommand BakeEnvironmentCommand{
        "bake-environment",
        "Bake an HDR panorama into the model preview's environment cubes (<id>_radiance.dds, <id>_irradiance.dds in --out)",
        "An equirectangular .hdr, Z up",
        RunBakeEnvironment,
        AddBakeEnvironmentOptions,
    };
    const GameCommandRegistration BakeEnvironmentRegistered(BakeEnvironmentCommand);
}
