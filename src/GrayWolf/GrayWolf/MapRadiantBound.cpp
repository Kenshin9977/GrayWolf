#include "stdafx.h"

#include "MapRadiantBound.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace Maps
{
namespace
{
    // a mesh of a brush model's ("brushmodel_<n>"): moved with its entity, never cut
    bool BrushModelMesh(const Mesh& M) { return M.Name.compare(0, 11, "brushmodel_") == 0; }

    bool Finite(const double* P) { return std::isfinite(P[0]) && std::isfinite(P[1]) && std::isfinite(P[2]); }
    bool InWorld(const double* P, double Limit) { return std::fabs(P[0]) <= Limit && std::fabs(P[1]) <= Limit && std::fabs(P[2]) <= Limit; }

    // how far P lies beyond the box Lo..Hi (the most it passes a side by; 0 within)
    double Beyond(const double (&Lo)[3], const double (&Hi)[3], const double* P)
    {
        double D = 0;
        for (int A = 0; A < 3; A++) D = (std::max)({ D, Lo[A] - P[A], P[A] - Hi[A] });
        return D;
    }

    // Each element of the level: its points (a triangle's corners, a placement's or an entity's
    // origin), finite ones only
    using Visitor = std::function<void(const double (*P)[3], int Count)>;
    void Elements(const MapScene& Scene, const std::vector<Entity>& Ents, const Visitor& Visit)
    {
        double P[3][3];
        const auto& Cv = Scene.Collision.Vertices;
        const auto& Ct = Scene.Collision.Triangles;
        for (size_t T = 0; T + 2 < Ct.size(); T += 3)
        {
            bool Ok = true;
            for (int J = 0; J < 3; J++)
            {
                const size_t I = (size_t)Ct[T + J] * 3;
                if (I + 2 >= Cv.size())
                {
                    Ok = false;
                    break;
                }
                for (int A = 0; A < 3; A++) P[J][A] = Cv[I + A];
                Ok = Ok && Finite(P[J]);
            }
            if (Ok) Visit(P, 3);
        }
        for (const Mesh& M : Scene.Objects)
        {
            if (BrushModelMesh(M)) continue;
            for (const Surface& S : M.Surfaces)
                for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                {
                    bool Ok = true;
                    for (int J = 0; J < 3 && Ok; J++)
                    {
                        if (S.Indices[K + J] >= S.Vertices.size())
                        {
                            Ok = false;
                            break;
                        }
                        for (int A = 0; A < 3; A++) P[J][A] = S.Vertices[S.Indices[K + J]].Pos[A];
                        Ok = Finite(P[J]);
                    }
                    if (Ok) Visit(P, 3);
                }
        }
        for (const auto* List : { &Scene.StaticInstances, &Scene.DynamicInstances, &Scene.SplinedInstances })
            for (const Instance& I : *List)
            {
                for (int A = 0; A < 3; A++) P[0][A] = I.Origin[A];
                if (Finite(P[0])) Visit(P, 1);
            }
        for (const Entity& E : Ents)
        {
            const auto Class = E.find("classname");
            float O[3];
            if ((Class != E.end() && Class->second == "worldspawn") || !EntityFloats(E, "origin", O, 3)) continue;
            for (int A = 0; A < 3; A++) P[0][A] = O[A];
            if (Finite(P[0])) Visit(P, 1);
        }
    }

    // ---- clipping a triangle to the box

    struct Corner
    {
        Vertex V;
        std::vector<float> More;   // its further UV sets (Surface::MoreUvs)
    };

    Corner Lerp(const Corner& A, const Corner& B, double T)
    {
        Corner C;
        const auto L = [T](double X, double Y) { return (float)(X + (Y - X) * T); };
        for (int K = 0; K < 3; K++) C.V.Pos[K] = L(A.V.Pos[K], B.V.Pos[K]), C.V.Normal[K] = L(A.V.Normal[K], B.V.Normal[K]);
        const double N = std::sqrt((double)C.V.Normal[0] * C.V.Normal[0] + (double)C.V.Normal[1] * C.V.Normal[1] + (double)C.V.Normal[2] * C.V.Normal[2]);
        if (N > 1e-12)
            for (float& X : C.V.Normal) X = (float)(X / N);
        for (int K = 0; K < 2; K++) C.V.Uv[K] = L(A.V.Uv[K], B.V.Uv[K]);
        for (int K = 0; K < 4; K++) C.V.Color[K] = (uint8_t)std::lround(std::clamp(A.V.Color[K] + (B.V.Color[K] - A.V.Color[K]) * T, 0.0, 255.0));
        C.More.resize(A.More.size());
        for (size_t K = 0; K < A.More.size() && K < B.More.size(); K++) C.More[K] = L(A.More[K], B.More[K]);
        return C;
    }

    // Sutherland-Hodgman against the box's six planes
    std::vector<Corner> Clip(std::vector<Corner> Poly, const double (&Lo)[3], const double (&Hi)[3])
    {
        for (int A = 0; A < 3 && !Poly.empty(); A++)
            for (int Far = 0; Far < 2 && !Poly.empty(); Far++)
            {
                // the signed distance inside the plane (>= 0: kept)
                const auto In = [&](const Corner& C) { return Far ? Hi[A] - C.V.Pos[A] : C.V.Pos[A] - Lo[A]; };
                std::vector<Corner> Next;
                for (size_t I = 0; I < Poly.size(); I++)
                {
                    const Corner& P = Poly[I];
                    const Corner& Q = Poly[(I + 1) % Poly.size()];
                    const double Dp = In(P), Dq = In(Q);
                    if (Dp >= 0) Next.push_back(P);
                    if ((Dp >= 0) != (Dq >= 0))
                    {
                        Corner C = Lerp(P, Q, Dp / (Dp - Dq));
                        C.V.Pos[A] = (float)(Far ? Hi[A] : Lo[A]);   // on the plane exactly
                        Next.push_back(std::move(C));
                    }
                }
                Poly = std::move(Next);
            }
        return Poly.size() >= 3 ? Poly : std::vector<Corner>{};
    }

    // what becomes of a triangle: kept whole, cut off (outside, or nothing left once clipped),
    // clipped (the box lies within the target's world: what passed it is clipped too); Left: a
    // corner not a number, the writer's to judge as before
    enum class Cut { Whole, Gone, Clipped, Left };
    Cut Judge(const SkyBound& B, const double (*P)[3])
    {
        bool All = true;
        for (int J = 0; J < 3; J++)
        {
            if (!Finite(P[J])) return Cut::Left;
            All = All && B.Holds(P[J]);
        }
        if (All) return Cut::Whole;
        for (int A = 0; A < 3; A++)
            if ((P[0][A] < B.Lo[A] && P[1][A] < B.Lo[A] && P[2][A] < B.Lo[A]) || (P[0][A] > B.Hi[A] && P[1][A] > B.Hi[A] && P[2][A] > B.Hi[A]))
                return Cut::Gone;
        return Cut::Clipped;
    }
}

bool SkyBound::Holds(const double* P) const
{
    constexpr double Slack = 1e-3;
    for (int A = 0; A < 3; A++)
        if (!(P[A] >= Lo[A] - Slack && P[A] <= Hi[A] + Slack)) return false;
    return true;
}

bool SkyBound::Holds(const float* P) const
{
    const double D[3] = { P[0], P[1], P[2] };
    return Holds(D);
}

SkyBound UsefulBound(const MapScene& Scene, const std::vector<Entity>& Ents, bool Starts, const double (&StartLo)[3],
                     const double (&StartHi)[3], double Limit)
{
    SkyBound B;
    double Lo[3], Hi[3];
    if (Starts)
    {
        B.FromStarts = true;
        for (int A = 0; A < 3; A++) Lo[A] = StartLo[A], Hi[A] = StartHi[A];
    }
    else
    {
        // the median of the triangles' first corners, axis by axis
        std::vector<double> Axis[3];
        Elements(Scene, Ents, [&](const double (*P)[3], int Count) {
            if (Count == 3)
                for (int A = 0; A < 3; A++) Axis[A].push_back(P[0][A]);
        });
        if (Axis[0].empty()) return B;
        for (int A = 0; A < 3; A++)
        {
            std::nth_element(Axis[A].begin(), Axis[A].begin() + Axis[A].size() / 2, Axis[A].end());
            Lo[A] = Hi[A] = Axis[A][Axis[A].size() / 2];
        }
    }
    // each element's distance beyond the reference (its farthest point), sorted; kept out to the gap
    std::vector<double> Far;
    Elements(Scene, Ents, [&](const double (*P)[3], int Count) {
        double D = 0;
        for (int J = 0; J < Count; J++) D = (std::max)(D, Beyond(Lo, Hi, P[J]));
        Far.push_back(D);
    });
    if (Far.empty()) return B;
    std::sort(Far.begin(), Far.end());
    B.Reach = Far.back();
    for (size_t I = 1; I < Far.size(); I++)
        if (Far[I] > SkyBoundGapFactor * (std::max)(Far[I - 1], SkyBoundFloor))
        {
            B.Reach = Far[I - 1], B.Next = Far[I];
            break;
        }
    // the box of what is kept, within the world
    for (int A = 0; A < 3; A++) B.Lo[A] = 1e300, B.Hi[A] = -1e300;
    Elements(Scene, Ents, [&](const double (*P)[3], int Count) {
        double D = 0;
        for (int J = 0; J < Count; J++) D = (std::max)(D, Beyond(Lo, Hi, P[J]));
        if (D > B.Reach) return;
        for (int J = 0; J < Count; J++)
            if (InWorld(P[J], Limit))
                for (int A = 0; A < 3; A++) B.Lo[A] = (std::min)(B.Lo[A], P[J][A]), B.Hi[A] = (std::max)(B.Hi[A], P[J][A]);
    });
    B.Cuts = B.Next > 0 && B.Lo[0] <= B.Hi[0];
    // a little past what is kept, so that an edge clipped to the box never lies on one of the
    // level's faces: BO1 mp_firingrange's water clipped onto its sky brushes' faces stopped
    // cod2map ("WindingAreaWithKnownNormal( wInner, plane ) <= 0", tris_mergeconcave.cpp)
    if (B.Cuts)
        for (int A = 0; A < 3; A++) B.Lo[A] = (std::max)(B.Lo[A] - SkyBoundClipMargin, -Limit), B.Hi[A] = (std::min)(B.Hi[A] + SkyBoundClipMargin, Limit);
    return B;
}

void CutToBound(const MapScene& Scene, const SkyBound& Bound, BoundedWorld& Out)
{
    // the drawn world: a triangle outside left out, one across clipped (a fan of the polygon left)
    Out.Objects.reserve(Scene.Objects.size());
    for (const Mesh& M : Scene.Objects)
    {
        if (BrushModelMesh(M))
        {
            Out.Objects.push_back(M);
            continue;
        }
        Mesh& To = Out.Objects.emplace_back();
        To.Name = M.Name;
        for (const Surface& S : M.Surfaces)
        {
            Surface& T = To.Surfaces.emplace_back();
            T.Material = S.Material;
            T.Vertices = S.Vertices;
            T.UvSets = S.UvSets;
            T.MoreUvs = S.MoreUvs;
            T.Indices.reserve(S.Indices.size());
            const size_t More = S.UvSets > 1 ? (size_t)(S.UvSets - 1) * 2 : 0;
            const bool HasMore = More && S.MoreUvs.size() >= S.Vertices.size() * More;
            const auto Of = [&](uint32_t I) {
                Corner C{ S.Vertices[I], {} };
                if (HasMore) C.More.assign(S.MoreUvs.begin() + (ptrdiff_t)(I * More), S.MoreUvs.begin() + (ptrdiff_t)((I + 1) * More));
                return C;
            };
            for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
            {
                const uint32_t Tri[3] = { S.Indices[K], S.Indices[K + 1], S.Indices[K + 2] };
                if (Tri[0] >= S.Vertices.size() || Tri[1] >= S.Vertices.size() || Tri[2] >= S.Vertices.size())
                {
                    T.Indices.insert(T.Indices.end(), Tri, Tri + 3);   // the writer's to skip, as before
                    continue;
                }
                double P[3][3];
                for (int J = 0; J < 3; J++)
                    for (int A = 0; A < 3; A++) P[J][A] = S.Vertices[Tri[J]].Pos[A];
                const Cut C = Judge(Bound, P);
                if (C == Cut::Whole || C == Cut::Left)
                {
                    T.Indices.insert(T.Indices.end(), Tri, Tri + 3);
                    continue;
                }
                const std::vector<Corner> Poly = C == Cut::Gone ? std::vector<Corner>{} : Clip({ Of(Tri[0]), Of(Tri[1]), Of(Tri[2]) }, Bound.Lo, Bound.Hi);
                if (Poly.empty())
                {
                    Out.TrianglesCut++;
                    continue;
                }
                Out.TrianglesClipped++;
                const uint32_t First = (uint32_t)T.Vertices.size();
                for (const Corner& Q : Poly)
                {
                    T.Vertices.push_back(Q.V);
                    if (HasMore) T.MoreUvs.insert(T.MoreUvs.end(), Q.More.begin(), Q.More.end());
                }
                for (uint32_t J = 1; J + 1 < (uint32_t)Poly.size(); J++) T.Indices.insert(T.Indices.end(), { First, First + J, First + J + 1 });
            }
            if (!HasMore && More) T.MoreUvs = S.MoreUvs;   // as it came (short)
            if (T.Indices.size() == S.Indices.size() && T.Vertices.size() == S.Vertices.size()) continue;
            // the vertices no triangle uses any more left out (the sky box takes in every vertex)
            std::vector<uint32_t> Remap(T.Vertices.size(), UINT32_MAX);
            std::vector<Vertex> Used;
            std::vector<float> UsedMore;
            for (uint32_t& I : T.Indices)
            {
                if (I >= T.Vertices.size())
                {
                    I = UINT32_MAX;   // still out of range
                    continue;
                }
                if (Remap[I] == UINT32_MAX)
                {
                    Remap[I] = (uint32_t)Used.size();
                    Used.push_back(T.Vertices[I]);
                    if (HasMore) UsedMore.insert(UsedMore.end(), T.MoreUvs.begin() + (ptrdiff_t)(I * More), T.MoreUvs.begin() + (ptrdiff_t)((I + 1) * More));
                }
                I = Remap[I];
            }
            T.Vertices = std::move(Used);
            if (HasMore) T.MoreUvs = std::move(UsedMore);
        }
    }
    // the collision triangles the same way
    Out.CollisionVertices = Scene.Collision.Vertices;
    const auto& Ct = Scene.Collision.Triangles;
    Out.CollisionTriangles.reserve(Ct.size());
    for (size_t K = 0; K + 2 < Ct.size(); K += 3)
    {
        const uint32_t Tri[3] = { Ct[K], Ct[K + 1], Ct[K + 2] };
        if ((size_t)Tri[0] * 3 + 2 >= Scene.Collision.Vertices.size() || (size_t)Tri[1] * 3 + 2 >= Scene.Collision.Vertices.size() ||
            (size_t)Tri[2] * 3 + 2 >= Scene.Collision.Vertices.size())
        {
            Out.CollisionTriangles.insert(Out.CollisionTriangles.end(), Tri, Tri + 3);
            continue;
        }
        double P[3][3];
        Corner In[3];
        for (int J = 0; J < 3; J++)
            for (int A = 0; A < 3; A++) P[J][A] = In[J].V.Pos[A] = Scene.Collision.Vertices[(size_t)Tri[J] * 3 + A];
        const Cut C = Judge(Bound, P);
        if (C == Cut::Whole || C == Cut::Left)
        {
            Out.CollisionTriangles.insert(Out.CollisionTriangles.end(), Tri, Tri + 3);
            continue;
        }
        const std::vector<Corner> Poly = C == Cut::Gone ? std::vector<Corner>{} : Clip({ In[0], In[1], In[2] }, Bound.Lo, Bound.Hi);
        if (Poly.empty())
        {
            Out.CollisionCut++;
            continue;
        }
        Out.CollisionClipped++;
        const uint32_t First = (uint32_t)(Out.CollisionVertices.size() / 3);
        for (const Corner& Q : Poly) Out.CollisionVertices.insert(Out.CollisionVertices.end(), { Q.V.Pos[0], Q.V.Pos[1], Q.V.Pos[2] });
        for (uint32_t J = 1; J + 1 < (uint32_t)Poly.size(); J++) Out.CollisionTriangles.insert(Out.CollisionTriangles.end(), { First, First + J, First + J + 1 });
    }
    if (Out.CollisionCut + Out.CollisionClipped)
    {
        // the vertices no triangle uses any more left out, as above
        std::vector<uint32_t> Remap(Out.CollisionVertices.size() / 3, UINT32_MAX);
        std::vector<float> Used;
        for (uint32_t& I : Out.CollisionTriangles)
        {
            if (I >= Remap.size())
            {
                I = UINT32_MAX;
                continue;
            }
            if (Remap[I] == UINT32_MAX)
            {
                Remap[I] = (uint32_t)(Used.size() / 3);
                Used.insert(Used.end(), Out.CollisionVertices.begin() + (ptrdiff_t)I * 3, Out.CollisionVertices.begin() + (ptrdiff_t)I * 3 + 3);
            }
            I = Remap[I];
        }
        Out.CollisionVertices = std::move(Used);
    }
    // the placements standing outside left out (a model cannot be clipped)
    const std::pair<const std::vector<Instance>*, std::vector<Instance>*> Lists[] = { { &Scene.StaticInstances, &Out.StaticInstances },
                                                                                       { &Scene.DynamicInstances, &Out.DynamicInstances },
                                                                                       { &Scene.SplinedInstances, &Out.SplinedInstances } };
    for (const auto& [From, To] : Lists)
        for (const Instance& I : *From)
        {
            const double O[3] = { I.Origin[0], I.Origin[1], I.Origin[2] };
            if (Finite(O) && !Bound.Holds(O)) Out.PlacementsCut++;
            else To->push_back(I);
        }
}
}
