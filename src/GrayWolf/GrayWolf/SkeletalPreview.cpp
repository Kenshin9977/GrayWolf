#include "stdafx.h"

#include "SkeletalPreview.h"
#include "ModelPreview.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
    struct V3 { float x = 0, y = 0, z = 0; };
    struct Q4 { float x = 0, y = 0, z = 0, w = 1; };

    inline V3 Add(const V3& a, const V3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline V3 Sub(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline V3 Lerp(const V3& a, const V3& b, float t) { return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t }; }
    inline Q4 QConj(const Q4& q) { return { -q.x, -q.y, -q.z, q.w }; }
    inline float QDot(const Q4& a, const Q4& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

    // Hamilton product a (x) b: R(a(x)b) = R(a) . R(b), i.e. rotate by b first, then by a.
    inline Q4 QMul(const Q4& a, const Q4& b)
    {
        return {
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
        };
    }
    // v + 2*cross(u, cross(u,v) + w*v) - the standard quaternion-sandwich rotation, expanded.
    inline V3 QRot(const Q4& q, const V3& v)
    {
        const V3 u{ q.x, q.y, q.z };
        const V3 uv{ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
        const V3 t{ uv.x + q.w * v.x, uv.y + q.w * v.y, uv.z + q.w * v.z };
        const V3 uuvt{ u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x };
        return { v.x + 2 * uuvt.x, v.y + 2 * uuvt.y, v.z + 2 * uuvt.z };
    }
    inline Q4 QNorm(const Q4& q)
    {
        const float L = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return (L < 1e-8f) ? Q4{ 0, 0, 0, 1 } : Q4{ q.x / L, q.y / L, q.z / L, q.w / L };
    }
    inline Q4 QInv(const Q4& q) { return QConj(q); } // unit quaternion: inverse == conjugate
    inline Q4 QSlerp(const Q4& a, Q4 b, float t)
    {
        float D = QDot(a, b);
        if (D < 0) { b = { -b.x, -b.y, -b.z, -b.w }; D = -D; } // take the shorter arc
        if (D > 0.9995f) return QNorm({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t });
        const float Theta0 = std::acos(std::max(-1.0f, std::min(1.0f, D)));
        const float Theta = Theta0 * t;
        const float SinTheta0 = std::sin(Theta0);
        const float S0 = std::cos(Theta) - D * std::sin(Theta) / SinTheta0;
        const float S1 = std::sin(Theta) / SinTheta0;
        return { a.x * S0 + b.x * S1, a.y * S0 + b.y * S1, a.z * S0 + b.z * S1, a.w * S0 + b.w * S1 };
    }

    // Finds the two keys straddling FrameTime T (or the nearest single key when T is outside the
    // track's range) and the lerp/slerp parameter between them. A linear scan is fine: a preview only
    // evaluates a handful of keys per bone per displayed frame.
    template<class KeyT>
    void FindBracket(const std::vector<KeyT>& Keys, float T, size_t& I0, size_t& I1, float& Alpha)
    {
        if (Keys.size() == 1 || T <= (float)Keys.front().Frame) { I0 = I1 = 0; Alpha = 0; return; }
        if (T >= (float)Keys.back().Frame) { I0 = I1 = Keys.size() - 1; Alpha = 0; return; }
        for (size_t i = 0; i + 1 < Keys.size(); i++)
        {
            if (T >= (float)Keys[i].Frame && T <= (float)Keys[i + 1].Frame)
            {
                I0 = i; I1 = i + 1;
                const float Span = (float)Keys[i + 1].Frame - (float)Keys[i].Frame;
                Alpha = (Span > 0) ? (T - (float)Keys[i].Frame) / Span : 0.0f;
                return;
            }
        }
        I0 = I1 = Keys.size() - 1; Alpha = 0;
    }
}

void EvaluateSkeletonPose(const PreviewGeometry& Bind, const SeAnimClip& Clip, float FrameTime,
                          std::vector<SkeletonPoseBone>& OutWorldPose)
{
    const size_t N = Bind.Bones.size();
    OutWorldPose.assign(N, SkeletonPoseBone{});
    if (N == 0) return;

    // Bind-pose bones are stored GLOBAL (model-space); derive each one's LOCAL (parent-relative)
    // transform once, since Absolute/Relative/Additive/Delta are all defined in that local space.
    std::vector<V3> BindLocalPos(N); std::vector<Q4> BindLocalRot(N);
    for (size_t i = 0; i < N; i++)
    {
        const PreviewBone& B = Bind.Bones[i];
        const V3 WP{ B.Pos[0], B.Pos[1], B.Pos[2] };
        const Q4 WR = QNorm({ B.Rot[0], B.Rot[1], B.Rot[2], B.Rot[3] });
        if (B.Parent < 0 || (size_t)B.Parent >= N) { BindLocalPos[i] = WP; BindLocalRot[i] = WR; continue; }
        const PreviewBone& P = Bind.Bones[B.Parent];
        const Q4 PWRInv = QInv(QNorm({ P.Rot[0], P.Rot[1], P.Rot[2], P.Rot[3] }));
        BindLocalPos[i] = QRot(PWRInv, Sub(WP, V3{ P.Pos[0], P.Pos[1], P.Pos[2] }));
        BindLocalRot[i] = QNorm(QMul(PWRInv, WR));
    }

    std::unordered_map<std::string, size_t> TrackByName;
    TrackByName.reserve(Clip.Bones.size());
    for (size_t i = 0; i < Clip.Bones.size(); i++) TrackByName.emplace(Clip.Bones[i].Name, i);

    std::vector<V3> LocalPos(N); std::vector<Q4> LocalRot(N);
    for (size_t i = 0; i < N; i++)
    {
        LocalPos[i] = BindLocalPos[i];
        LocalRot[i] = BindLocalRot[i];

        const auto It = TrackByName.find(Bind.Bones[i].Name);
        if (It == TrackByName.end()) continue; // not animated - keep bind pose
        const SeAnimBoneTrack& Tr = Clip.Bones[It->second];
        // rotations are the bone's local rotation, but an additive one's, which is a change added to
        // the pose beneath (here the bind pose): taken whole, a near-identity delta folded every bone
        const SeAnimBoneType EffType = Tr.HasTypeOverride ? Tr.TypeOverride : Clip.Type;
        const bool IsRoot = (Bind.Bones[i].Parent < 0);

        if (!Tr.RotKeys.empty())
        {
            size_t i0, i1; float a;
            FindBracket(Tr.RotKeys, FrameTime, i0, i1, a);
            const Q4 R0 = QNorm({ Tr.RotKeys[i0].Value.x, Tr.RotKeys[i0].Value.y, Tr.RotKeys[i0].Value.z, Tr.RotKeys[i0].Value.w });
            const Q4 Keyed = (i0 == i1) ? R0 :
                QSlerp(R0, QNorm({ Tr.RotKeys[i1].Value.x, Tr.RotKeys[i1].Value.y, Tr.RotKeys[i1].Value.z, Tr.RotKeys[i1].Value.w }), a);
            LocalRot[i] = EffType == SeAnimBoneType::Additive ? QNorm(QMul(BindLocalRot[i], Keyed)) : Keyed;
        }
        if (!Tr.PosKeys.empty())
        {
            size_t i0, i1; float a;
            FindBracket(Tr.PosKeys, FrameTime, i0, i1, a);
            const V3 P0{ Tr.PosKeys[i0].Value.x, Tr.PosKeys[i0].Value.y, Tr.PosKeys[i0].Value.z };
            const V3 Keyed = (i0 == i1) ? P0 : Lerp(P0, V3{ Tr.PosKeys[i1].Value.x, Tr.PosKeys[i1].Value.y, Tr.PosKeys[i1].Value.z }, a);

            LocalPos[i] = (EffType == SeAnimBoneType::Absolute && !IsRoot) ? Keyed : Add(BindLocalPos[i], Keyed);
        }
    }

    // Compose world transforms parent-first - bones are assumed listed parent-before-child (true of
    // every .semodel/.XMODEL_EXPORT this reads, and SEAnim's tag list, sourced from the same model).
    for (size_t i = 0; i < N; i++)
    {
        const int32_t P = Bind.Bones[i].Parent;
        SkeletonPoseBone& Out = OutWorldPose[i];
        if (P < 0 || (size_t)P >= N)
        {
            Out.Pos[0] = LocalPos[i].x; Out.Pos[1] = LocalPos[i].y; Out.Pos[2] = LocalPos[i].z;
            Out.Rot[0] = LocalRot[i].x; Out.Rot[1] = LocalRot[i].y; Out.Rot[2] = LocalRot[i].z; Out.Rot[3] = LocalRot[i].w;
            continue;
        }
        const Q4 ParentWR{ OutWorldPose[P].Rot[0], OutWorldPose[P].Rot[1], OutWorldPose[P].Rot[2], OutWorldPose[P].Rot[3] };
        const V3 ParentWP{ OutWorldPose[P].Pos[0], OutWorldPose[P].Pos[1], OutWorldPose[P].Pos[2] };
        const V3 WP = Add(ParentWP, QRot(ParentWR, LocalPos[i]));
        const Q4 WR = QNorm(QMul(ParentWR, LocalRot[i]));
        Out.Pos[0] = WP.x; Out.Pos[1] = WP.y; Out.Pos[2] = WP.z;
        Out.Rot[0] = WR.x; Out.Rot[1] = WR.y; Out.Rot[2] = WR.z; Out.Rot[3] = WR.w;
    }
}

void SkinPreviewGeometry(const PreviewGeometry& Bind, const std::vector<SkeletonPoseBone>& WorldPose,
                          std::vector<float>& OutPos, std::vector<float>& OutNrm)
{
    OutPos = Bind.Pos; OutNrm = Bind.Nrm; // pass-through default for unweighted vertices / early-outs
    const size_t N = Bind.Bones.size();
    if (Bind.VtxBoneIdx.empty() || WorldPose.size() != N || N == 0) return;

    // Per-bone skin transform = WorldPose[bone] . inverse(BindWorld[bone]), kept as a (rotation,
    // translation) pair since every transform here is rigid (no scale):
    //   Skin(p) = CurWR*(BindWRInv*(p-BindWP)) + CurWP = (CurWR*BindWRInv)*p + (CurWP - (CurWR*BindWRInv)*BindWP)
    std::vector<Q4> SkinRot(N); std::vector<V3> SkinPos(N);
    for (size_t b = 0; b < N; b++)
    {
        const Q4 BindWRInv = QInv(QNorm({ Bind.Bones[b].Rot[0], Bind.Bones[b].Rot[1], Bind.Bones[b].Rot[2], Bind.Bones[b].Rot[3] }));
        const V3 BindWP{ Bind.Bones[b].Pos[0], Bind.Bones[b].Pos[1], Bind.Bones[b].Pos[2] };
        const Q4 CurWR{ WorldPose[b].Rot[0], WorldPose[b].Rot[1], WorldPose[b].Rot[2], WorldPose[b].Rot[3] };
        const V3 CurWP{ WorldPose[b].Pos[0], WorldPose[b].Pos[1], WorldPose[b].Pos[2] };
        SkinRot[b] = QNorm(QMul(CurWR, BindWRInv));
        SkinPos[b] = Sub(CurWP, QRot(SkinRot[b], BindWP));
    }

    const int MaxInf = PREVIEW_MAX_BONE_INFLUENCES;
    const size_t VertCount = Bind.Pos.size() / 3;
    for (size_t v = 0; v < VertCount; v++)
    {
        const V3 BP{ Bind.Pos[v * 3], Bind.Pos[v * 3 + 1], Bind.Pos[v * 3 + 2] };
        const V3 BN{ Bind.Nrm[v * 3], Bind.Nrm[v * 3 + 1], Bind.Nrm[v * 3 + 2] };
        float WSum = 0; V3 AccP{}, AccN{};
        for (int k = 0; k < MaxInf; k++)
        {
            const size_t Slot = v * (size_t)MaxInf + k;
            if (Slot >= Bind.VtxBoneIdx.size()) break;
            const int32_t Bi = Bind.VtxBoneIdx[Slot];
            const float W = Bind.VtxBoneWeight[Slot];
            if (Bi < 0 || (size_t)Bi >= N || W <= 0.0f) continue;
            const V3 Pp = Add(QRot(SkinRot[Bi], BP), SkinPos[Bi]);
            const V3 Np = QRot(SkinRot[Bi], BN);
            AccP.x += Pp.x * W; AccP.y += Pp.y * W; AccP.z += Pp.z * W;
            AccN.x += Np.x * W; AccN.y += Np.y * W; AccN.z += Np.z * W;
            WSum += W;
        }
        if (WSum > 1e-6f)
        {
            OutPos[v*3] = AccP.x / WSum; OutPos[v*3+1] = AccP.y / WSum; OutPos[v*3+2] = AccP.z / WSum;
            const float NLen = std::sqrt(AccN.x*AccN.x + AccN.y*AccN.y + AccN.z*AccN.z);
            if (NLen > 1e-6f) { OutNrm[v*3] = AccN.x/NLen; OutNrm[v*3+1] = AccN.y/NLen; OutNrm[v*3+2] = AccN.z/NLen; }
        }
        // else: bind-pose pass-through already sits in OutPos/OutNrm from the copy above.
    }
}

float PoseStretch(const PreviewGeometry& Bind, const std::vector<float>& Posed)
{
    const size_t N = Bind.Pos.size() / 3, Tris = Bind.Idx.size() / 3;
    if (Posed.size() != Bind.Pos.size() || Tris == 0) return 0.0f;
    const auto Len2 = [](const float* P, uint32_t A, uint32_t B) {
        const float X = P[A * 3] - P[B * 3], Y = P[A * 3 + 1] - P[B * 3 + 1], Z = P[A * 3 + 2] - P[B * 3 + 2];
        return X * X + Y * Y + Z * Z;
    };
    const size_t Step = std::max<size_t>(1, Tris / 50000);
    size_t Edges = 0, Torn = 0;
    for (size_t t = 0; t < Tris; t += Step)
        for (int e = 0; e < 3; e++)
        {
            const uint32_t A = Bind.Idx[t * 3 + e], B = Bind.Idx[t * 3 + (e + 1) % 3];
            if (A >= N || B >= N) continue;
            const float Before = Len2(Bind.Pos.data(), A, B);
            if (Before < 1e-6f) continue;
            Edges++;
            Torn += Len2(Posed.data(), A, B) > 9.0f * Before;
        }
    return Edges ? (float)Torn / (float)Edges : 0.0f;
}
