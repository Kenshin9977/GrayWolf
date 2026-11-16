#include "stdafx.h"

#include "MapRadiantPlanes.h"

#include "MapRadiantWriter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace Maps
{
namespace
{
    // SnapVector, then the distance to a whole number within 0.001 (cod2map's FindFloatPlane)
    CompilerPlane Snapped(CompilerPlane P)
    {
        for (int I = 0; I < 3; I++)
            if (std::fabs((double)P.N[I] - 1.0) < 1e-7 || std::fabs((double)P.N[I] + 1.0) < 1e-7)
            {
                const float S = P.N[I] > 0 ? 1.0f : -1.0f;
                P.N[0] = P.N[1] = P.N[2] = 0.0f;
                P.N[I] = S;
                break;
            }
        const float R = std::round(P.D);
        if (std::fabs((double)P.D - (double)R) < 0.001) P.D = R;
        return P;
    }
    bool Same(const CompilerPlane& A, const CompilerPlane& B, float Sign = 1.0f)
    {
        for (int I = 0; I < 3; I++)
            if (std::fabs((double)A.N[I] - Sign * (double)B.N[I]) >= 1e-7) return false;
        return std::fabs((double)A.D - Sign * (double)B.D) < 0.001;
    }
    bool Axial(const CompilerPlane& P)
    {
        return std::fabs(P.N[0]) == 1.0f || std::fabs(P.N[1]) == 1.0f || std::fabs(P.N[2]) == 1.0f;
    }
    int64_t Bucket(float D) { return (int64_t)std::floor(std::fabs((double)D) * 8.0); }

    // the bevels cod2map adds to a brush of these sides (Sides snapped)
    CompilerBrush Beveled(std::vector<CompilerPlane> Sides)
    {
        CompilerBrush R;
        R.Sides = std::move(Sides);
        Brush B;
        for (const CompilerPlane& P : R.Sides)
        {
            BrushSide S;
            for (int I = 0; I < 3; I++) S.Normal[I] = P.N[I];
            S.Dist = P.D;
            B.Sides.push_back(S);
        }
        const std::vector<BrushFace> Faces = BrushFaces(B);
        if (Faces.empty()) return R;
        double Lo[3] = { 1e30, 1e30, 1e30 }, Hi[3] = { -1e30, -1e30, -1e30 };
        std::vector<std::array<float, 3>> Corners;
        for (const BrushFace& F : Faces)
            for (const auto& V : F.Points)
            {
                Corners.push_back({ (float)V[0], (float)V[1], (float)V[2] });
                for (int A = 0; A < 3; A++) Lo[A] = (std::min)(Lo[A], (double)(float)V[A]), Hi[A] = (std::max)(Hi[A], (double)(float)V[A]);
            }
        const auto Has = [&](const CompilerPlane& P) {
            for (const CompilerPlane& S : R.Sides)
                if (Same(S, P)) return true;
            for (const CompilerPlane& S : R.Bevels)
                if (Same(S, P)) return true;
            return false;
        };
        // an axial plane at the bounds for each axis and direction no side faces
        for (int A = 0; A < 3; A++)
            for (int Sign = -1; Sign <= 1; Sign += 2)
            {
                bool Faced = false;
                for (const CompilerPlane& S : R.Sides) Faced = Faced || S.N[A] == (float)Sign;
                if (Faced) continue;
                CompilerPlane P;
                P.N[0] = P.N[1] = P.N[2] = 0.0f;
                P.N[A] = (float)Sign;
                P.D = Sign > 0 ? (float)Hi[A] : (float)-Lo[A];
                R.Bevels.push_back(Snapped(P));
                R.BoxBevels++;
            }
        // the upright planes through the edges of the sides not axial
        for (const BrushFace& F : Faces)
        {
            if (Axial(R.Sides[F.Side])) continue;
            const size_t Count = F.Points.size();
            for (size_t K = 0; K < Count; K++)
            {
                const auto& A = F.Points[(K + Count - 1) % Count];
                const auto& C = F.Points[K];
                float V[3] = { (float)C[0] - (float)A[0], (float)C[1] - (float)A[1], (float)C[2] - (float)A[2] };
                const float L = std::sqrt(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]);
                if (L < 0.5f) continue;
                for (float& X : V) X /= L;
                bool AxialEdge = false;
                for (const float X : V) AxialEdge = AxialEdge || std::fabs((double)std::fabs(X) - 1.0) < 1e-7;
                if (AxialEdge || V[2] > 0.866f || V[2] < -0.866f) continue;
                const float H = std::sqrt(V[0] * V[0] + V[1] * V[1]);
                for (const float Way : { 1.0f, -1.0f })
                {
                    CompilerPlane P;
                    P.N[0] = -V[1] / H * Way, P.N[1] = V[0] / H * Way, P.N[2] = 0.0f;
                    P.D = P.N[0] * (float)A[0] + P.N[1] * (float)A[1] + P.N[2] * (float)A[2];
                    P = Snapped(P);
                    bool Behind = true;
                    for (const auto& Q : Corners)
                        if (P.N[0] * Q[0] + P.N[1] * Q[1] + P.N[2] * Q[2] - P.D > 0.5f)
                        {
                            Behind = false;
                            break;
                        }
                    // a side lying on it (each corner within 0.5) stops it too
                    for (const BrushFace& G : Faces)
                    {
                        bool On = true;
                        for (const auto& Q : G.Points)
                            if (std::fabs(P.N[0] * (float)Q[0] + P.N[1] * (float)Q[1] + P.N[2] * (float)Q[2] - P.D) > 0.5f)
                            {
                                On = false;
                                break;
                            }
                        Behind = Behind && !On;
                    }
                    if (!Behind || Has(P)) continue;
                    R.Bevels.push_back(P);
                    R.EdgeBevels++;
                }
            }
        }
        return R;
    }
}

size_t PlaneTable::Find(const CompilerPlane& In)
{
    const CompilerPlane P = Snapped(In);
    const int64_t K = Bucket(P.D);
    for (int64_t B = K - 1; B <= K + 1; B++)
        if (const auto It = Buckets.find(B); It != Buckets.end())
            for (const size_t S : It->second)
                if (Same(Planes[S], P)) return S;
    const size_t Slot = Planes.size();
    CompilerPlane Flip = P;
    for (float& X : Flip.N) X = -X;
    Flip.D = -P.D;
    Planes.push_back(P);
    Planes.push_back(Flip);
    Buckets[K].push_back(Slot);
    Buckets[Bucket(Flip.D)].push_back(Slot + 1);
    return Slot;
}

CompilerBrush CompilerPlanes(const std::vector<std::array<std::array<float, 3>, 3>>& Points)
{
    std::vector<CompilerPlane> Sides;
    for (const auto& Pt : Points)
    {
        float A[3], B[3];
        for (int I = 0; I < 3; I++) A[I] = Pt[0][I] - Pt[1][I], B[I] = Pt[2][I] - Pt[1][I];
        CompilerPlane P;
        P.N[0] = A[1] * B[2] - A[2] * B[1];
        P.N[1] = A[2] * B[0] - A[0] * B[2];
        P.N[2] = A[0] * B[1] - A[1] * B[0];
        const float L = std::sqrt(P.N[0] * P.N[0] + P.N[1] * P.N[1] + P.N[2] * P.N[2]);
        if (!(L > 0.0f)) continue;
        for (float& X : P.N) X /= L;
        P.D = Pt[0][0] * P.N[0] + Pt[0][1] * P.N[1] + Pt[0][2] * P.N[2];
        Sides.push_back(Snapped(P));
    }
    return Beveled(std::move(Sides));
}

CompilerBrush CompilerPlanes(const Brush& B)
{
    std::vector<CompilerPlane> Sides;
    for (const BrushSide& S : B.Sides)
    {
        const double L = std::sqrt((double)S.Normal[0] * S.Normal[0] + (double)S.Normal[1] * S.Normal[1] + (double)S.Normal[2] * S.Normal[2]);
        if (!(L > 0.5)) continue;
        CompilerPlane P;
        for (int I = 0; I < 3; I++) P.N[I] = (float)(S.Normal[I] / L);
        P.D = (float)(S.Dist / L);
        Sides.push_back(Snapped(P));
    }
    return Beveled(std::move(Sides));
}

PlaneCount CountPlanes(const std::vector<const CompilerBrush*>& Brushes, const std::vector<CompilerPlane>& Again)
{
    PlaneCount C;
    PlaneTable T;
    std::vector<char> SideSlot;
    for (const CompilerBrush* B : Brushes)
    {
        if (!B || B->Sides.empty()) continue;
        C.Brushes++;
        C.Sides += B->Sides.size();
        C.BoxBevels += B->BoxBevels;
        C.EdgeBevels += B->EdgeBevels;
        C.CollisionSides += B->EdgeBevels;
        for (const CompilerPlane& P : B->Sides)
        {
            const size_t S = T.Find(P);
            if (S >= SideSlot.size()) SideSlot.resize(S + 2, 0);
            SideSlot[S] = 1;
            C.CollisionSides += !Axial(P);
        }
        for (const CompilerPlane& P : B->Bevels) T.Find(P);
    }
    for (const CompilerPlane& P : Again) T.Find(P);
    C.Slots = T.Slots();
    for (const char U : SideSlot) C.SidePlanes += U;
    return C;
}

PlaneCount CountPlanes(const std::string& MapText)
{
    // a brush's side: "( x y z ) ( x y z ) ( x y z ) material ..." at depth 2 (entity, brush)
    std::vector<CompilerBrush> All;
    std::vector<std::array<std::array<float, 3>, 3>> Points;
    // an entity standing at an origin: cod2map finds each of its brushes' planes again, moved there
    // (0x1400a45c0: d - n . origin), sides and bevels
    std::vector<CompilerPlane> Again;
    size_t First = 0;   // the entity's first brush in All
    bool Placed = false;
    float Origin[3] = { 0, 0, 0 };
    int Depth = 0;
    bool Patch = false;
    size_t At = 0;
    while (At < MapText.size())
    {
        size_t End = MapText.find('\n', At);
        if (End == std::string::npos) End = MapText.size();
        size_t S = At;
        while (S < End && (MapText[S] == ' ' || MapText[S] == '\t')) S++;
        size_t E = End;
        while (E > S && (MapText[E - 1] == '\r' || MapText[E - 1] == ' ')) E--;
        const size_t Len = E - S;
        if (Len == 1 && MapText[S] == '{')
        {
            if (++Depth == 2) Points.clear(), Patch = false;
            else if (Depth == 1) First = All.size(), Placed = false;
        }
        else if (Len == 1 && MapText[S] == '}')
        {
            if (Depth == 2 && !Patch && !Points.empty()) All.push_back(CompilerPlanes(Points));
            if (Depth == 1 && Placed)
                for (size_t B = First; B < All.size(); B++)
                    for (const auto* List : { &All[B].Sides, &All[B].Bevels })
                        for (CompilerPlane P : *List)
                        {
                            P.D -= P.N[0] * Origin[0] + P.N[1] * Origin[1] + P.N[2] * Origin[2];
                            Again.push_back(P);
                        }
            Depth--;
        }
        else if (Depth == 1 && MapText.compare(S, 10, "\"origin\" \"") == 0)
        {
            const char* C = MapText.c_str() + S + 10;
            Placed = true;
            for (float& V : Origin)
            {
                char* Next = nullptr;
                V = (float)std::strtod(C, &Next);
                Placed = Placed && Next != C;
                C = Next;
            }
        }
        else if (Depth == 2 && Len && MapText[S] != '(' && MapText[S] != '/' && MapText[S] != '"')
            Patch = true;   // "mesh", "curve"
        else if (Depth == 2 && !Patch && Len && MapText[S] == '(')
        {
            std::array<std::array<float, 3>, 3> P;
            const char* C = MapText.c_str() + S;
            bool Ok = true;
            for (int J = 0; J < 3 && Ok; J++)
            {
                while (*C == ' ' || *C == '(') C++;
                for (int K = 0; K < 3 && Ok; K++)
                {
                    char* Next = nullptr;
                    P[J][K] = (float)std::strtod(C, &Next);
                    Ok = Next != C;
                    C = Next;
                }
                while (*C == ' ') C++;
                Ok = Ok && *C == ')';
                C++;
            }
            if (Ok) Points.push_back(P);
        }
        At = End + 1;
    }
    std::vector<const CompilerBrush*> Of;
    for (const CompilerBrush& B : All) Of.push_back(&B);
    return CountPlanes(Of, Again);
}
}
