#include "stdafx.h"

#include "MapRadiantTextures.h"

#include "ExportNaming.h"
#include "MapRadiantGeometry.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace Maps
{
namespace
{
    using V3 = DrawnWorld::V3;
    using namespace RadiantGeometry;

    constexpr double Tolerance = 0.05;    // a drawn vertex this close to a side's plane lies on it (units)
    constexpr double Aligned = 0.9995;    // a drawn triangle's normal this close to the side's (cosine)
    constexpr double Cell = 256.0;        // the grid's cell (units)
    constexpr double Whole = 0.999;       // a triangle this much inside a face is covered whole
    constexpr double Split = 0.05;        // a second material over this share of the drawn area: a split face
    constexpr double MaxUvError = 1.0 / 2048;  // a fit missing a drawn UV by more (repeats: half a texel of a 1024 image) leaves the side a tool

    // a cell of the grid for the triangles facing along Facing (0..5: +x, -x, +y, -y, +z, -z)
    uint64_t Key(int Facing, int64_t X, int64_t Y, int64_t Z)
    {
        constexpr int64_t Bias = 1 << 19;
        return ((uint64_t)Facing << 60) | ((uint64_t)(X + Bias) << 40) | ((uint64_t)(Y + Bias) << 20) | (uint64_t)(Z + Bias);
    }
    int64_t CellOf(double V) { return (int64_t)std::floor(V / Cell); }
    int FacingOf(const V3& N)
    {
        int A = 0;
        for (int K = 1; K < 3; K++)
            if (std::fabs(N[K]) > std::fabs(N[A])) A = K;
        return A * 2 + (N[A] < 0 ? 1 : 0);
    }

    constexpr double OnEdge = 1e-6;       // a point this close to a face's edge is on it, when a triangle is clipped to the face

    // as the .map writes a number (four decimals)
    double Written(double V) { return std::round(V * 1e4) / 1e4; }

    // how far a UV is from another, whole repeats aside
    double Off(double A, double B)
    {
        const double D = A - B;
        return std::fabs(D - std::round(D));
    }

    // Quake 3's TextureAxisFromPlane: the axes (index of s, of t; t points down the axis) of the
    // first base plane whose normal is closest to N
    void BaseAxes(const V3& N, int& Sv, int& Tv)
    {
        static const double Normals[6][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
        static const int S[6] = { 0, 0, 1, 1, 0, 0 }, T[6] = { 1, 1, 2, 2, 2, 2 };
        double Best = 0;
        int Bi = 0;
        for (int I = 0; I < 6; I++)
        {
            const double D = N[0] * Normals[I][0] + N[1] * Normals[I][1] + N[2] * Normals[I][2];
            if (D > Best) Best = D, Bi = I;
        }
        Sv = S[Bi];
        Tv = T[Bi];
    }

    // sine and cosine as Quake 3 takes them (exact at quarter turns)
    void SinCos(double Degrees, double& S, double& C)
    {
        if (Degrees == 0) S = 0, C = 1;
        else if (Degrees == 90) S = 1, C = 0;
        else if (Degrees == 180 || Degrees == -180) S = 0, C = -1;
        else if (Degrees == 270 || Degrees == -90) S = -1, C = 0;
        else
        {
            const double R = Degrees * 3.14159265358979323846 / 180.0;
            S = std::sin(R), C = std::cos(R);
        }
    }
}

bool RadiantTool(const std::string& Lower)
{
    for (const char* Tool : { "caulk", "clip", "nodraw", "trigger", "mantle", "ladder", "sky", "hint", "portal", "nosight" })
        if (Lower.rfind(Tool, 0) == 0) return true;
    return Lower.size() > 5 && Lower.compare(Lower.size() - 5, 5, "_clip") == 0;   // metal_clip
}

void RadiantBaseAxes(const V3& N, V3& S, V3& T)
{
    int Sv = 0, Tv = 1;
    BaseAxes(N, Sv, Tv);
    S = { 0, 0, 0 }, T = { 0, 0, 0 };
    S[Sv] = 1, T[Tv] = -1;
}

uint32_t DrawnWorld::Owner(const std::string& MeshName)
{
    return MeshName.compare(0, 11, "brushmodel_") == 0 ? (uint32_t)std::strtoul(MeshName.c_str() + 11, nullptr, 10) : 0;
}

DrawnWorld::DrawnWorld(const MapScene& Scene, RadiantTextureStats& Stats)
{
    std::map<std::string, uint32_t> Index;
    Gone.resize(Scene.Objects.size());
    for (size_t O = 0; O < Scene.Objects.size(); O++)
    {
        const Mesh& M = Scene.Objects[O];
        Gone[O].resize(M.Surfaces.size());
        const uint32_t Whose = Owner(M.Name);
        for (size_t Si = 0; Si < M.Surfaces.size(); Si++)
        {
            const Surface& S = M.Surfaces[Si];
            Gone[O][Si].assign(S.Indices.size() / 3, 0);
            if (S.Material.empty() || RadiantTool(ExportNaming::Lower(S.Material))) continue;
            const auto [It, New] = Index.emplace(S.Material, (uint32_t)Names.size());
            if (New) Names.push_back(S.Material);
            for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
            {
                Tri T{};
                V3 Normals = { 0, 0, 0 };
                bool Valid = true, White = true;
                for (int J = 0; J < 3; J++)
                {
                    const uint32_t I = S.Indices[K + J];
                    if (I >= S.Vertices.size())
                    {
                        Valid = false;
                        break;
                    }
                    const Vertex& V = S.Vertices[I];
                    T.P[J] = { V.Pos[0], V.Pos[1], V.Pos[2] };
                    T.Uv[J][0] = V.Uv[0], T.Uv[J][1] = V.Uv[1];
                    for (int A = 0; A < 3; A++) Normals[A] += V.Normal[A];
                    for (const uint8_t Channel : V.Color) White = White && Channel == 255;
                }
                if (!Valid) continue;
                if (!White)
                {
                    Stats.TrianglesColoured++;
                    continue;
                }
                const V3 C = Cross(Sub(T.P[1], T.P[0]), Sub(T.P[2], T.P[0]));
                const double L = Length(C);
                if (L < 1e-6) continue;
                T.N = Mul(C, (Dot(C, Normals) < 0 ? -1.0 : 1.0) / L);
                T.Area = L / 2;
                T.Material = It->second;
                T.Object = (uint32_t)O, T.Surf = (uint32_t)Si, T.K = (uint32_t)(K / 3), T.Owner = Whose;
                const uint32_t Number = (uint32_t)Tris.size();
                Tris.push_back(T);
                int64_t Lo[3], Hi[3];
                for (int A = 0; A < 3; A++)
                {
                    Lo[A] = CellOf((std::min)({ T.P[0][A], T.P[1][A], T.P[2][A] }) - Tolerance);
                    Hi[A] = CellOf((std::max)({ T.P[0][A], T.P[1][A], T.P[2][A] }) + Tolerance);
                }
                for (int64_t X = Lo[0]; X <= Hi[0]; X++)
                    for (int64_t Y = Lo[1]; Y <= Hi[1]; Y++)
                        for (int64_t Z = Lo[2]; Z <= Hi[2]; Z++) Grid.emplace_back(Key(FacingOf(T.N), X, Y, Z), Number);
            }
        }
    }
    std::sort(Grid.begin(), Grid.end());
    for (size_t I = 0; I < Grid.size();)
    {
        size_t J = I;
        while (J < Grid.size() && Grid[J].first == Grid[I].first) J++;
        Cells.emplace(Grid[I].first, std::make_pair((uint32_t)I, (uint32_t)J));
        I = J;
    }
    Stamp.assign(Tris.size(), 0);
}

bool DrawnWorld::Removed(size_t Object, size_t Surf, size_t K) const
{
    return Object < Gone.size() && Surf < Gone[Object].size() && K < Gone[Object][Surf].size() && Gone[Object][Surf][K];
}

bool DrawnWorld::Texture(const std::vector<V3>& Face, const V3& N, double D, const V3& WrittenN, uint32_t Whose, const HiddenTest* Hidden,
                         SideTexture& Out, RadiantTextureStats& Stats)
{
    if (Face.size() < 3 || Tris.empty()) return false;
    // the triangles on the face's plane overlapping it: their area inside it, whole or not
    struct Hit
    {
        uint32_t Tri;
        double Inside;
        bool Whole;
    };
    std::vector<Hit> Hits;
    if (++Query == 0)
    {
        std::fill(Stamp.begin(), Stamp.end(), 0);
        Query = 1;
    }
    int64_t Lo[3], Hi[3];
    for (int A = 0; A < 3; A++)
    {
        double Min = Face[0][A], Max = Face[0][A];
        for (const V3& P : Face) Min = (std::min)(Min, P[A]), Max = (std::max)(Max, P[A]);
        Lo[A] = CellOf(Min - Tolerance), Hi[A] = CellOf(Max + Tolerance);
    }
    // the face's edges as planes, the inside behind them
    std::vector<std::pair<V3, double>> Edges;
    for (size_t K = 0; K < Face.size(); K++)
    {
        const V3 E = Sub(Face[(K + 1) % Face.size()], Face[K]);
        V3 Outward = Cross(E, N);   // the face counter-clockwise about N: away from it
        const double L = Length(Outward);
        if (L < 1e-9) continue;
        Outward = Mul(Outward, 1.0 / L);
        Edges.emplace_back(Outward, Dot(Outward, Face[K]));
    }
    const double FaceArea = Area(Face);
    if (FaceArea <= 0) return false;
    const auto Look = [&](uint32_t I) {
        if (Stamp[I] == Query) return;
        Stamp[I] = Query;
        const Tri& T = Tris[I];
        if (T.Owner != Whose || Dot(T.N, N) < Aligned) return;
        double Far = 0;
        for (const V3& P : T.P) Far = (std::max)(Far, std::fabs(Dot(N, P) - D));
        if (Far > Tolerance) return;
        std::vector<V3> W;
        for (const V3& P : T.P) W.push_back(Sub(P, Mul(N, Dot(N, P) - D)));
        const double Full = Area(W);
        for (const auto& [En, Ed] : Edges)
            if ((W = Clip(W, En, Ed, OnEdge)).size() < 3) return;
        const double In = Area(W);
        if (In <= 1e-3 * Full || In <= 1e-4) return;   // an edge shared, a sliver
        Hits.push_back({ I, In, In >= Whole * Full });
        Stats.Distance[Far == 0 ? 0 : Far < 1e-3 ? 1 : Far < 1e-2 ? 2 : 3]++;
    };
    // the facings a triangle within Aligned of N may have: an axis N is nearly as close to as its
    // closest (two normals Aligned apart differ by at most 0.032 per component)
    const double Closest = (std::max)({ std::fabs(N[0]), std::fabs(N[1]), std::fabs(N[2]) });
    for (int A = 0; A < 3; A++)
    {
        if (std::fabs(N[A]) < Closest - 0.07) continue;
        const int Facing = A * 2 + (N[A] < 0 ? 1 : 0);
        for (int64_t X = Lo[0]; X <= Hi[0]; X++)
            for (int64_t Y = Lo[1]; Y <= Hi[1]; Y++)
                for (int64_t Z = Lo[2]; Z <= Hi[2]; Z++)
                {
                    const auto It = Cells.find(Key(Facing, X, Y, Z));
                    if (It == Cells.end()) continue;
                    for (uint32_t G = It->second.first; G < It->second.second; G++) Look(Grid[G].second);
                }
    }
    if (Hits.empty()) return false;

    // the material covering most of the face
    std::map<uint32_t, double> ByMaterial;
    double Drawn = 0;
    for (const Hit& H : Hits) ByMaterial[Tris[H.Tri].Material] += H.Inside, Drawn += H.Inside;
    uint32_t Material = 0;
    double Most = -1, Second = 0;
    for (const auto& [M, A] : ByMaterial)
        if (A > Most) Second = (std::max)(Second, Most), Most = A, Material = M;
        else Second = (std::max)(Second, A);

    // the projection: Quake 3's base axes for the plane Radiant reads; each triangle's own map
    // (u = A x + B y + U, v = C x + E y + V on those axes), the triangles grouped by map (several
    // brushes' faces, a patch, on one plane), the group covering most of the face fitted
    int Sv = 0, Tv = 1;
    BaseAxes(WrittenN, Sv, Tv);
    V3 Centre = { 0, 0, 0 };
    for (const V3& P : Face) Centre = { Centre[0] + P[0], Centre[1] + P[1], Centre[2] + P[2] };
    Centre = Mul(Centre, 1.0 / (double)Face.size());
    struct Map
    {
        double A, B, C, E, U, V;   // U, V: at the face's centre, whole repeats aside
        double Area;
    };
    std::vector<Map> Groups;
    std::vector<int> GroupOf(Hits.size(), -1);
    std::vector<size_t> Order;
    for (size_t I = 0; I < Hits.size(); I++)
        if (Tris[Hits[I].Tri].Material == Material) Order.push_back(I);
    std::sort(Order.begin(), Order.end(), [&](size_t X, size_t Y) { return Hits[X].Inside > Hits[Y].Inside; });
    const auto Frac = [](double V) { return V - std::floor(V); };
    for (const size_t I : Order)
    {
        const Tri& T = Tris[Hits[I].Tri];
        const double X1 = T.P[1][Sv] - T.P[0][Sv], Y1 = T.P[1][Tv] - T.P[0][Tv];
        const double X2 = T.P[2][Sv] - T.P[0][Sv], Y2 = T.P[2][Tv] - T.P[0][Tv];
        const double Det = X1 * Y2 - X2 * Y1;
        if (std::fabs(Det) < 1e-9) continue;
        const double U1 = T.Uv[1][0] - T.Uv[0][0], U2 = T.Uv[2][0] - T.Uv[0][0];
        const double V1 = T.Uv[1][1] - T.Uv[0][1], V2 = T.Uv[2][1] - T.Uv[0][1];
        Map M;
        M.A = (U1 * Y2 - U2 * Y1) / Det, M.B = (X1 * U2 - X2 * U1) / Det;
        M.C = (V1 * Y2 - V2 * Y1) / Det, M.E = (X1 * V2 - X2 * V1) / Det;
        const double Dx = Centre[Sv] - T.P[0][Sv], Dy = Centre[Tv] - T.P[0][Tv];
        M.U = Frac(T.Uv[0][0] + M.A * Dx + M.B * Dy), M.V = Frac(T.Uv[0][1] + M.C * Dx + M.E * Dy);
        M.Area = Hits[I].Inside;
        const auto Same = [&M](const Map& G) {
            const double Su = std::fabs(G.A) + std::fabs(G.B), St = std::fabs(G.C) + std::fabs(G.E);
            return std::fabs(G.A - M.A) + std::fabs(G.B - M.B) <= 1e-3 * Su && std::fabs(G.C - M.C) + std::fabs(G.E - M.E) <= 1e-3 * St &&
                   Off(G.U, M.U) <= 2e-3 && Off(G.V, M.V) <= 2e-3;
        };
        size_t G = 0;
        while (G < Groups.size() && !Same(Groups[G])) G++;
        if (G == Groups.size()) Groups.push_back(M);
        else Groups[G].Area += M.Area;
        GroupOf[I] = (int)G;
    }
    if (Groups.empty())
    {
        Stats.SidesNoFit++;
        return false;
    }
    int Main = 0;
    double MaterialArea = 0;
    for (size_t G = 0; G < Groups.size(); G++)
    {
        MaterialArea += Groups[G].Area;
        if (Groups[G].Area > Groups[Main].Area) Main = (int)G;
    }
    const auto InMain = [&](size_t I) { return GroupOf[I] == Main; };

    // (A, B, C, E) by least squares over the group's edges
    double Mxx = 0, Mxy = 0, Myy = 0, Ru[2] = { 0, 0 }, Rv[2] = { 0, 0 };
    for (size_t I = 0; I < Hits.size(); I++)
    {
        if (!InMain(I)) continue;
        const Tri& T = Tris[Hits[I].Tri];
        for (int J = 1; J < 3; J++)
        {
            const double Dx = T.P[J][Sv] - T.P[0][Sv], Dy = T.P[J][Tv] - T.P[0][Tv];
            const double Du = T.Uv[J][0] - T.Uv[0][0], Dv = T.Uv[J][1] - T.Uv[0][1];
            Mxx += Dx * Dx, Mxy += Dx * Dy, Myy += Dy * Dy;
            Ru[0] += Dx * Du, Ru[1] += Dy * Du, Rv[0] += Dx * Dv, Rv[1] += Dy * Dv;
        }
    }
    const double Det = Mxx * Myy - Mxy * Mxy;
    if (std::fabs(Det) <= 1e-12 * (std::max)(1.0, Mxx * Myy))
    {
        Stats.SidesNoFit++;
        return false;
    }
    const double A = (Ru[0] * Myy - Ru[1] * Mxy) / Det, B = (Ru[1] * Mxx - Ru[0] * Mxy) / Det;
    const double C = (Rv[0] * Myy - Rv[1] * Mxy) / Det, E = (Rv[1] * Mxx - Rv[0] * Mxy) / Det;
    // u = (c x + s y - Sx) / W: (A, B) = (c, s) / W; v = (s x - c y - Sy) / H: (C, E) = (s, -c) / H
    const double InvW = std::hypot(A, B);
    double Angle = std::atan2(B, A) * 180.0 / 3.14159265358979323846;
    double Sn = 0, Cs = 0;
    SinCos(Angle, Sn, Cs);
    const double InvH = Sn * C - Cs * E;
    if (InvW < 1e-9 || std::fabs(InvH) < 1e-9)
    {
        Stats.SidesNoFit++;
        return false;
    }
    double W = 1.0 / InvW, Hh = 1.0 / InvH;
    // the angle within (-90, 90], a half turn being both sizes negated
    if (Angle > 90) Angle -= 180, W = -W, Hh = -Hh;
    else if (Angle <= -90) Angle += 180, W = -W, Hh = -Hh;
    if (std::fabs(Angle - std::round(Angle)) < 1e-3) Angle = std::round(Angle);
    if (std::fabs(W - std::round(W)) < 1e-3) W = std::round(W);
    if (std::fabs(Hh - std::round(Hh)) < 1e-3) Hh = std::round(Hh);
    Angle = Written(Angle), W = Written(W), Hh = Written(Hh);
    if (Angle == 0) Angle = 0;   // no -0
    SinCos(Angle, Sn, Cs);

    // the shifts: the mean (on the circle of one repeat) of what each vertex asks
    double Su[2] = { 0, 0 }, St[2] = { 0, 0 };
    constexpr double Turn = 2 * 3.14159265358979323846;
    for (size_t I = 0; I < Hits.size(); I++)
    {
        if (!InMain(I)) continue;
        const Tri& T = Tris[Hits[I].Tri];
        for (int J = 0; J < 3; J++)
        {
            const double X = T.P[J][Sv], Y = T.P[J][Tv];
            const double Pu = (Cs * X + Sn * Y) / W - T.Uv[J][0], Pv = (Sn * X - Cs * Y) / Hh - T.Uv[J][1];
            Su[0] += std::cos(Turn * Pu), Su[1] += std::sin(Turn * Pu);
            St[0] += std::cos(Turn * Pv), St[1] += std::sin(Turn * Pv);
        }
    }
    const auto Shift = [&](const double* Sum, double Size) {
        double F = std::atan2(Sum[1], Sum[0]) / Turn;   // in repeats
        F -= std::floor(F);
        double S = Written(F * std::fabs(Size));
        if (S >= std::fabs(Size)) S -= std::fabs(Size);
        return S * (Size < 0 ? -1.0 : 1.0);
    };
    const double Sx = Shift(Su, W), Sy = Shift(St, Hh);

    // the error, as written
    double Error = 0;
    for (size_t I = 0; I < Hits.size(); I++)
    {
        if (!InMain(I)) continue;
        const Tri& T = Tris[Hits[I].Tri];
        for (int J = 0; J < 3; J++)
        {
            const double X = T.P[J][Sv], Y = T.P[J][Tv];
            Error = (std::max)({ Error, Off((Cs * X + Sn * Y - Sx) / W, T.Uv[J][0]), Off((Sn * X - Cs * Y - Sy) / Hh, T.Uv[J][1]) });
        }
    }

    (Error < 1e-4 ? Stats.UvExact : Error < 1e-3 ? Stats.UvBelow1e3 : Error < 1e-2 ? Stats.UvBelow1e2 : Error < 1e-1 ? Stats.UvBelow1e1 : Stats.UvAbove)++;
    Stats.UvErrorSum += Error;
    if (Error > MaxUvError)
    {
        Stats.SidesUvMissed++;
        return false;
    }

    // a face the fitted triangles draw in part: the rest should be hidden by other brushes. Over
    // 5% of it neither drawn so nor hidden (cut by the compiler as outside the level, by a brush
    // the clip map does not keep, drawn by other triangles: another material, a patch on it)
    // would be drawn by the side where the game draws something else or nothing: left a tool,
    // its triangles patches.
    if (Hidden && Groups[Main].Area < 0.98 * FaceArea)
    {
        const V3 U = Mul(Sub(Face[1], Face[0]), 1.0 / Length(Sub(Face[1], Face[0])));
        const V3 Vv = Cross(N, U);
        double Lo2[2] = { 1e300, 1e300 }, Hi2[2] = { -1e300, -1e300 };
        for (const V3& P : Face)
        {
            const double C[2] = { Dot(Sub(P, Face[0]), U), Dot(Sub(P, Face[0]), Vv) };
            for (int A = 0; A < 2; A++) Lo2[A] = (std::min)(Lo2[A], C[A]), Hi2[A] = (std::max)(Hi2[A], C[A]);
        }
        const double Step = (std::max)(std::sqrt(FaceArea) / 12.0, 0.5);
        const auto Inside = [&](const V3& P, const V3* W, size_t Count) {
            int Sign = 0;
            for (size_t K = 0; K < Count; K++)
            {
                const double S = Dot(Cross(Sub(W[(K + 1) % Count], W[K]), Sub(P, W[K])), N);
                if (std::fabs(S) < 1e-9) continue;
                if (Sign == 0) Sign = S > 0 ? 1 : -1;
                else if ((S > 0) != (Sign > 0)) return false;
            }
            return true;
        };
        size_t Samples = 0, Undrawn = 0;
        for (double A = Lo2[0] + Step / 2; A < Hi2[0]; A += Step)
            for (double B = Lo2[1] + Step / 2; B < Hi2[1]; B += Step)
            {
                const V3 P = { Face[0][0] + U[0] * A + Vv[0] * B, Face[0][1] + U[1] * A + Vv[1] * B, Face[0][2] + U[2] * A + Vv[2] * B };
                if (!Inside(P, Face.data(), Face.size())) continue;
                Samples++;
                bool Covered = false;
                for (size_t I = 0; I < Hits.size() && !Covered; I++)
                {
                    if (!InMain(I)) continue;
                    const Tri& T = Tris[Hits[I].Tri];
                    V3 W[3];
                    for (int J = 0; J < 3; J++) W[J] = Sub(T.P[J], Mul(N, Dot(N, T.P[J]) - D));
                    Covered = Inside(P, W, 3);
                }
                if (!Covered && !(*Hidden)(V3{ P[0] + N[0] * 0.5, P[1] + N[1] * 0.5, P[2] + N[2] * 0.5 })) Undrawn++;
            }
        if (Samples && Undrawn > 0.05 * Samples)
        {
            Stats.SidesUndrawn++;
            return false;
        }
    }

    // the group's triangles inside the face, drawn by it
    for (size_t I = 0; I < Hits.size(); I++)
    {
        if (!InMain(I) || !Hits[I].Whole) continue;
        const Tri& T = Tris[Hits[I].Tri];
        char& G = Gone[T.Object][T.Surf][T.K];
        if (!G) Stats.TrianglesRemoved++;
        G = 1;
    }
    if (MaterialArea - Groups[Main].Area > Split * MaterialArea) Stats.SidesMixedMaps++;
    Out.Material = Names[Material];
    Out.Width = W, Out.Height = Hh, Out.ShiftX = Sx, Out.ShiftY = Sy, Out.Rotation = Angle, Out.UvError = Error;
    Stats.SidesTextured++;
    const double Covered = Groups[Main].Area / FaceArea;
    Stats.Coverage[Covered < 0.1 ? 0 : Covered < 0.5 ? 1 : Covered < 0.9 ? 2 : 3]++;
    if (Second > Split * Drawn) Stats.SidesSplit++;
    return true;
}
}
