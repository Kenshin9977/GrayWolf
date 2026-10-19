#include "stdafx.h"

#include "MapRadiantWriter.h"

#include "ExportNaming.h"
#include "GdtWriter.h"
#include "MapLive.h"
#include "MapMaterials.h"
#include "MapRadiantGeometry.h"
#include "MapRadiantTextures.h"
#include "MaterialGdt.h"
#include "MapSceneWriter.h"
#include "WraithModel.h"
#include "XMEExport.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace fs = std::filesystem;

namespace Maps
{
namespace
{
    using namespace RadiantGeometry;

    constexpr double OnPlane = 1e-5;     // a point this close to a plane is on it (the planes are exact)
    constexpr double Huge = 262144.0;    // the half size of a side's starting polygon

    struct Plane
    {
        V3 N;
        double D;
    };

    // Q3's BaseWindingForPlane: a square on the plane larger than any level
    std::vector<V3> BaseWinding(const Plane& P)
    {
        int Major = 0;
        for (int K = 1; K < 3; K++)
            if (std::fabs(P.N[K]) > std::fabs(P.N[Major])) Major = K;
        V3 Up = Major == 2 ? V3{ 1, 0, 0 } : V3{ 0, 0, 1 };
        Up = Sub(Up, Mul(P.N, Dot(Up, P.N)));
        Up = Mul(Up, 1.0 / Length(Up));
        const V3 Right = Cross(Up, P.N);
        const V3 Org = Mul(P.N, P.D);
        const V3 U = Mul(Up, Huge), R = Mul(Right, Huge);
        return { Add(Sub(Org, R), U), Add(Add(Org, R), U), Sub(Add(Org, R), U), Sub(Sub(Org, R), U) };
    }

    // ---- the text

    std::string Num(double V)
    {
        if (std::fabs(V - std::round(V)) < 1e-9) return std::to_string((long long)std::llround(V));
        char Buf[64];
        std::snprintf(Buf, sizeof(Buf), "%.4f", V);
        std::string S = Buf;
        while (!S.empty() && S.back() == '0') S.pop_back();
        if (!S.empty() && S.back() == '.') S.pop_back();
        return S == "-0" ? "0" : S;
    }

    // Three points of a face (Quake's order: (p0 - p1) x (p2 - p1) points out), as written (Num:
    // four decimals): three of its corners, snapped to whole units when that keeps the plane, else
    // as they are, else (a sliver of a face, whose corners give its plane badly) three points far
    // apart on the plane. Kept: the plane they give passes within 0.0005 of every corner; with
    // Brush (the brush's corners), its distance to each of them within 0.0005 of the true plane's
    // (a sliver's plane tilted by its corners' rounding moves the corners of its neighbours), the
    // points closest to that when none does.
    double Written(double V) { return std::round(V * 1e4) / 1e4; }

    std::array<V3, 3> ThreePoints(const BrushFace& F, const Plane& P, const std::vector<V3>* Brush = nullptr)
    {
        const auto& W = F.Points;
        const auto Out = [&P](std::array<V3, 3> Pts) {
            if (Dot(Cross(Sub(Pts[0], Pts[1]), Sub(Pts[2], Pts[1])), P.N) < 0) std::swap(Pts[0], Pts[2]);
            return Pts;
        };
        // how far the plane the points give strays from the true one (-1: not that plane)
        const auto Strays = [&W, &P, Brush](const std::array<V3, 3>& Pts) {
            V3 N = Cross(Sub(Pts[0], Pts[1]), Sub(Pts[2], Pts[1]));
            const double L = Length(N);
            if (L <= 0) return -1.0;
            N = Mul(N, 1.0 / L);
            if (Dot(N, P.N) < (Brush ? 0.0 : 0.99)) return -1.0;
            const double D = Dot(N, Pts[1]);
            double Most = 0;
            for (const V3& V : Brush ? *Brush : W) Most = (std::max)(Most, std::fabs(Dot(N, V) - D - (Brush ? Dot(P.N, V) - P.D : 0.0)));
            return Most;
        };
        const auto Keeps = [&Strays](const std::array<V3, 3>& Pts) {
            const double S = Strays(Pts);
            return S >= 0 && S <= 0.0005;
        };
        const V3 A = W[0];
        size_t Bi = 1;
        for (size_t K = 1; K < W.size(); K++)
            if (Length(Sub(W[K], A)) > Length(Sub(W[Bi], A))) Bi = K;
        const V3 B = W[Bi];
        const V3 Dir = Mul(Sub(B, A), 1.0 / Length(Sub(B, A)));
        size_t Ci = 0;
        double Best = -1;
        for (size_t K = 0; K < W.size(); K++)
        {
            const V3 D = Sub(W[K], A);
            const double Off = Length(Sub(D, Mul(Dir, Dot(D, Dir))));
            if (Off > Best) Best = Off, Ci = K;
        }
        const std::array<V3, 3> Corners = Out({ A, B, W[Ci] });
        for (const double Snap : { 0.002, 0.0 })
        {
            std::array<V3, 3> Pts = Corners;
            for (auto& Pt : Pts)
                for (auto& C : Pt) C = std::fabs(C - std::round(C)) < Snap ? std::round(C) : Written(C);
            if (Keeps(Pts)) return Pts;
        }
        // far apart on the plane, around the face's centre
        V3 Centre = { 0, 0, 0 };
        for (const V3& V : W) Centre = Add(Centre, Mul(V, 1.0 / (double)W.size()));
        std::array<V3, 3> Closest{};
        double Least = -1;
        for (const double Far : { 4096.0, 65536.0 })
        {
            const V3 U = Mul(Dir, Far), Vv = Mul(Cross(P.N, Dir), Far);
            std::array<V3, 3> Pts = Out({ Add(Centre, U), Centre, Add(Centre, Vv) });
            for (auto& Pt : Pts)
                for (auto& C : Pt) C = Written(C);
            if (!Brush) return Pts;
            const double S = Strays(Pts);
            if (S >= 0 && S <= 0.0005) return Pts;
            if (Least < 0 || (S >= 0 && S < Least)) Closest = Pts, Least = S;
        }
        return Closest;
    }

    // The written brush as a reader of the .map gets it back (radiant_check's test): each side's
    // plane from its three points as written, its face that plane clipped by the others (a point
    // within 0.001 of a plane on it); sound when every side bounds a face, the faces close and
    // enclose a volume
    bool Sound(const std::vector<std::array<V3, 3>>& Sides)
    {
        constexpr double Eps = 0.001;
        std::vector<Plane> Planes;
        for (const auto& Pts : Sides)
        {
            std::array<V3, 3> Read;
            for (int K = 0; K < 3; K++)
                for (int C = 0; C < 3; C++) Read[K][C] = std::strtod(Num(Pts[K][C]).c_str(), nullptr);
            V3 N = Cross(Sub(Read[0], Read[1]), Sub(Read[2], Read[1]));
            const double L = Length(N);
            if (L <= 0) return false;
            N = Mul(N, 1.0 / L);
            Planes.push_back({ N, Dot(N, Read[1]) });
        }
        std::vector<std::vector<V3>> Polys;
        V3 Centre = { 0, 0, 0 };
        size_t Corners = 0;
        for (size_t I = 0; I < Planes.size(); I++)
        {
            std::vector<V3> W = BaseWinding(Planes[I]);
            for (size_t J = 0; J < Planes.size() && W.size() >= 3; J++)
                if (J != I) W = Clip(W, Planes[J].N, Planes[J].D, Eps);
            if (W.size() < 3) return false;
            for (const V3& V : W) Centre = Add(Centre, V);
            Corners += W.size();
            Polys.push_back(std::move(W));
        }
        Centre = Mul(Centre, 1.0 / (double)Corners);
        V3 Total = { 0, 0, 0 };
        double Surface = 0, Volume = 0;
        for (size_t I = 0; I < Polys.size(); I++)
        {
            V3 A = { 0, 0, 0 };
            double Vol = 0;
            const auto& W = Polys[I];
            for (size_t K = 1; K + 1 < W.size(); K++)
            {
                const V3 C = Cross(Sub(W[K], W[0]), Sub(W[K + 1], W[0]));
                A = Add(A, Mul(C, 0.5));
                Vol += Dot(Sub(W[0], Centre), C) / 6.0;
            }
            // each face counter-clockwise seen from outside
            if (Dot(A, Planes[I].N) < 0) A = Mul(A, -1), Vol = -Vol;
            Total = Add(Total, A);
            Surface += Length(A);
            Volume += Vol;
        }
        return Length(Total) <= 1e-3 * (std::max)(Surface, 1.0) && Volume > 0;
    }

    bool Starts(const std::string& S, const char* Prefix) { return S.rfind(Prefix, 0) == 0; }

    // The tool material a brush is written with: a side's own when the clip map names one per side
    // and it is a tool material (IW), else the one Black Ops III's mod tools give the brush's
    // content flags (Quake's lineage, the same bits from CoD4 to Black Ops). A drawn surface's
    // brush (Black Ops: bullets and missiles, 0x2080; CoD4: solid with a drawn material) stood for
    // geometry now drawn by a misc_model: clip_full, which blocks everything and seals nothing.
    class ToolNames
    {
    public:
        explicit ToolNames(const Collision& C)
        {
            for (const auto& [Name, Contents] : C.Materials)
                if (Sky.empty() && Starts(ExportNaming::Lower(Name), "sky")) Sky = ExportNaming::Lower(Name);
            if (Sky.empty()) Sky = "sky";
        }
        std::string Of(const Brush& B, const BrushSide& S) const
        {
            const std::string M = ExportNaming::Lower(S.Material);
            if (RadiantTool(M)) return M;
            constexpr uint32_t Solid = 0x1, Glass = 0x10, CanShootClip = 0x40, MissileClip = 0x80, VehicleClip = 0x200,
                               ItemClip = 0x400, SkyFlag = 0x800, NoSight = 0x1000, ClipShot = 0x2000, PlayerClip = 0x10000,
                               MonsterClip = 0x20000, Mantle = 0x1000000, Detail = 0x8000000, Structural = 0x10000000,
                               Trigger = 0x40000000;
            const uint32_t C = B.Contents & ~Detail;
            if (C & Trigger) return "trigger";
            if (C & SkyFlag) return Sky;
            if (C & Solid) return M.empty() || RadiantTool(M) ? "caulk" : "clip_full";
            const bool Player = C & PlayerClip, Monster = C & MonsterClip;
            if (Player && Monster) return C & NoSight ? "clip_nosight" : "clip";
            if (Player) return "clip_player";
            if (Monster) return "clip_ai";
            if ((C & ClipShot) && (C & MissileClip)) return "clip_full";
            if (C & Mantle) return "mantle_on";
            if (C & Glass) return "clip";
            if (C & VehicleClip) return "clip_vehicle";
            if (C & ItemClip) return "clip_physics";
            if (C & NoSight) return "nosight_noclip";
            if (C & ClipShot) return "clip_weapon";
            if (C & MissileClip) return "clip_missile";
            if (C & CanShootClip) return "clip_weapon";
            if (C & Structural) return "hint";
            return "clip";
        }

    private:
        std::string Sky;
    };

    // A drawn material's name in the .map: as the model files name it (its GDT asset name with a
    // GDT, which then lists it)
    std::string MaterialName(const std::string& Material, RadiantGdt* Gdt)
    {
        if (!Gdt) return Material.empty() ? "default" : ExportNaming::SafeFileName(Material);
        Gdt->Materials.insert(Material);
        return MaterialGdt::AssetName(Material);
    }

    // The drawn triangles a brush's solid sides take their materials from (none: tool materials
    // only): Owner's (DrawnWorld::Owner), counted in Stats; the GDT naming them
    struct Texturing
    {
        DrawnWorld* Drawn = nullptr;
        RadiantGdt* Gdt = nullptr;
        uint32_t Owner = 0;
        RadiantTextureStats* Stats = nullptr;
        const DrawnWorld::HiddenTest* Hidden = nullptr;   // inside another brush (the world's)
    };

    // The world's solid brushes, to tell a point inside one (what hides a side's face): each
    // brush's planes, its bounds in a grid of Cell units
    class Solids
    {
    public:
        explicit Solids(const Collision& C)
        {
            for (const Brush& B : C.Brushes)
            {
                if (!(B.Contents & 1)) continue;
                const std::vector<BrushFace> Faces = BrushFaces(B);
                if (Faces.size() < 4) continue;
                std::vector<Plane> P;
                for (const BrushSide& S : B.Sides)
                {
                    const double L = std::sqrt((double)S.Normal[0] * S.Normal[0] + (double)S.Normal[1] * S.Normal[1] + (double)S.Normal[2] * S.Normal[2]);
                    if (L > 0.5) P.push_back({ { S.Normal[0] / L, S.Normal[1] / L, S.Normal[2] / L }, S.Dist / L });
                }
                V3 Lo = Faces[0].Points[0], Hi = Lo;
                for (const BrushFace& F : Faces)
                    for (const V3& V : F.Points)
                        for (int A = 0; A < 3; A++) Lo[A] = (std::min)(Lo[A], V[A]), Hi[A] = (std::max)(Hi[A], V[A]);
                const uint32_t Number = (uint32_t)Planes.size();
                Planes.push_back(std::move(P));
                for (int64_t X = CellOf(Lo[0]); X <= CellOf(Hi[0]); X++)
                    for (int64_t Y = CellOf(Lo[1]); Y <= CellOf(Hi[1]); Y++)
                        for (int64_t Z = CellOf(Lo[2]); Z <= CellOf(Hi[2]); Z++) Grid[Key(X, Y, Z)].push_back(Number);
            }
        }
        bool Inside(const V3& P) const
        {
            const auto It = Grid.find(Key(CellOf(P[0]), CellOf(P[1]), CellOf(P[2])));
            if (It == Grid.end()) return false;
            for (const uint32_t B : It->second)
            {
                bool In = true;
                for (const Plane& S : Planes[B]) In = In && Dot(S.N, P) - S.D <= 0.01;
                if (In) return true;
            }
            return false;
        }

    private:
        static constexpr double Cell = 512.0;
        static int64_t CellOf(double V) { return (int64_t)std::floor(V / Cell); }
        static uint64_t Key(int64_t X, int64_t Y, int64_t Z)
        {
            constexpr int64_t Bias = 1 << 20;
            return ((uint64_t)(X + Bias) << 42) | ((uint64_t)(Y + Bias) << 21) | (uint64_t)(Z + Bias);
        }
        std::vector<std::vector<Plane>> Planes;
        std::unordered_map<uint64_t, std::vector<uint32_t>> Grid;
    };

    // Writes one brush, Origin and Axis moving it (a brush model's, to its entity), all its sides
    // of material Only when set; false when its planes bound no closed volume. Its solid sides
    // (caulk, clip_full) lying on Tex's drawn triangles take their material, its other solid sides
    // then caulk.
    bool WriteBrush(std::string& Out, const Brush& B, size_t Number, const float* Origin, const float* Axis, const ToolNames& Tools,
                    const std::string& Only, RadiantStats& Stats, const Texturing& Tex = {})
    {
        Brush Moved = B;
        if (Origin)
            for (auto& S : Moved.Sides)
            {
                // a plane N . p = D moved by p' = Axis^T p + O: N' = Axis^T N, D' = D + N' . O
                const float N[3] = { S.Normal[0], S.Normal[1], S.Normal[2] };
                for (int K = 0; K < 3; K++) S.Normal[K] = N[0] * Axis[K] + N[1] * Axis[3 + K] + N[2] * Axis[6 + K];
                S.Dist += S.Normal[0] * Origin[0] + S.Normal[1] * Origin[1] + S.Normal[2] * Origin[2];
            }
        // the faces and their three points; when the brush they write is not sound, the points
        // checked against the whole brush, then also the faces with slivers up to 0.1 thick left out
        std::vector<BrushFace> Faces;
        std::vector<std::array<V3, 3>> Points;
        bool Written = false;
        for (int Try = 0; Try < 3 && !Written; Try++)
        {
            if (Try != 1) Faces = BrushFaces(Moved, Try == 0 ? 0.01 : 0.1);
            if (Faces.size() < 4) break;
            std::vector<V3> Corners;
            for (const BrushFace& F : Faces) Corners.insert(Corners.end(), F.Points.begin(), F.Points.end());
            Points.clear();
            for (const BrushFace& F : Faces)
            {
                const BrushSide& S = Moved.Sides[F.Side];
                const double L = std::sqrt((double)S.Normal[0] * S.Normal[0] + (double)S.Normal[1] * S.Normal[1] + (double)S.Normal[2] * S.Normal[2]);
                const Plane P = { { S.Normal[0] / L, S.Normal[1] / L, S.Normal[2] / L }, S.Dist / L };
                Points.push_back(ThreePoints(F, P, Try ? &Corners : nullptr));
            }
            Written = Sound(Points);
        }
        if (!Written)
        {
            (Faces.size() < 4 ? Stats.BrushesDropped : Stats.BrushesUnsound)++;
            return false;
        }
        // each side's material and projection
        std::vector<std::string> Names;
        for (const BrushFace& F : Faces) Names.push_back(Only.empty() ? Tools.Of(Moved, Moved.Sides[F.Side]) : Only);
        std::vector<SideTexture> Textures(Faces.size());
        std::vector<char> Textured(Faces.size(), 0);
        if (Tex.Drawn && Tex.Stats)
        {
            RadiantTextureStats& TS = *Tex.Stats;
            const auto Solid = [](const std::string& Name) { return Name == "caulk" || Name == "clip_full"; };
            bool Any = false;
            for (size_t K = 0; K < Faces.size(); K++)
            {
                if (!Solid(Names[K])) continue;
                TS.Sides++;
                const BrushSide& S = Moved.Sides[Faces[K].Side];
                const double L = std::sqrt((double)S.Normal[0] * S.Normal[0] + (double)S.Normal[1] * S.Normal[1] + (double)S.Normal[2] * S.Normal[2]);
                const V3 N = { S.Normal[0] / L, S.Normal[1] / L, S.Normal[2] / L };
                // the normal Radiant reads back from the three points as written
                std::array<V3, 3> Read;
                for (int J = 0; J < 3; J++)
                    for (int C = 0; C < 3; C++) Read[J][C] = std::strtod(Num(Points[K][J][C]).c_str(), nullptr);
                V3 Back = Cross(Sub(Read[0], Read[1]), Sub(Read[2], Read[1]));
                Back = Mul(Back, 1.0 / (std::max)(Length(Back), 1e-12));
                if (Tex.Drawn->Texture(Faces[K].Points, N, S.Dist / L, Back, Tex.Owner, Tex.Hidden, Textures[K], TS)) Textured[K] = 1, Any = true;
            }
            if (Any)
            {
                for (size_t K = 0; K < Faces.size(); K++)
                    if (!Textured[K] && Solid(Names[K])) Names[K] = "caulk";
                TS.BrushesTextured++;
            }
        }
        Out += "// brush " + std::to_string(Number) + "\n{\n";
        for (size_t K = 0; K < Faces.size(); K++)
        {
            const auto& Pts = Points[K];
            Out += " ";
            for (const V3& Pt : Pts) Out += "( " + Num(Pt[0]) + " " + Num(Pt[1]) + " " + Num(Pt[2]) + " ) ";
            if (Textured[K])
            {
                const SideTexture& T = Textures[K];
                Out += MaterialName(T.Material, Tex.Gdt) + " " + Num(T.Width) + " " + Num(T.Height) + " " + Num(T.ShiftX) + " " + Num(T.ShiftY) + " " + Num(T.Rotation) +
                       " 0 lightmap_gray 16384 16384 0 0 0 0\n";
            }
            else
                Out += Names[K] + " 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n";
        }
        Out += "}\n";
        Stats.Brushes++;
        Stats.Faces += Faces.size();
        return true;
    }

    // What becomes of drawn triangle K (indices 3K..3K+2) of a surface no brush side draws: a
    // patch, nothing (no area: drawn as nothing), or a misc_model (no material to give a patch,
    // or a value Radiant cannot read)
    enum class Fate { Patch, None, Model };
    Fate FateOf(const Surface& S, size_t K)
    {
        const uint32_t* I = &S.Indices[K * 3];
        if (I[0] >= S.Vertices.size() || I[1] >= S.Vertices.size() || I[2] >= S.Vertices.size()) return Fate::None;
        V3 P[3];
        for (int J = 0; J < 3; J++)
        {
            const Vertex& V = S.Vertices[I[J]];
            for (const float F : { V.Pos[0], V.Pos[1], V.Pos[2], V.Uv[0], V.Uv[1] })
                if (!std::isfinite(F)) return Fate::Model;
            P[J] = { V.Pos[0], V.Pos[1], V.Pos[2] };
        }
        if (Length(Cross(Sub(P[1], P[0]), Sub(P[2], P[0]))) < 1e-6) return Fate::None;
        return S.Material.empty() ? Fate::Model : Fate::Patch;
    }

    // The drawn triangles of Owner's meshes (DrawnWorld::Owner) no brush side draws, each a patch:
    // a 2x2 mesh whose fourth vertex repeats the second, one triangle whichever way Radiant splits
    // the cell, facing as the game's (a compiled mesh's triangles face against (row 1 - row 0) x
    // (column 1 - column 0)); nonColliding (the clip map's brushes collide); each vertex its
    // position, colour ("c", left out when white), UV ("t": UV x 1024) and lightmap coordinates
    // (the plane's texture axes, a texel per 16 units, as "2 2 16 8" says)
    void WritePatches(std::string& Out, const MapScene& Scene, const DrawnWorld& Drawn, uint32_t Owner, RadiantGdt* Gdt, RadiantStats& Stats,
                      size_t& Number)
    {
        for (size_t Oi = 0; Oi < Scene.Objects.size(); Oi++)
        {
            const Mesh& M = Scene.Objects[Oi];
            if (DrawnWorld::Owner(M.Name) != Owner) continue;
            for (size_t Si = 0; Si < M.Surfaces.size(); Si++)
            {
                const Surface& S = M.Surfaces[Si];
                std::string Name;
                for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                {
                    if (Drawn.Removed(Oi, Si, K / 3)) continue;
                    const Fate F = FateOf(S, K / 3);
                    if (F == Fate::None) Stats.TrianglesDegenerate++;
                    if (F != Fate::Patch) continue;
                    if (Name.empty()) Name = MaterialName(S.Material, Gdt);
                    const Vertex* V[3] = { &S.Vertices[S.Indices[K]], &S.Vertices[S.Indices[K + 1]], &S.Vertices[S.Indices[K + 2]] };
                    V3 P[3];
                    for (int J = 0; J < 3; J++) P[J] = { V[J]->Pos[0], V[J]->Pos[1], V[J]->Pos[2] };
                    const V3 N = Cross(Sub(P[1], P[0]), Sub(P[2], P[0]));
                    V3 Ls, Lt;
                    RadiantBaseAxes(Mul(N, 1.0 / Length(N)), Ls, Lt);
                    bool Coloured = false;
                    const auto Text = [&](int J) {
                        std::string T = "\tv " + Num(P[J][0]) + " " + Num(P[J][1]) + " " + Num(P[J][2]);
                        const uint8_t* C = V[J]->Color;
                        if (C[0] != 255 || C[1] != 255 || C[2] != 255 || C[3] != 255)
                        {
                            T += " c " + std::to_string(C[0]) + " " + std::to_string(C[1]) + " " + std::to_string(C[2]) + " " + std::to_string(C[3]);
                            Coloured = true;
                        }
                        return T + " t " + Num(V[J]->Uv[0] * 1024.0) + " " + Num(V[J]->Uv[1] * 1024.0) + " " + Num(Dot(P[J], Ls) / 16.0) + " " +
                               Num(Dot(P[J], Lt) / 16.0) + "\n";
                    };
                    Out += "// brush " + std::to_string(Number++) + "\n {\n  mesh\n  {\n  contents nonColliding;\n   " + Name +
                           "\n   lightmap_gray\n   2 2 16 8\n   (\n" + Text(0) + Text(1) + "   )\n   (\n" + Text(2) + Text(1) + "   )\n  }\n }\n";
                    Stats.Patches++;
                    if (Coloured) Stats.PatchesColoured++;
                }
            }
        }
    }

    std::string Quote(const std::string& Key, const std::string& Value)
    {
        std::string V = Value;
        for (auto& C : V)
            if (C == '"') C = '\'';
        return "\"" + Key + "\" \"" + V + "\"\n";
    }

    // Instance::Axis rows as Radiant's "angles" (pitch yaw roll, Q3's AnglesToAxis inverted)
    std::string Angles(const float Axis[9])
    {
        constexpr double Deg = 180.0 / 3.14159265358979323846;
        const double Fx = Axis[0], Fy = Axis[1], Fz = Axis[2], Lx = Axis[3], Ly = Axis[4], Lz = Axis[5], Uz = Axis[8];
        const double Cp = std::sqrt(Fx * Fx + Fy * Fy);
        double Pitch = std::atan2(-Fz, Cp) * Deg, Yaw, Roll;
        if (Cp > 1e-6)
        {
            Yaw = std::atan2(Fy, Fx) * Deg;
            Roll = std::atan2(Lz, Uz) * Deg;
        }
        else
        {
            Yaw = std::atan2(-Lx, Ly) * Deg;
            Roll = 0;
        }
        const auto Round = [](double V) { return std::fabs(V - std::round(V)) < 1e-3 ? std::round(V) : V; };
        return Num(Round(Pitch)) + " " + Num(Round(Yaw)) + " " + Num(Round(Roll));
    }

    std::string Vec(const float V[3]) { return Num(V[0]) + " " + Num(V[1]) + " " + Num(V[2]); }

    // ---- the models it names, as XMODEL_EXPORT (game units)

    // One rigid model of Surfaces (each split below the per-surface vertex limit); with ForGdt, its
    // materials named by their GDT asset names and added to ForGdt->Materials
    bool WriteXmodel(const std::vector<const Surface*>& Surfaces, const MapScene& Scene, const std::string& Name,
                     const fs::path& Path, RadiantGdt* ForGdt)
    {
        WraithModel Model;
        Model.AssetName = Name;
        WraithBone& Bone = Model.AddBone();
        Bone.TagName = "tag_origin";
        std::map<std::string, int32_t> MaterialIndex;
        for (const Surface* S : Surfaces)
        {
            if (S->Indices.size() < 3) continue;
            auto [It, New] = MaterialIndex.emplace(S->Material, (int32_t)Model.Materials.size());
            if (New)
            {
                WraithMaterial M;
                M.MaterialName = S->Material.empty() ? "default" : ExportNaming::SafeFileName(S->Material);
                if (ForGdt && !S->Material.empty())
                {
                    M.MaterialName = MaterialGdt::AssetName(S->Material);
                    ForGdt->Materials.insert(S->Material);
                }
                if (const auto Mat = Scene.Materials.find(S->Material); Mat != Scene.Materials.end() && !Mat->second.ColorMap.empty())
                    M.DiffuseMapName = Mat->second.ColorMap;   // the image's name, as model exports give it
                Model.AddMaterial(M);
            }
            // pieces of at most Limit vertices, each vertex copied once per piece
            constexpr size_t Limit = 65000;
            WraithSubmesh* Mesh = nullptr;
            std::map<uint32_t, uint32_t> Remap;
            for (size_t K = 0; K + 2 < S->Indices.size(); K += 3)
            {
                if (!Mesh || Mesh->Verticies.size() + 3 > Limit)
                {
                    Mesh = &Model.AddSubmesh();
                    Mesh->AddMaterial(It->second);
                    Remap.clear();
                }
                uint32_t Tri[3];
                for (int J = 0; J < 3; J++)
                {
                    const uint32_t I = S->Indices[K + J];
                    if (I >= S->Vertices.size()) return false;
                    auto [R, Added] = Remap.emplace(I, (uint32_t)Mesh->Verticies.size());
                    if (Added)
                    {
                        const Vertex& V = S->Vertices[I];
                        WraithVertex& W = Mesh->AddVertex();
                        W.Position = Vector3(V.Pos[0], V.Pos[1], V.Pos[2]);
                        W.Normal = Vector3(V.Normal[0], V.Normal[1], V.Normal[2]);
                        W.AddUVLayer(V.Uv[0], V.Uv[1]);
                        W.AddVertexWeight(0, 1.0f);
                        for (int C = 0; C < 4; C++) W.Color[C] = V.Color[C];
                    }
                    Tri[J] = R->second;
                }
                Mesh->AddFace(Tri[0], Tri[1], Tri[2]);
            }
        }
        if (Model.Submeshes.empty()) return false;
        try
        {
            CodXME::ExportXME(Model, Path.string());
        }
        catch (const std::exception&)
        {
            return false;
        }
        return true;
    }

    class RadiantSceneWriter final : public ISceneWriter
    {
    public:
        explicit RadiantSceneWriter(bool Materials) : WithMaterials(Materials) {}
        const char* Extension() const override { return ".map"; }
        bool Write(const MapScene& Scene, const std::string& Path, const Materials::IAdapter& Adapter, SceneWriteResult& Result,
                   std::string& Error) const override
        {
            RadiantStats Stats;
            RadiantGdt G;
            if (!WriteRadiant(Scene, Path, Stats, Error, WithMaterials ? &G : nullptr))
            {
                Error = "radiant map: " + Error;
                return false;
            }
            if (WithMaterials) Result.LaterCounters = MaterialsLater(Scene, Adapter, std::move(G));
            Result.Counters = { { "map_radiant_maps", 1 },
                                { "map_radiant_clip_brushes", Scene.Collision.ClipBrushes },
                                { "map_radiant_brushes", Stats.Brushes },
                                { "map_radiant_brushes_dropped", Stats.BrushesDropped },
                                { "map_radiant_brushes_unsound", Stats.BrushesUnsound },
                                { "map_radiant_trigger_brushes", Stats.TriggerBrushes },
                                { "map_radiant_triangles", Stats.Triangles },
                                { "map_radiant_triangles_dropped", Stats.TrianglesDropped },
                                { "map_radiant_triangles_duplicate", Stats.TrianglesDuplicate },
                                { "map_radiant_entities", Stats.Entities },
                                { "map_radiant_brush_entities", Stats.BrushEntities },
                                { "map_radiant_brush_entities_empty", Stats.BrushEntitiesEmpty },
                                { "map_radiant_placements", Stats.Placements },
                                { "map_radiant_world_models", Stats.WorldModels },
                                { "map_radiant_model_files", Stats.ModelFiles },
                                { "map_radiant_models_elsewhere", Stats.ModelsElsewhere } };
            Result.Counters.insert(Result.Counters.end(), { { "map_radiant_patches", Stats.Patches },
                                                            { "map_radiant_patches_coloured", Stats.PatchesColoured },
                                                            { "map_radiant_triangles_degenerate", Stats.TrianglesDegenerate },
                                                            { "map_radiant_triangles_in_models", Stats.TrianglesInModels } });
            for (const auto& [Prefix, T] : { std::make_pair("map_radiant_texture_", &Stats.Textures), std::make_pair("map_radiant_prism_texture_", &Stats.PrismTextures),
                                             std::make_pair("map_radiant_entity_texture_", &Stats.EntityTextures) })
            for (const auto& [Key, N] : std::initializer_list<std::pair<const char*, size_t>>{
                     { "sides_solid", T->Sides }, { "sides_textured", T->SidesTextured }, { "sides_split", T->SidesSplit },
                     { "sides_no_fit", T->SidesNoFit }, { "sides_mixed_maps", T->SidesMixedMaps }, { "sides_uv_missed", T->SidesUvMissed }, { "sides_undrawn", T->SidesUndrawn }, { "brushes_textured", T->BrushesTextured },
                     { "uv_error_below_1e4", T->UvExact }, { "uv_error_below_1e3", T->UvBelow1e3 },
                     { "uv_error_below_1e2", T->UvBelow1e2 }, { "uv_error_below_1e1", T->UvBelow1e1 }, { "uv_error_above_1e1", T->UvAbove },
                     { "uv_error_mean_1e6", T->SidesTextured + T->SidesUvMissed ? (size_t)std::llround(1e6 * T->UvErrorSum / (double)(T->SidesTextured + T->SidesUvMissed)) : 0 },
                     { "triangles_drawn", T->TrianglesDrawn }, { "triangles_removed", T->TrianglesRemoved },
                     { "triangles_coloured", T->TrianglesColoured },
                     { "plane_distance_0", T->Distance[0] }, { "plane_distance_below_1e3", T->Distance[1] },
                     { "plane_distance_below_1e2", T->Distance[2] }, { "plane_distance_below_tolerance", T->Distance[3] },
                     { "coverage_below_10", T->Coverage[0] }, { "coverage_below_50", T->Coverage[1] }, { "coverage_below_90", T->Coverage[2] },
                     { "coverage_90_up", T->Coverage[3] } })
                Result.Counters.emplace_back(std::string(Prefix) + Key, N);
            return true;
        }

    private:
        bool WithMaterials;

        // The GDT with the material and image entries of the materials its model files name, once
        // the run has written the images (some come from other zones): each image hard-linked (else
        // copied) to <map>_radiant/model_export/<map>/images/, which the image entries name.
        static std::function<void(std::map<std::string, size_t>&)> MaterialsLater(const MapScene& Scene, const Materials::IAdapter& Adapter,
                                                                                 RadiantGdt G)
        {
            std::vector<Materials::Description> Described;
            for (const std::string& Name : G.Materials) Described.push_back(DescribeMaterial(Scene, Name, Adapter, G.Root).second);
            return [G = std::move(G), Described = std::move(Described)](std::map<std::string, size_t>& Counters) {
                const fs::path Images = G.Root / "model_export" / G.Map / "images";
                // an image's file name there, the first source to take a name keeping it
                std::map<fs::path, std::string> Named;
                std::set<std::string> Taken;
                MaterialGdt::Builder B([&](const fs::path& File) {
                    auto [It, New] = Named.emplace(File, std::string());
                    if (New)
                    {
                        // a plain file name ("$specular.png" as "specular.png")
                        std::string Stem = MaterialGdt::AssetName(File.stem().string());
                        if (Stem.empty()) Stem = "image";
                        const std::string Ext = ExportNaming::Lower(File.extension().string());
                        std::string Name = Stem + Ext;
                        for (int N = 2; !Taken.insert(Name).second; N++) Name = Stem + "_" + std::to_string(N) + Ext;
                        It->second = Name;
                    }
                    return "model_export/" + G.Map + "/images/" + It->second;
                }, "material_gdt_");
                for (const auto& D : Described) B.Add(D, G.Root);
                std::vector<std::string> Entries = G.ModelEntries;
                const std::vector<std::string> Mine = B.Entries("\n");
                Entries.insert(Entries.end(), Mine.begin(), Mine.end());
                std::error_code Ec;
                fs::create_directories(Images, Ec);
                for (const auto& [File, Name] : Named)
                {
                    const fs::path Target = Images / Name;
                    fs::remove(Target, Ec);
                    fs::create_hard_link(File, Target, Ec);
                    if (Ec) fs::copy_file(File, Target, fs::copy_options::overwrite_existing, Ec);
                    Counters[Ec ? "material_gdt_images_not_copied" : "material_gdt_images_copied"]++;
                }
                for (const auto& [K, N] : B.Counters()) Counters[K] += N;
                fs::create_directories(G.Root / "source_data", Ec);
                const fs::path GdtFile = G.Root / "source_data" / (G.Map + ".gdt");
                std::ofstream(GdtFile, std::ios::binary) << Gdt::Document(Entries, "\n");
                std::ofstream(fs::path(GdtFile).replace_extension(".names.json"), std::ios::binary) << B.NamesJson();
            };
        }
    };
}

namespace
{
    // The faces of the sides Used, each oriented counter-clockwise seen from outside; Bounded false
    // when a face reaches the starting polygon's edge (an open brush)
    std::vector<BrushFace> Faces(const std::vector<Plane>& Planes, const std::vector<bool>& Used, bool& Bounded)
    {
        std::vector<BrushFace> Out;
        Bounded = true;
        for (size_t I = 0; I < Planes.size(); I++)
        {
            if (!Used[I]) continue;
            std::vector<V3> W = BaseWinding(Planes[I]);
            for (size_t J = 0; J < Planes.size() && W.size() >= 3; J++)
                if (J != I && Used[J]) W = Clip(W, Planes[J].N, Planes[J].D, OnPlane);
            if (W.size() < 3 || Area(W) < 1e-6) continue;
            V3 A = { 0, 0, 0 };
            for (size_t K = 1; K + 1 < W.size(); K++) A = Add(A, Cross(Sub(W[K], W[0]), Sub(W[K + 1], W[0])));
            if (Dot(A, Planes[I].N) < 0) std::reverse(W.begin(), W.end());
            for (const V3& V : W)
                if (std::fabs(V[0]) > Huge / 2 || std::fabs(V[1]) > Huge / 2 || std::fabs(V[2]) > Huge / 2) Bounded = false;
            Out.push_back({ I, std::move(W) });
        }
        return Out;
    }

    // how thin a face is: its area over its longest edge
    double Thickness(const std::vector<V3>& W)
    {
        double Longest = 0;
        for (size_t K = 0; K < W.size(); K++) Longest = (std::max)(Longest, Length(Sub(W[(K + 1) % W.size()], W[K])));
        return Longest > 0 ? Area(W) / Longest : 0;
    }
}

std::vector<BrushFace> BrushFaces(const Brush& B, double Within)
{
    std::vector<Plane> Planes;
    std::vector<bool> Used;
    for (const BrushSide& S : B.Sides)
    {
        const V3 N = { S.Normal[0], S.Normal[1], S.Normal[2] };
        const double L = Length(N);
        Planes.push_back(L > 0.5 ? Plane{ Mul(N, 1.0 / L), S.Dist / L } : Plane{ { 0, 0, 0 }, 0 });
        // a plane an earlier side already has bounds nothing more
        bool Duplicate = L <= 0.5;
        for (size_t J = 0; J + 1 < Planes.size() && !Duplicate; J++)
            Duplicate = Used[J] && Dot(Planes[J].N, Planes.back().N) > 0.999999 && std::fabs(Planes[J].D - Planes.back().D) < OnPlane;
        Used.push_back(!Duplicate);
    }
    bool Bounded = true;
    std::vector<BrushFace> Out = Faces(Planes, Used, Bounded);
    // a side that bounds nothing is left out; so is one whose face is a sliver or lies on another
    // side's plane (a bevel, a plane nearly another's: three points give its plane badly, Radiant
    // would drop it) when the brush stays closed and within Within of its plane without it
    std::vector<bool> Bounds(Planes.size(), false);
    for (const BrushFace& F : Out) Bounds[F.Side] = true;
    for (size_t I = 0; I < Planes.size(); I++) Used[I] = Used[I] && Bounds[I];
    std::vector<std::pair<double, size_t>> Thin;
    for (const BrushFace& F : Out)
    {
        // thin, or lying on another side's plane (two planes nearly one, crossing on the face)
        bool OnAnother = false;
        for (size_t J = 0; J < Planes.size() && !OnAnother; J++)
        {
            if (J == F.Side || !Used[J] || Dot(Planes[J].N, Planes[F.Side].N) < 0.99) continue;
            OnAnother = true;
            for (const V3& V : F.Points) OnAnother = OnAnother && std::fabs(Dot(Planes[J].N, V) - Planes[J].D) < Within;
        }
        if (const double T = Thickness(F.Points); T < (std::max)(0.05, Within) || OnAnother) Thin.emplace_back(OnAnother ? 0.0 : T, F.Side);
    }
    std::sort(Thin.begin(), Thin.end());
    for (const auto& [T, Side] : Thin)
    {
        Used[Side] = false;
        bool Bounded2 = true;
        std::vector<BrushFace> Without = Faces(Planes, Used, Bounded2);
        // left out when the brush without it stays closed and within Within of its plane
        double Out1 = 0;
        for (const BrushFace& F : Without)
            for (const V3& V : F.Points) Out1 = (std::max)(Out1, Dot(Planes[Side].N, V) - Planes[Side].D);
        if (Bounded2 && Without.size() >= 4 && Out1 <= Within)
            Out = std::move(Without);
        else
            Used[Side] = true;
    }
    return Out;
}

namespace
{
    // A collision triangle as a brush: its face, the face Thick units behind, a side per edge;
    // false for one thinner than 1/20 unit (no face to stand on, its planes would not survive
    // three points)
    bool Prism(const V3 P[3], double Thick, Brush& B)
    {
        V3 N = Cross(Sub(P[1], P[0]), Sub(P[2], P[0]));
        const double Twice = Length(N);
        double Longest = 0;
        for (int J = 0; J < 3; J++) Longest = (std::max)(Longest, Length(Sub(P[(J + 1) % 3], P[J])));
        if (Twice < 0.01 || Twice / Longest < 0.05) return false;
        N = Mul(N, 1.0 / Twice);
        const auto Side = [&B](const V3& Normal, double Dist) {
            BrushSide S;
            for (int J = 0; J < 3; J++) S.Normal[J] = (float)Normal[J];
            S.Dist = (float)Dist;
            B.Sides.push_back(S);
        };
        Side(N, Dot(N, P[0]));
        Side(Mul(N, -1), -(Dot(N, P[0]) - Thick));
        for (int J = 0; J < 3; J++)
        {
            V3 E = Cross(Sub(P[(J + 1) % 3], P[J]), N);
            E = Mul(E, 1.0 / Length(E));
            if (Dot(E, Sub(P[(J + 2) % 3], P[J])) > 0) E = Mul(E, -1);
            Side(E, Dot(E, P[J]));
        }
        return true;
    }

    // The entity string as Radiant reads it: a key numbered 0 is no script string, its name
    // follows quoted (MWR: 0 "materiallayering" "1")
    std::string EntityText(const std::string& Ents)
    {
        std::string Text;
        for (size_t I = 0; I < Ents.size();)
        {
            const size_t End = (std::min)(Ents.find('\n', I), Ents.size());
            const size_t From = Ents.compare(I, 3, "0 \"") == 0 ? I + 2 : I;
            Text.append(Ents, From, End - From);
            Text += '\n';
            I = End + 1;
        }
        return Text;
    }

    bool Numbered(const std::string& Key) { return !Key.empty() && std::all_of(Key.begin(), Key.end(), [](char C) { return C >= '0' && C <= '9'; }); }
    // three numbers, as an origin or angles are
    bool Vector3(const std::string& V)
    {
        const char* P = V.c_str();
        for (int K = 0; K < 3; K++)
        {
            char* End = nullptr;
            std::strtof(P, &End);
            if (End == P) return false;
            P = End;
        }
        while (*P == ' ') P++;
        return *P == 0;
    }

    // The keys an entity string names by number (a script string the title keeps in its exe:
    // Ghosts on) that its values tell apart, added to Names when it has none yet: classname (the
    // key worldspawn is the value of), model (values "*N" or "?N"), origin (the vector a brush
    // entity has), angles (the other vector a spawn point has)
    void InferKeys(const std::vector<Entity>& Ents, std::map<std::string, std::string>& Names)
    {
        std::map<std::string, std::string> Found;
        for (const Entity& E : Ents)
            for (const auto& [K, V] : E)
                if (Numbered(K) && V == "worldspawn") Found.emplace(K, "classname");
        if (Found.empty()) return;
        const std::string Class = Found.begin()->first;
        std::map<std::string, size_t> Models, Origins, Angles;
        for (const Entity& E : Ents)
            for (const auto& [K, V] : E)
                if (Numbered(K) && V.size() > 1 && (V[0] == '*' || V[0] == '?') && std::isdigit((unsigned char)V[1])) Models[K]++;
        const std::string Model = Models.empty() ? std::string() : std::max_element(Models.begin(), Models.end(),
                                      [](const auto& A, const auto& B) { return A.second < B.second; })->first;
        for (const Entity& E : Ents)
        {
            const bool Brush = !Model.empty() && E.count(Model);
            const auto C = E.find(Class);
            const bool Spawn = C != E.end() && C->second.find("spawn") != std::string::npos;
            for (const auto& [K, V] : E)
            {
                if (!Numbered(K) || !Vector3(V)) continue;
                if (Brush) Origins[K]++;
                if (Spawn) Angles[K]++;
            }
        }
        const auto Most = [](const std::map<std::string, size_t>& M, const std::string& Not) {
            std::string Best;
            size_t N = 0;
            for (const auto& [K, C] : M)
                if (K != Not && C > N) Best = K, N = C;
            return Best;
        };
        const std::string Origin = Most(Origins, "");
        const std::string Angle = Most(Angles, Origin);
        if (!Model.empty()) Found.emplace(Model, "model");
        if (!Origin.empty()) Found.emplace(Origin, "origin");
        if (!Angle.empty()) Found.emplace(Angle, "angles");
        for (const auto& [K, V] : Found) Names.emplace(K, V);
    }
}

bool TrianglePrism(const std::array<double, 3> P[3], double Thick, Brush& B) { return Prism(P, Thick, B); }

bool WriteRadiant(const MapScene& Scene, const std::string& MapPath, RadiantStats& Stats, std::string& Error, RadiantGdt* ForGdt)
{
    const fs::path Path(MapPath);
    const std::string Map = ExportNaming::SafeFileName(Path.stem().string());
    const fs::path Root = Path.parent_path() / (Map + "_radiant");
    const fs::path ModelDir = Root / "model_export" / Map;
    std::error_code Ec;
    fs::create_directories(ModelDir, Ec);
    if (Ec)
    {
        Error = "cannot create " + ModelDir.string();
        return false;
    }

    std::vector<Entity> Ents;
    const std::string Text = EntityText(Scene.MapEnts);
    if (!Text.empty() && !ParseEntities(Text, Ents)) Ents.clear();
    std::map<std::string, std::string> Names = Scene.EntityKeys;
    InferKeys(Ents, Names);
    for (Entity& E : Ents)
        for (const auto& [Number, Key] : Names)
            if (const auto It = E.find(Number); It != E.end())
            {
                E[Key] = It->second;
                E.erase(It);
            }

    std::string Out = "iwmap 4\n\"000_Global\" flags  active\n\"The Map\" flags \n";
    size_t EntityNumber = 0;
    const auto Begin = [&](const Entity* Keys, const char* Class) {
        Out += "// entity " + std::to_string(EntityNumber++) + "\n{\n";
        Out += Quote("classname", Keys && Keys->count("classname") ? Keys->at("classname") : Class);
        if (Keys)
            for (const auto& [K, V] : *Keys)
                if (K != "classname") Out += Quote(K, V);
    };

    // worldspawn: its keys, the world's brushes
    const ToolNames Tools(Scene.Collision);
    DrawnWorld Drawn(Scene, Stats.Textures);
    Stats.Textures.TrianglesDrawn = Drawn.Count();
    const Solids SolidBrushes(Scene.Collision);
    const DrawnWorld::HiddenTest Hidden = [&SolidBrushes](const V3& P) { return SolidBrushes.Inside(P); };
    const Texturing Tex{ &Drawn, ForGdt, 0, &Stats.Textures, &Hidden };
    // the brush models a mesh draws
    std::set<uint32_t> Owners;
    for (const Mesh& M : Scene.Objects)
        if (const uint32_t O = DrawnWorld::Owner(M.Name)) Owners.insert(O);
    std::set<uint32_t> Patched;   // the brush models whose triangles their entity holds as patches
    const Entity* World = nullptr;
    for (const Entity& E : Ents)
        if (E.count("classname") && E.at("classname") == "worldspawn")
        {
            World = &E;
            break;
        }
    Begin(World, "worldspawn");
    size_t BrushNumber = 0;
    for (const Brush& B : Scene.Collision.Brushes)
        if (WriteBrush(Out, B, BrushNumber, nullptr, nullptr, Tools, "", Stats, Tex)) BrushNumber++;
    Out += "}\n";

    // the collision triangles (patches, terrain): each a prism Thick units deep behind its face
    // (they face out), in a func_group (the world's at compile time). A triangle given twice (the
    // same corners) is written once: facing the same way, a copy; facing the other way (a sheet
    // colliding on both sides), the one facing up, else the first.
    constexpr double Thick = 2.0;   // how deep a prism is
    const auto& Tv = Scene.Collision.Vertices;
    const auto Prisms = [&](const std::vector<uint32_t>& Tris, size_t& Number) {
        std::map<std::array<float, 9>, size_t> Seen;   // corners, sorted -> the triangle written
        std::vector<char> Kept(Tris.size() / 3, 1);
        const auto Corner = [&](size_t T, int J) {
            const size_t I = (size_t)Tris[T * 3 + J] * 3;
            return V3{ Tv[I], Tv[I + 1], Tv[I + 2] };
        };
        const auto Up = [&](size_t T) {
            return Cross(Sub(Corner(T, 1), Corner(T, 0)), Sub(Corner(T, 2), Corner(T, 0)))[2] > 0;
        };
        for (size_t T = 0; T < Kept.size(); T++)
        {
            std::array<std::array<float, 3>, 3> C;
            for (int J = 0; J < 3; J++)
                for (int A = 0; A < 3; A++) C[J][A] = Tv[(size_t)Tris[T * 3 + J] * 3 + A];
            std::sort(C.begin(), C.end());
            std::array<float, 9> Key;
            for (int J = 0; J < 9; J++) Key[J] = C[J / 3][J % 3];
            const auto [It, New] = Seen.emplace(Key, T);
            if (New) continue;
            Stats.TrianglesDuplicate++;
            // the other way round and facing up where the first does not: this one instead
            const size_t First = It->second;
            const V3 A = Cross(Sub(Corner(First, 1), Corner(First, 0)), Sub(Corner(First, 2), Corner(First, 0)));
            const V3 B = Cross(Sub(Corner(T, 1), Corner(T, 0)), Sub(Corner(T, 2), Corner(T, 0)));
            if (Dot(A, B) < 0 && Up(T) && !Up(First))
            {
                Kept[First] = 0;
                It->second = T;
            }
            else
                Kept[T] = 0;
        }
        for (size_t T = 0; T < Kept.size(); T++)
        {
            if (!Kept[T]) continue;
            V3 P[3];
            for (int J = 0; J < 3; J++) P[J] = Corner(T, J);
            Brush B;
            if (!Prism(P, Thick, B))
            {
                Stats.TrianglesDropped++;
                continue;
            }
            // counted as triangles, not as the clip map's brushes
            const size_t Dropped = Stats.BrushesDropped, Unsound = Stats.BrushesUnsound;
            if (WriteBrush(Out, B, Number, nullptr, nullptr, Tools, "clip_full", Stats, { Tex.Drawn, ForGdt, 0, &Stats.PrismTextures, &Hidden }))
            {
                Number++;
                Stats.Brushes--, Stats.Triangles++;
            }
            else
            {
                Stats.BrushesDropped = Dropped, Stats.BrushesUnsound = Unsound;
                Stats.TrianglesDropped++;
            }
        }
    };
    if (!Scene.Collision.Triangles.empty())
    {
        Begin(nullptr, "func_group");
        BrushNumber = 0;
        Prisms(Scene.Collision.Triangles, BrushNumber);
        Out += "}\n";
    }

    // the other entities; a brush entity's brushes moved to where it stands
    for (const Entity& E : Ents)
    {
        if (&E == World) continue;
        // a brush model ("*N") or a trigger volume ("?N"), each in its own space: its brushes moved
        // to where the entity stands
        const auto Model = E.find("model");
        const char Kind = Model != E.end() && !Model->second.empty() ? Model->second[0] : 0;
        const bool BrushEntity = Kind == '*' || Kind == '?';
        const std::vector<std::vector<Brush>>& From = Kind == '?' ? Scene.Collision.Triggers : Scene.Collision.Models;
        size_t N = 0;
        if (BrushEntity)
        {
            N = (size_t)std::strtoul(Model->second.c_str() + 1, nullptr, 10);
            // a brush model without brushes still holds its drawn triangles' patches
            const bool HasDrawn = Kind == '*' && N > 0 && Owners.count((uint32_t)N);
            if ((N >= From.size() || From[N].empty()) && !HasDrawn)
            {
                // no brush to give it: kept as the game has it
                Stats.BrushEntitiesEmpty++;
                Begin(&E, "info_null");
                Out += "}\n";
                Stats.Entities++;
                continue;
            }
        }
        Entity Keys = E;
        if (BrushEntity) Keys.erase("model");
        Begin(&Keys, "info_null");
        if (BrushEntity)
        {
            float Origin[3] = { 0, 0, 0 }, Ang[3] = { 0, 0, 0 }, Axis[9];
            EntityFloats(E, "origin", Origin, 3);
            EntityFloats(E, "angles", Ang, 3);
            AnglesToAxis(Ang, Axis);
            // a trigger's brushes are its volume, whatever the clip map gives them
            const auto Class = E.find("classname");
            const bool Trigger = Kind == '?' || (Class != E.end() && Class->second.rfind("trigger", 0) == 0);
            size_t Number = 0;
            // a brush model's brushes take the materials its drawn triangles give them, the others patches
            const Texturing Own{ Kind == '*' ? Tex.Drawn : nullptr, ForGdt, (uint32_t)N, &Stats.EntityTextures, &Hidden };
            if (N < From.size())
                for (const Brush& B : From[N])
                    if (WriteBrush(Out, B, Number, Origin, Axis, Tools, Trigger ? "trigger" : "", Stats, Own))
                    {
                        Number++;
                        if (Kind == '?') Stats.Brushes--, Stats.TriggerBrushes++;   // not the clip map's
                    }
            if (Kind == '*' && N > 0 && Patched.insert((uint32_t)N).second) WritePatches(Out, Scene, Drawn, (uint32_t)N, ForGdt, Stats, Number);
            Stats.BrushEntities++;
        }
        Out += "}\n";
        Stats.Entities++;
    }

    // the world's drawn triangles no brush side draws, and those of brush models no entity holds,
    // as patches
    {
        std::string Group;
        size_t Number = 0;
        WritePatches(Group, Scene, Drawn, 0, ForGdt, Stats, Number);
        for (const uint32_t O : Owners)
            if (!Patched.count(O)) WritePatches(Group, Scene, Drawn, O, ForGdt, Stats, Number);
        if (!Group.empty())
        {
            Begin(nullptr, "func_group");
            Out += Group + "}\n";
        }
    }

    // the placements, then the world geometry left, as misc_models; the models they name as files
    std::set<std::string> Named;
    const auto Place = [&](const Instance& I) {
        Out += "// entity " + std::to_string(EntityNumber++) + "\n{\n";
        Out += Quote("classname", "misc_model") + Quote("model", I.Model) + Quote("origin", Vec(I.Origin)) +
               Quote("angles", Angles(I.Axis));
        if (std::fabs(I.Scale - 1.0f) > 1e-4f) Out += Quote("modelscale", Num(I.Scale));
        Out += "}\n";
        Stats.Placements++;
        Named.insert(I.Model);
    };
    for (const auto* List : { &Scene.StaticInstances, &Scene.DynamicInstances, &Scene.SplinedInstances })
        for (const Instance& I : *List)
            if (!I.Model.empty()) Place(I);

    // the GDT listing them, LF as it has always been written
    std::vector<std::string> Entries;
    const auto Listed = [&](const std::string& Name, const std::string& File) {
        Entries.push_back(Gdt::Entry(Name, "xmodel.gdf", { { "filename", Gdt::Path(Map + "/" + File) }, { "type", "rigid" } }, "\n"));
    };
    for (const std::string& Name : Named)
    {
        const auto It = Scene.Models.find(Name);
        if (It == Scene.Models.end())
        {
            Stats.ModelsElsewhere++;
            continue;
        }
        std::vector<const Surface*> Surfaces;
        for (const Surface& S : It->second.Surfaces) Surfaces.push_back(&S);
        const std::string File = ExportNaming::SafeFileName(Name) + ".XMODEL_EXPORT";
        if (!WriteXmodel(Surfaces, Scene, Name, ModelDir / File, ForGdt)) continue;
        Listed(Name, File);
        Stats.ModelFiles++;
    }

    // the world's triangles neither a brush side nor a patch draws in tiles of Tile units (by
    // triangle centre), each its own model; a brush model's one model each
    constexpr double Tile = 4096.0;
    std::map<std::string, std::vector<Surface>> Pieces;
    for (size_t Oi = 0; Oi < Scene.Objects.size(); Oi++)
    {
        const Mesh& M = Scene.Objects[Oi];
        const bool IsWorld = M.Name == "world" || M.Name.compare(0, 11, "brushmodel_") != 0;
        for (size_t Si = 0; Si < M.Surfaces.size(); Si++)
        {
            const Surface& S = M.Surfaces[Si];
            std::map<std::string, std::map<uint32_t, uint32_t>> Remaps;
            for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
            {
                if (Drawn.Removed(Oi, Si, K / 3)) continue;
                if (FateOf(S, K / 3) != Fate::Model) continue;
                const uint32_t Tri[3] = { S.Indices[K], S.Indices[K + 1], S.Indices[K + 2] };
                if (Tri[0] >= S.Vertices.size() || Tri[1] >= S.Vertices.size() || Tri[2] >= S.Vertices.size()) continue;
                Stats.TrianglesInModels++;
                std::string Piece = Map + "_" + ExportNaming::SafeFileName(M.Name);
                if (IsWorld)
                {
                    double C[2] = { 0, 0 };
                    for (int J = 0; J < 3; J++)
                        for (int A = 0; A < 2; A++) C[A] += S.Vertices[Tri[J]].Pos[A] / 3.0;
                    Piece = Map + "_world_" + std::to_string((long long)std::floor(C[0] / Tile)) + "_" +
                            std::to_string((long long)std::floor(C[1] / Tile));
                    for (auto& Ch : Piece)
                        if (Ch == '-') Ch = 'm';
                }
                auto& Surfaces = Pieces[Piece];
                auto& Remap = Remaps[Piece];
                if (Surfaces.empty() || Surfaces.back().Material != S.Material || Remap.empty())
                {
                    Surfaces.push_back({ S.Material, {}, {} });
                    Remap.clear();
                }
                Surface& Into = Surfaces.back();
                for (const uint32_t I : Tri)
                {
                    auto [R, Added] = Remap.emplace(I, (uint32_t)Into.Vertices.size());
                    if (Added) Into.Vertices.push_back(S.Vertices[I]);
                    Into.Indices.push_back(R->second);
                }
            }
        }
    }
    for (const auto& [Name, Surfaces] : Pieces)
    {
        std::vector<const Surface*> List;
        for (const Surface& S : Surfaces) List.push_back(&S);
        const std::string File = Name + ".XMODEL_EXPORT";
        if (!WriteXmodel(List, Scene, Name, ModelDir / File, ForGdt)) continue;
        Listed(Name, File);
        Stats.WorldModels++;
        Out += "// entity " + std::to_string(EntityNumber++) + "\n{\n";
        Out += Quote("classname", "misc_model") + Quote("model", Name) + Quote("origin", "0 0 0") + Quote("angles", "0 0 0");
        Out += "}\n";
    }
    std::ofstream File(Path, std::ios::binary);
    if (!File)
    {
        Error = "cannot create " + Path.string();
        return false;
    }
    File << Out;
    if (ForGdt)
    {
        ForGdt->Root = Root;
        ForGdt->Map = Map;
        ForGdt->ModelEntries = std::move(Entries);
        return File.good();
    }
    fs::create_directories(Root / "source_data", Ec);
    std::ofstream(Root / "source_data" / (Map + ".gdt"), std::ios::binary) << Gdt::Document(Entries, "\n");
    return File.good();
}

const ISceneWriter& RadiantWriter(bool Materials)
{
    static const RadiantSceneWriter Plain(false), WithMaterials(true);
    return Materials ? WithMaterials : Plain;
}
}
