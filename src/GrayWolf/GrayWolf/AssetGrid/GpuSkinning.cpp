#include "stdafx.h"

#include "GpuSkinning.h"
#include "ModelPreview.h"      // PreviewGeometry, PREVIEW_MAX_BONE_INFLUENCES
#include "SkeletalPreview.h"   // SkeletonPoseBone

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace
{
    const char* kSkinShader = R"(
struct Vtx { float3 P; float3 N; float2 UV; int4 B; float4 W; };
StructuredBuffer<Vtx> Bind : register(t0);
StructuredBuffer<float4> Bones : register(t1);   // per bone: the skin rotation (quaternion), the translation
RWByteAddressBuffer Out : register(u0);
cbuffer Counts : register(b0) { uint Count; uint BoneCount; uint2 Pad; };

float3 QRot(float4 q, float3 v) { float3 t = 2 * cross(q.xyz, v); return v + q.w * t + cross(q.xyz, t); }

[numthreads(64, 1, 1)]
void Main(uint3 Id : SV_DispatchThreadID)
{
    uint i = Id.x;
    if (i >= Count) return;
    Vtx v = Bind[i];
    float3 p = 0, n = 0;
    float ws = 0;
    [unroll] for (int k = 0; k < 4; k++)
    {
        int b = v.B[k]; float w = v.W[k];
        if (b < 0 || (uint)b >= BoneCount || w <= 0) continue;
        float4 q = Bones[b * 2];
        p += (QRot(q, v.P) + Bones[b * 2 + 1].xyz) * w;
        n += QRot(q, v.N) * w;
        ws += w;
    }
    if (ws > 1e-6) { p /= ws; n /= ws; } else { p = v.P; n = v.N; }
    uint o = i * 32;
    Out.Store3(o, asuint(p));
    Out.Store3(o + 12, asuint(n));
    Out.Store2(o + 24, asuint(v.UV));
}
)";

    struct Q { float x, y, z, w; };
    Q Mul(const Q& a, const Q& b)
    {
        return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    }
    Q Norm(const Q& q)
    {
        const float L = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return L < 1e-8f ? Q{ 0, 0, 0, 1 } : Q{ q.x / L, q.y / L, q.z / L, q.w / L };
    }
    void Rot(const Q& q, const float v[3], float o[3])
    {
        const float t[3] = { 2 * (q.y * v[2] - q.z * v[1]), 2 * (q.z * v[0] - q.x * v[2]), 2 * (q.x * v[1] - q.y * v[0]) };
        o[0] = v[0] + q.w * t[0] + (q.y * t[2] - q.z * t[1]);
        o[1] = v[1] + q.w * t[1] + (q.z * t[0] - q.x * t[2]);
        o[2] = v[2] + q.w * t[2] + (q.x * t[1] - q.y * t[0]);
    }
}

GpuSkinning::Rig GpuSkinning::Upload(ID3D11Device* Dev, const PreviewGeometry& Bind)
{
    Rig R;
    const size_t N = Bind.Pos.size() / 3;
    if (!N || Bind.Nrm.size() != N * 3 || Bind.UV.size() != N * 2) return R;
    struct Vtx { float P[3], Nr[3], UV[2]; int32_t B[4]; float W[4]; };
    static_assert(sizeof(Vtx) == 64, "the shader's Vtx");
    std::vector<Vtx> V(N);
    const bool Skinned = Bind.VtxBoneIdx.size() >= N * PREVIEW_MAX_BONE_INFLUENCES && Bind.VtxBoneWeight.size() >= N * PREVIEW_MAX_BONE_INFLUENCES;
    for (size_t i = 0; i < N; i++)
    {
        std::memcpy(V[i].P, &Bind.Pos[i * 3], 12); std::memcpy(V[i].Nr, &Bind.Nrm[i * 3], 12); std::memcpy(V[i].UV, &Bind.UV[i * 2], 8);
        for (int k = 0; k < 4; k++)
        {
            const size_t S = i * PREVIEW_MAX_BONE_INFLUENCES + k;
            V[i].B[k] = Skinned && k < PREVIEW_MAX_BONE_INFLUENCES ? Bind.VtxBoneIdx[S] : -1;
            V[i].W[k] = Skinned && k < PREVIEW_MAX_BONE_INFLUENCES ? Bind.VtxBoneWeight[S] : 0.0f;
        }
    }
    D3D11_BUFFER_DESC Bd{}; Bd.ByteWidth = (UINT)(N * sizeof(Vtx)); Bd.Usage = D3D11_USAGE_IMMUTABLE; Bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; Bd.StructureByteStride = sizeof(Vtx);
    D3D11_SUBRESOURCE_DATA Sd{}; Sd.pSysMem = V.data();
    ComPtr<ID3D11Buffer> Buf;
    if (FAILED(Dev->CreateBuffer(&Bd, &Sd, &Buf)) || FAILED(Dev->CreateShaderResourceView(Buf.Get(), nullptr, &R.Vertices))) return {};
    const UINT Counts[4] = { (UINT)N, (UINT)Bind.Bones.size(), 0, 0 };
    D3D11_BUFFER_DESC Cd{}; Cd.ByteWidth = sizeof(Counts); Cd.Usage = D3D11_USAGE_IMMUTABLE; Cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA Cs{}; Cs.pSysMem = Counts;
    if (FAILED(Dev->CreateBuffer(&Cd, &Cs, &R.Counts))) return {};
    R.VertexCount = (UINT)N;
    R.BoneCount = (UINT)Bind.Bones.size();
    return R;
}

bool GpuSkinning::MakeOutput(ID3D11Device* Dev, UINT VertexCount, ComPtr<ID3D11Buffer>& Vb, ComPtr<ID3D11UnorderedAccessView>& Uav)
{
    D3D11_BUFFER_DESC Bd{}; Bd.ByteWidth = VertexCount * 32; Bd.Usage = D3D11_USAGE_DEFAULT;
    Bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS; Bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(Dev->CreateBuffer(&Bd, nullptr, &Vb))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC Ud{}; Ud.Format = DXGI_FORMAT_R32_TYPELESS; Ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    Ud.Buffer.NumElements = VertexCount * 8; Ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    return SUCCEEDED(Dev->CreateUnorderedAccessView(Vb.Get(), &Ud, &Uav));
}

bool GpuSkinning::Init(ID3D11Device* D)
{
    if (Cs) return true;
    Dev = D;
    ComPtr<ID3DBlob> Blob, Err;
    if (FAILED(D3DCompile(kSkinShader, std::strlen(kSkinShader), "GridSkin", nullptr, nullptr, "Main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Blob, &Err)))
    {
        if (Err) OutputDebugStringA((const char*)Err->GetBufferPointer());
        return false;
    }
    return SUCCEEDED(D->CreateComputeShader(Blob->GetBufferPointer(), Blob->GetBufferSize(), nullptr, &Cs));
}

void GpuSkinning::Skin(ID3D11DeviceContext* Ctx, const Rig& R, const PreviewGeometry& Bind, const std::vector<SkeletonPoseBone>& WorldPose,
                       ID3D11UnorderedAccessView* Out)
{
    if (!Cs || !R.Valid() || !Out) return;
    const UINT N = std::max(1u, R.BoneCount);
    if (N > BonesCapacity)
    {
        BonesCapacity = std::max(N, 256u);
        D3D11_BUFFER_DESC Bd{}; Bd.ByteWidth = BonesCapacity * 32; Bd.Usage = D3D11_USAGE_DYNAMIC; Bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; Bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; Bd.StructureByteStride = 16;
        Bones.Reset(); BonesSrv.Reset();
        if (FAILED(Dev->CreateBuffer(&Bd, nullptr, &Bones)) || FAILED(Dev->CreateShaderResourceView(Bones.Get(), nullptr, &BonesSrv))) { BonesCapacity = 0; return; }
    }
    // each bone's skin transform: its pose's world transform after the inverse of its bind's
    Scratch.assign((size_t)N * 8, 0.0f);
    for (UINT b = 0; b < R.BoneCount && b < WorldPose.size() && b < Bind.Bones.size(); b++)
    {
        const auto& B = Bind.Bones[b];
        const Q BindInv = [&] { const Q q = Norm({ B.Rot[0], B.Rot[1], B.Rot[2], B.Rot[3] }); return Q{ -q.x, -q.y, -q.z, q.w }; }();
        const Q Cur{ WorldPose[b].Rot[0], WorldPose[b].Rot[1], WorldPose[b].Rot[2], WorldPose[b].Rot[3] };
        const Q S = Norm(Mul(Cur, BindInv));
        float Moved[3];
        Rot(S, B.Pos, Moved);
        float* O = &Scratch[(size_t)b * 8];
        O[0] = S.x; O[1] = S.y; O[2] = S.z; O[3] = S.w;
        O[4] = WorldPose[b].Pos[0] - Moved[0]; O[5] = WorldPose[b].Pos[1] - Moved[1]; O[6] = WorldPose[b].Pos[2] - Moved[2];
    }
    D3D11_MAPPED_SUBRESOURCE Map{};
    if (FAILED(Ctx->Map(Bones.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Map))) return;
    std::memcpy(Map.pData, Scratch.data(), Scratch.size() * sizeof(float));
    Ctx->Unmap(Bones.Get(), 0);

    ID3D11ShaderResourceView* Srvs[] = { R.Vertices.Get(), BonesSrv.Get() };
    ID3D11Buffer* Cbs[] = { R.Counts.Get() };
    Ctx->CSSetShader(Cs.Get(), nullptr, 0);
    Ctx->CSSetShaderResources(0, 2, Srvs);
    Ctx->CSSetConstantBuffers(0, 1, Cbs);
    Ctx->CSSetUnorderedAccessViews(0, 1, &Out, nullptr);
    Ctx->Dispatch((R.VertexCount + 63) / 64, 1, 1);
    ID3D11UnorderedAccessView* NoUav[] = { nullptr };
    ID3D11ShaderResourceView* NoSrv[] = { nullptr, nullptr };
    Ctx->CSSetUnorderedAccessViews(0, 1, NoUav, nullptr);
    Ctx->CSSetShaderResources(0, 2, NoSrv);
    Ctx->CSSetShader(nullptr, nullptr, 0);
}
