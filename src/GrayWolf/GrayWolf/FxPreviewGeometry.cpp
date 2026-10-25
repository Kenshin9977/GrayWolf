#include "stdafx.h"

#include "FxPreviewGeometry.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace FxPreview::Geometry
{
namespace
{
    // How much brighter an emissive material draws than its texture times its colour: from its
    // HDR intensity when the game gives it (Black Ops III's hdrScale: fog 8, fire 256, sparks 512,
    // "_i12000" 12000; its square root, fire about 3), else 2.5. The preview has no HDR: past
    // white it clips.
    float EmissiveBoost(const Material* M)
    {
        if (!M || (M->Mode != Blend::Premultiplied && M->HdrScale <= 0)) return 1.0f;
        return M->HdrScale > 0 ? std::clamp(std::sqrt(M->HdrScale) / 5.3f, 0.3f, 12.0f) : 2.5f;
    }

    // a quad's whole image: the soft dot whole, not an atlas frame of it
    const float Full[4] = { 0, 0, 1, 1 };
}

float ToLinear(float C) { return std::pow(std::clamp(C, 0.0f, 1.0f), 2.2f); }

void Batch::Clear()
{
    Verts.clear();
    Items.clear();
    Quads = 0;
}

void Batch::Quad(const XMVECTOR& C, const XMVECTOR& Right, const XMVECTOR& Up, const float* Uv, const float* Col, const View& V,
                 ID3D11ShaderResourceView* Srv, int Mode)
{
    Item It;
    It.Depth = XMVectorGetX(XMVector3Dot(XMVectorSubtract(C, V.Eye), V.Forward));
    It.Srv = Srv;
    It.Mode = Mode;
    It.Bright = NextBright;
    It.First = (uint32_t)Verts.size();
    It.Count = 6;
    const XMVECTOR P[4] = { XMVectorAdd(XMVectorSubtract(C, Right), Up), XMVectorAdd(XMVectorAdd(C, Right), Up),
                            XMVectorSubtract(XMVectorAdd(C, Right), Up), XMVectorSubtract(XMVectorSubtract(C, Right), Up) };
    // a lit sprite shaded as a ball under the sun: each corner's normal leans out of the quad
    // towards the viewer, lit by an ambient and one light from above
    float Shade[4] = { 1, 1, 1, 1 };
    if (NextLit)
    {
        const XMVECTOR Sun = XMVector3Normalize(XMVectorSet(0.35f, 0.25f, 0.9f, 0));
        const XMVECTOR Toward = XMVector3Normalize(XMVectorSubtract(V.Eye, C));
        for (int K = 0; K < 4; K++)
        {
            const XMVECTOR N = XMVector3Normalize(XMVectorAdd(XMVectorScale(XMVector3Normalize(XMVectorSubtract(P[K], C)), 0.8f),
                                                              XMVectorScale(Toward, 0.6f)));
            Shade[K] = 0.42f + 0.7f * std::max(0.0f, XMVectorGetX(XMVector3Dot(N, Sun)));
        }
    }
    const float U[4][2] = { { Uv[0], Uv[1] }, { Uv[0] + Uv[2], Uv[1] }, { Uv[0] + Uv[2], Uv[1] + Uv[3] }, { Uv[0], Uv[1] + Uv[3] } };
    for (int K : { 0, 1, 2, 0, 2, 3 })
    {
        Vertex Vx;
        XMFLOAT3 F;
        XMStoreFloat3(&F, P[K]);
        Vx.Pos[0] = F.x; Vx.Pos[1] = F.y; Vx.Pos[2] = F.z;
        Vx.Uv[0] = U[K][0]; Vx.Uv[1] = U[K][1];
        std::copy(Col, Col + 4, Vx.Col);
        for (int X = 0; X < 3; X++) Vx.Col[X] *= Shade[K];
        Verts.push_back(Vx);
    }
    Items.push_back(It);
    Quads++;
}

// ---- particles

void Batch::AddParticles(const Frame& F, const View& V, const DrawOptions& O, const Images& Img, std::vector<const Particle*>& Models)
{
    for (const Particle& Pt : F.Particles)
    {
        if (Pt.Type == ElemType::Model)
        {
            if (Pt.Model) Models.push_back(&Pt);
            continue;
        }
        const XMVECTOR C = XMVectorSet(Pt.Pos[0], Pt.Pos[1], Pt.Pos[2], 0);
        float Col[4] = { ToLinear(Pt.Color[0]), ToLinear(Pt.Color[1]), ToLinear(Pt.Color[2]), std::clamp(Pt.Color[3], 0.0f, 1.0f) };
        if (Pt.Type == ElemType::Light || Pt.Type == ElemType::SpotLight || Pt.Type == ElemType::LensFlare)
            AddLight(Pt, C, Col, V, O, Img);
        else if (Pt.Type == ElemType::Beam)
            AddBeam(Pt, C, Col, V, Img);
        else
            AddSprite(Pt, C, Col, V, O, Img);
    }
}

void Batch::AddLight(const Particle& Pt, const XMVECTOR& C, float* Col, const View& V, const DrawOptions& O, const Images& Img)
{
    if (!O.ShowLights) return;
    // Black Ops III gives the light's reach and intensity, the older games its size
    const float Reach = Pt.LightRadius > 0 ? Pt.LightRadius : std::max(Pt.Size[0], 8.0f);
    const float Strength = std::clamp(Pt.LightIntensity > 0 ? Pt.LightIntensity : 1.0f, 0.2f, 1.5f);
    if (Col[0] + Col[1] + Col[2] < 0.01f) Col[0] = Col[1] = Col[2] = 1.0f;
    if (Pt.Type == ElemType::LensFlare)
    {
        AddLensFlare(C, Col, Strength, V, Img);
        return;
    }
    // a glow where it is, a tenth of its reach: the bloom a bright light leaves (a light lights,
    // it is not seen: the flash of an explosion is its light)
    const float R = std::clamp(Reach * 0.1f, 2.0f, 40.0f);
    const float Glow[4] = { Col[0], Col[1], Col[2], 0.3f * Strength };
    Quad(C, XMVectorScale(V.Right, R), XMVectorScale(V.Up, R), Full, Glow, V, Img.Dot, (int)Blend::Additive);
    if (Pt.Type == ElemType::SpotLight) AddSpotCone(Pt, C, Col, Reach, Strength, V, Img);
}

void Batch::AddLensFlare(const XMVECTOR& C, const float* Col, float Strength, const View& V, const Images& Img)
{
    // a flare as big on the screen wherever it is: a glow and a horizontal streak
    const float Dist = std::max(1.0f, XMVectorGetX(XMVector3Dot(XMVectorSubtract(C, V.Eye), V.Forward)));
    const float S = Dist * std::tan(V.Fov * 0.5f) * 0.05f;
    const float Glow[4] = { Col[0], Col[1], Col[2], 0.55f * Strength };
    const float Streak[4] = { Col[0], Col[1], Col[2], 0.3f * Strength };
    Quad(C, XMVectorScale(V.Right, S), XMVectorScale(V.Up, S), Full, Glow, V, Img.Dot, (int)Blend::Additive);
    Quad(C, XMVectorScale(V.Right, S * 7), XMVectorScale(V.Up, S * 0.15f), Full, Streak, V, Img.Dot, (int)Blend::Additive);
}

void Batch::AddSpotCone(const Particle& Pt, const XMVECTOR& C, const float* Col, float Reach, float Strength, const View& V,
                        const Images& Img)
{
    // its cone along the element's forward axis, fading to its reach; truncated when
    // its def gives a radius at the light (MW3's, Ghosts' startRadius)
    float Half = Pt.LightFov > 0 ? Pt.LightFov : 60.0f;
    Half = (Half > 3.2f ? XMConvertToRadians(Half) : Half) * 0.5f;
    Half = std::clamp(Half, 0.05f, 1.4f);
    const XMVECTOR Dir = XMVector3Normalize(XMVectorSet(Pt.Axis[0][0], Pt.Axis[0][1], Pt.Axis[0][2], 0));
    const XMVECTOR U = XMVector3Normalize(XMVectorSet(Pt.Axis[1][0], Pt.Axis[1][1], Pt.Axis[1][2], 0));
    const XMVECTOR W = XMVector3Cross(Dir, U);
    const float Start = Pt.El ? std::clamp(Pt.El->Light.StartRadius, 0.0f, 400.0f) : 0.0f;
    const float L = std::min(Reach, 400.0f), Rim = Start + L * std::tan(Half);
    Item It;
    It.Srv = Img.White;
    It.Mode = (int)Blend::Additive;
    It.First = (uint32_t)Verts.size();
    It.Depth = XMVectorGetX(XMVector3Dot(XMVectorSubtract(XMVectorAdd(C, XMVectorScale(Dir, L * 0.5f)), V.Eye), V.Forward));
    const int Sides = 24;
    const auto Put = [&](XMVECTOR P, float A) {
        Vertex Vx{};
        XMFLOAT3 F3;
        XMStoreFloat3(&F3, P);
        Vx.Pos[0] = F3.x, Vx.Pos[1] = F3.y, Vx.Pos[2] = F3.z;
        Vx.Uv[0] = Vx.Uv[1] = 0.5f;
        Vx.Col[0] = Col[0], Vx.Col[1] = Col[1], Vx.Col[2] = Col[2], Vx.Col[3] = A;
        Verts.push_back(Vx);
    };
    for (int K = 0; K < Sides; K++)
    {
        const float A0 = XM_2PI * K / Sides, A1 = XM_2PI * (K + 1) / Sides;
        const XMVECTOR End = XMVectorAdd(C, XMVectorScale(Dir, L));
        const auto Ring = [&](const XMVECTOR& At, float R, float A) {
            return XMVectorAdd(At, XMVectorAdd(XMVectorScale(U, R * std::cos(A)), XMVectorScale(W, R * std::sin(A))));
        };
        Put(Ring(C, Start, A0), 0.12f * Strength);
        Put(Ring(End, Rim, A0), 0.0f);
        Put(Ring(End, Rim, A1), 0.0f);
        if (Start <= 0) continue;
        Put(Ring(C, Start, A0), 0.12f * Strength);
        Put(Ring(End, Rim, A1), 0.0f);
        Put(Ring(C, Start, A1), 0.12f * Strength);
    }
    It.Count = (uint32_t)Verts.size() - It.First;
    Items.push_back(It);
    Quads++;
}

void Batch::AddBeam(const Particle& Pt, const XMVECTOR& C, const float* Col, const View& V, const Images& Img)
{
    // from where it is along its forward axis (the target is the game's to give)
    const XMVECTOR Dir = XMVector3Normalize(XMVectorSet(Pt.Axis[0][0], Pt.Axis[0][1], Pt.Axis[0][2], 0));
    const float Len = Pt.Size[1] > 1.0f ? Pt.Size[1] * 2 : 96.0f, Width = std::max(Pt.Size[0], 1.0f);
    XMVECTOR Side = XMVector3Cross(Dir, XMVectorSubtract(V.Eye, C));
    Side = XMVector3LengthSq(Side).m128_f32[0] < 1e-8f ? V.Right : XMVector3Normalize(Side);
    const float Ray[4] = { Col[0] + Col[1] + Col[2] < 0.01f ? 1.0f : Col[0], Col[0] + Col[1] + Col[2] < 0.01f ? 1.0f : Col[1],
                           Col[0] + Col[1] + Col[2] < 0.01f ? 1.0f : Col[2], std::max(Col[3], 0.4f) };
    Quad(XMVectorAdd(C, XMVectorScale(Dir, Len * 0.5f)), XMVectorScale(Side, Width), XMVectorScale(Dir, Len * 0.5f), Full, Ray,
         V, Img.Dot, (int)Blend::Additive);
}

// a particle drawn with its material's image: by its element's type
void Batch::AddSprite(const Particle& Pt, const XMVECTOR& C, float* Col, const View& V, const DrawOptions& O, const Images& Img)
{
    const Material* M = Pt.Mat;
    if (M && M->Distortion) return;
    if (M && M->LightSprite)
    {
        // a sprite light lights what is around it: the glow a light leaves (its image is the
        // falloff the game lights with, not a picture)
        if (!O.ShowLights) return;
        const float R = std::clamp(std::max(Pt.Size[0], Pt.Size[1]) * 0.15f, 2.0f, 40.0f);
        const float Glow[4] = { Col[0], Col[1], Col[2], 0.3f * Col[3] };
        Quad(C, XMVectorScale(V.Right, R), XMVectorScale(V.Up, R), Full, Glow, V, Img.Dot, (int)Blend::Additive);
        return;
    }
    const int Mode = M ? (int)M->Mode : (int)Blend::Alpha;
    NextBright = EmissiveBoost(M);
    NextLit = M && M->Lit && NextBright <= 1.0f;
    struct Reset { Batch& B; ~Reset() { B.NextBright = 1; B.NextLit = false; } } Back{ *this };
    ID3D11ShaderResourceView* Srv = Img.Of(M);
    const float* Uv = Srv == Img.Dot ? Full : Pt.Uv;
    if (Col[3] <= 0.002f && Mode != (int)Blend::Opaque) return;
    // its brightness (vfx IntensityGraph): past white the shader bleeds it to white
    if (Pt.Intensity != 1.0f)
        for (int K = 0; K < 3; K++) Col[K] *= std::clamp(Pt.Intensity, 0.0f, 16.0f);
    if (Pt.Size[0] <= 0 && Pt.Size[1] <= 0) return;
    const Look L{ Uv, Col, Srv, Mode };
    switch (Pt.Type)
    {
    case ElemType::Decal: AddDecal(Pt, L, V, O); break;
    case ElemType::Tail:
    case ElemType::Line: AddTail(Pt, C, L, V); break;
    case ElemType::Oriented: AddOriented(Pt, C, L, V); break;
    case ElemType::Cloud: AddCloud(Pt, C, L, V); break;
    case ElemType::Cylinder: AddCylinder(Pt, C, L, V); break;
    default: AddBillboard(Pt, C, L, V); break;
    }
}

void Batch::AddDecal(const Particle& Pt, const Look& L, const View& V, const DrawOptions& O)
{
    // a mark lying on the ground under it, turned by its rotation
    const float Sx = Pt.Size[0], Cr = std::cos(Pt.Rotation), Sr = std::sin(Pt.Rotation);
    const float G = O.GridCenter[2] + 0.25f;
    const XMVECTOR At = XMVectorSet(Pt.Pos[0], Pt.Pos[1], G, 0);
    const XMVECTOR R = XMVectorSet(Cr, Sr, 0, 0), U = XMVectorSet(-Sr, Cr, 0, 0);
    Quad(At, XMVectorScale(R, Sx), XMVectorScale(U, Sx), L.Uv, L.Col, V, L.Srv, L.Mode);
}

void Batch::AddTail(const Particle& Pt, const XMVECTOR& C, const Look& L, const View& V)
{
    // along the velocity, behind the particle: half length Size[1], half width Size[0]
    const float Sx = Pt.Size[0], Sy = Pt.Size[1];
    const XMVECTOR Dir = XMVector3Normalize(XMVectorSet(Pt.Dir[0], Pt.Dir[1], Pt.Dir[2], 0));
    XMVECTOR Side = XMVector3Cross(Dir, XMVectorSubtract(V.Eye, C));
    Side = XMVector3LengthSq(Side).m128_f32[0] < 1e-8f ? V.Right : XMVector3Normalize(Side);
    const XMVECTOR Mid = XMVectorSubtract(C, XMVectorScale(Dir, Sy));
    Quad(Mid, XMVectorScale(Side, Sx), XMVectorScale(Dir, Sy), L.Uv, L.Col, V, L.Srv, L.Mode);
}

void Batch::AddOriented(const Particle& Pt, const XMVECTOR& C, const Look& L, const View& V)
{
    const float Sx = Pt.Size[0], Sy = Pt.Size[1], Cr = std::cos(Pt.Rotation), Sr = std::sin(Pt.Rotation);
    const XMVECTOR Tn = XMVectorSet(Pt.Axis[1][0], Pt.Axis[1][1], Pt.Axis[1][2], 0);
    const XMVECTOR Bn = XMVectorSet(Pt.Axis[2][0], Pt.Axis[2][1], Pt.Axis[2][2], 0);
    const XMVECTOR R = XMVectorAdd(XMVectorScale(Tn, Cr), XMVectorScale(Bn, Sr));
    const XMVECTOR U = XMVectorSubtract(XMVectorScale(Tn, Sr), XMVectorScale(Bn, Cr));
    Quad(C, XMVectorScale(R, Sx), XMVectorScale(U, -Sy), L.Uv, L.Col, V, L.Srv, L.Mode);
}

void Batch::AddCloud(const Particle& Pt, const XMVECTOR& C, const Look& L, const View& V)
{
    // a puff of small quads within the cloud's scale around it, stretched nowhere
    const float Spread = std::max(Pt.Scale, 0.0f);
    if (Spread <= 0) return;
    const int Count = 24;
    uint32_t H = Pt.Seed * 2654435761u + 1;
    const auto Next = [&] { H ^= H << 13; H ^= H >> 17; H ^= H << 5; return (H & 0xffffff) / 16777216.0f; };
    for (int K = 0; K < Count; K++)
    {
        // a gaussian-ish spread: the mean of three
        float D[3];
        for (float& Dk : D) Dk = (Next() + Next() + Next() - 1.5f) * 1.2f;
        const XMVECTOR Q = XMVectorAdd(C, XMVectorSet(D[0] * Spread, D[1] * Spread, D[2] * Spread, 0));
        Quad(Q, XMVectorScale(V.Right, Pt.Size[0]), XMVectorScale(V.Up, Pt.Size[1]), L.Uv, L.Col, V, L.Srv, L.Mode);
    }
}

void Batch::AddCylinder(const Particle& Pt, const XMVECTOR& C, const Look& L, const View& V)
{
    // CoD1's ring wall (a water sheet, a shock wave): from radius Sx where it is to Sy its
    // height (Scale) along its forward axis, the image once around it, its top up
    const float Sx = Pt.Size[0], Sy = Pt.Size[1];
    const float* Uv = L.Uv;
    const XMVECTOR Dir = XMVector3Normalize(XMVectorSet(Pt.Axis[0][0], Pt.Axis[0][1], Pt.Axis[0][2], 0));
    const XMVECTOR U = XMVector3Normalize(XMVectorSet(Pt.Axis[1][0], Pt.Axis[1][1], Pt.Axis[1][2], 0));
    const XMVECTOR W = XMVector3Cross(Dir, U);
    const XMVECTOR Top = XMVectorAdd(C, XMVectorScale(Dir, Pt.Scale));
    Item It;
    It.Srv = L.Srv;
    It.Mode = L.Mode;
    It.Bright = NextBright;
    It.First = (uint32_t)Verts.size();
    It.Depth = XMVectorGetX(XMVector3Dot(XMVectorSubtract(XMVectorAdd(C, XMVectorScale(Dir, Pt.Scale * 0.5f)), V.Eye), V.Forward));
    const int Sides = 32;
    const auto Put = [&](const XMVECTOR& At, float R, int K, bool Up) {
        const float A = XM_2PI * K / Sides;
        Vertex Vx{};
        XMFLOAT3 F3;
        XMStoreFloat3(&F3, XMVectorAdd(At, XMVectorAdd(XMVectorScale(U, R * std::cos(A)), XMVectorScale(W, R * std::sin(A)))));
        Vx.Pos[0] = F3.x, Vx.Pos[1] = F3.y, Vx.Pos[2] = F3.z;
        Vx.Uv[0] = Uv[0] + Uv[2] * K / Sides;
        Vx.Uv[1] = Uv[1] + (Up ? 0.0f : Uv[3]);
        std::copy(L.Col, L.Col + 4, Vx.Col);
        Verts.push_back(Vx);
    };
    for (int K = 0; K < Sides; K++)
    {
        Put(C, Sx, K, false), Put(Top, Sy, K, true), Put(Top, Sy, K + 1, true);
        Put(C, Sx, K, false), Put(Top, Sy, K + 1, true), Put(C, Sx, K + 1, false);
    }
    It.Count = (uint32_t)Verts.size() - It.First;
    Items.push_back(It);
    Quads++;
}

void Batch::AddBillboard(const Particle& Pt, const XMVECTOR& C, const Look& L, const View& V)
{
    // facing the camera, turned by its rotation
    const float Sx = Pt.Size[0], Sy = Pt.Size[1], Cr = std::cos(Pt.Rotation), Sr = std::sin(Pt.Rotation);
    const XMVECTOR R = XMVectorAdd(XMVectorScale(V.Right, Cr), XMVectorScale(V.Up, Sr));
    const XMVECTOR U = XMVectorSubtract(XMVectorScale(V.Right, -Sr), XMVectorScale(V.Up, -Cr));
    Quad(C, XMVectorScale(R, Sx), XMVectorScale(U, Sy), L.Uv, L.Col, V, L.Srv, L.Mode);
}

// ---- trails

void Batch::AddTrails(const Frame& F, const View& V, const Images& Img)
{
    for (const TrailStrip& S : F.Trails)
    {
        if (S.Points.size() < 2) continue;
        const Material* M = S.Mat;
        Item It;
        It.Srv = Img.Of(M);
        It.Mode = M ? (int)M->Mode : (int)Blend::Alpha;
        It.Wrap = S.Wrap;
        It.First = (uint32_t)Verts.size();
        if (S.Def && S.Def->Inds.size() >= 2) AddProfiledTrail(S, It, V);
        else AddRibbon(S, It, V);
    }
}

void Batch::AddProfiledTrail(const TrailStrip& S, Item It, const View& V)
{
    // its profile swept along it: each point's cross-section across its path (world up
    // above it, or X along an upright path), times its width; a segment's face drawn
    // where its normal looks at the eye, as the games cull
    float Depth = 0;
    const size_t N = S.Points.size();
    std::vector<XMVECTOR> Across(N), Above(N);
    for (size_t K = 0; K < N; K++)
    {
        const auto& A = S.Points[K == 0 ? 0 : K - 1].Pos;
        const auto& B = S.Points[std::min(N - 1, K + 1)].Pos;
        XMVECTOR Along = XMVectorSet(B[0] - A[0], B[1] - A[1], B[2] - A[2], 0);
        Along = XMVector3LengthSq(Along).m128_f32[0] < 1e-12f ? XMVectorSet(1, 0, 0, 0) : XMVector3Normalize(Along);
        const XMVECTOR Ref = std::fabs(XMVectorGetZ(Along)) < 0.95f ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(1, 0, 0, 0);
        Across[K] = XMVector3Normalize(XMVector3Cross(Along, Ref));
        Above[K] = XMVector3Cross(Across[K], Along);
        Depth += XMVectorGetX(XMVector3Dot(XMVectorSubtract(XMVectorSet(S.Points[K].Pos[0], S.Points[K].Pos[1], S.Points[K].Pos[2], 0), V.Eye), V.Forward));
    }
    const auto& Vs = S.Def->Verts;
    for (size_t K = 0; K + 1 < N; K++)
        for (size_t J = 0; J + 1 < S.Def->Inds.size(); J += 2)
        {
            const TrailDef::Vert& Va = Vs[S.Def->Inds[J]];
            const TrailDef::Vert& Vb = Vs[S.Def->Inds[J + 1]];
            const TrailStrip::Point& P0 = S.Points[K];
            const XMVECTOR At = XMVectorSet(P0.Pos[0], P0.Pos[1], P0.Pos[2], 0);
            const XMVECTOR Nrm = XMVectorAdd(XMVectorScale(Across[K], Va.Normal[0] + Vb.Normal[0]), XMVectorScale(Above[K], Va.Normal[1] + Vb.Normal[1]));
            if (XMVectorGetX(XMVector3Dot(Nrm, XMVectorSubtract(V.Eye, At))) < 0) continue;
            Vertex Q[4];
            for (int E = 0; E < 2; E++)
                for (int Sd = 0; Sd < 2; Sd++)
                {
                    const TrailStrip::Point& Pt = S.Points[K + E];
                    const TrailDef::Vert& Pv = Sd ? Vb : Va;
                    const XMVECTOR Pos = XMVectorAdd(XMVectorSet(Pt.Pos[0], Pt.Pos[1], Pt.Pos[2], 0),
                                                     XMVectorScale(XMVectorAdd(XMVectorScale(Across[K + E], Pv.Pos[0]), XMVectorScale(Above[K + E], Pv.Pos[1])), Pt.Width));
                    Vertex& Vx = Q[E * 2 + Sd];
                    XMFLOAT3 Fp;
                    XMStoreFloat3(&Fp, Pos);
                    Vx.Pos[0] = Fp.x; Vx.Pos[1] = Fp.y; Vx.Pos[2] = Fp.z;
                    Vx.Uv[0] = S.Uv[0] + S.Uv[2] * Pv.U;
                    Vx.Uv[1] = S.Uv[1] + S.Uv[3] * Pt.V;
                    const float Col[4] = { ToLinear(Pt.Color[0]), ToLinear(Pt.Color[1]), ToLinear(Pt.Color[2]), Pt.Color[3] };
                    std::copy(Col, Col + 4, Vx.Col);
                }
            for (int K2 : { 0, 1, 3, 0, 3, 2 }) Verts.push_back(Q[K2]);
        }
    It.Count = (uint32_t)Verts.size() - It.First;
    It.Depth = Depth / (float)N;
    if (It.Count) Items.push_back(It), Quads++;
}

void Batch::AddRibbon(const TrailStrip& S, Item It, const View& V)
{
    // a ribbon facing the camera: each point's sides across its path as the eye sees it
    float Depth = 0;
    for (size_t K = 0; K + 1 < S.Points.size(); K++)
    {
        Vertex Q[4];
        for (int E = 0; E < 2; E++)
        {
            const TrailStrip::Point& Pt = S.Points[K + E];
            const TrailStrip::Point& A = S.Points[K == 0 && E == 0 ? 0 : K + E - 1];
            const TrailStrip::Point& B = S.Points[std::min(S.Points.size() - 1, K + E + 1)];
            const XMVECTOR Pos = XMVectorSet(Pt.Pos[0], Pt.Pos[1], Pt.Pos[2], 0);
            const XMVECTOR Along = XMVectorSubtract(XMVectorSet(B.Pos[0], B.Pos[1], B.Pos[2], 0), XMVectorSet(A.Pos[0], A.Pos[1], A.Pos[2], 0));
            XMVECTOR Side = XMVector3Cross(Along, XMVectorSubtract(V.Eye, Pos));
            Side = XMVector3LengthSq(Side).m128_f32[0] < 1e-8f ? V.Right : XMVector3Normalize(Side);
            const float Col[4] = { ToLinear(Pt.Color[0]), ToLinear(Pt.Color[1]), ToLinear(Pt.Color[2]), Pt.Color[3] };
            for (int Sd = 0; Sd < 2; Sd++)
            {
                Vertex& Vx = Q[E * 2 + Sd];
                XMFLOAT3 Fp;
                XMStoreFloat3(&Fp, XMVectorAdd(Pos, XMVectorScale(Side, Sd ? Pt.Width : -Pt.Width)));
                Vx.Pos[0] = Fp.x; Vx.Pos[1] = Fp.y; Vx.Pos[2] = Fp.z;
                Vx.Uv[0] = S.Uv[0] + S.Uv[2] * Sd;
                Vx.Uv[1] = S.Uv[1] + S.Uv[3] * Pt.V;
                std::copy(Col, Col + 4, Vx.Col);
            }
            Depth += XMVectorGetX(XMVector3Dot(XMVectorSubtract(Pos, V.Eye), V.Forward));
        }
        for (int K2 : { 0, 1, 3, 0, 3, 2 }) Verts.push_back(Q[K2]);
    }
    It.Count = (uint32_t)Verts.size() - It.First;
    It.Depth = Depth / (2.0f * (S.Points.size() - 1));
    Items.push_back(It);
    Quads++;
}

void Batch::SortBackToFront()
{
    std::stable_sort(Items.begin(), Items.end(), [](const Item& A, const Item& B) { return A.Depth > B.Depth; });
}

// ---- the grid

std::vector<Vertex> GridLines(const DrawOptions& O, float Dist)
{
    std::vector<Vertex> Lines;
    float Step = O.GridStep;
    if (Step <= 0) Step = std::pow(10.0f, std::floor(std::log10(std::max(1.0f, Dist * 0.15f))));
    const int N = 20;
    const float Cx = std::round(O.GridCenter[0] / Step) * Step, Cy = std::round(O.GridCenter[1] / Step) * Step, Z = O.GridCenter[2];
    for (int K = -N; K <= N; K++)
    {
        const bool Axis = K == 0, Major = K % 5 == 0;
        const float A = Axis ? 0.45f : Major ? 0.22f : 0.08f;
        const float Gx[4] = { Axis ? 0.55f : 0.5f, 0.5f, 0.55f, A }, Gy[4] = { 0.5f, Axis ? 0.55f : 0.5f, 0.55f, A };
        const float X = Cx + K * Step, Y = Cy + K * Step;
        const Vertex V[4] = { { { X, Cy - N * Step, Z }, {}, {} }, { { X, Cy + N * Step, Z }, {}, {} },
                              { { Cx - N * Step, Y, Z }, {}, {} }, { { Cx + N * Step, Y, Z }, {}, {} } };
        for (int E = 0; E < 4; E++)
        {
            Vertex W = V[E];
            std::copy(E < 2 ? Gy : Gx, (E < 2 ? Gy : Gx) + 4, W.Col);
            for (int C = 0; C < 3; C++) W.Col[C] = ToLinear(W.Col[C]);
            Lines.push_back(W);
        }
    }
    return Lines;
}
}
