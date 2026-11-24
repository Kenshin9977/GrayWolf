#include "stdafx.h"

#include "MapRadiantWriter.h"
#include "RadiantTarget.h"

#include "ExportNaming.h"
#include "GdtWriter.h"
#include "MapLive.h"
#include "MapMaterials.h"
#include "MapRadiantBound.h"
#include "MapRadiantGeometry.h"
#include "MapRadiantPlanes.h"
#include "MapRadiantTextures.h"
#include "MaterialGdt.h"
#include "ModToolsTree.h"
#include "MapSceneWriter.h"
#include "WraithModel.h"
#include "XMBExport.h"
#include "XMEExport.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace Maps
{
namespace
{
    using namespace RadiantGeometry;

    constexpr double OnPlane = 1e-5;     // a point this close to a plane is on it (the planes are exact)
    // the most drawn triangles written a patch each (map_surfaces patches): Radiant holds a patch as an
    // object of its own, some 5 KB (5.8 million of them did not fit in 32 GB); stock zm_giant has 10 556
    constexpr size_t MostPatches = 250000;
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

    using ExportNaming::Num;

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
    // solid brush (CoD4: solid with a drawn material) stood for geometry now drawn by a misc_model:
    // clip_full, which blocks everything and seals nothing. Black Ops' drawn surfaces' brushes
    // stop bullets and missiles only (0x2080), the player walking through: clip_weapon (as
    // clip_full, BO1's mp_firingrange was full of invisible walls).
    class ToolNames
    {
    public:
        explicit ToolNames(const Collision&) {}
        std::string Of(const Brush& B, const BrushSide& S) const
        {
            const std::string M = ExportNaming::Lower(S.Material);
            if (RadiantTool(M))
                if (std::string T = Bo3ToolName(M); !T.empty()) return T;
            constexpr uint32_t Solid = 0x1, Glass = 0x10, CanShootClip = 0x40, MissileClip = 0x80, VehicleClip = 0x200,
                               ItemClip = 0x400, SkyFlag = 0x800, NoSight = 0x1000, ClipShot = 0x2000, PlayerClip = 0x10000,
                               MonsterClip = 0x20000, Mantle = 0x1000000, Detail = 0x8000000, Structural = 0x10000000,
                               Trigger = 0x40000000;
            const uint32_t C = B.Contents & ~Detail;
            if (C & Trigger) return "trigger";
            if (C & SkyFlag) return "sky";
            if (C & Solid) return M.empty() || RadiantTool(M) ? "caulk" : "clip_full";
            // what each BO3 tool blocks, as its GDT entry flags it (the mod tools' texture_assets):
            // clip 0x430200, clip_full 0x4336c0, clip_nosight 0x431640, clip_player_vehicle
            // 0x10200, clip_weapon 0x2040, clip_missile_no_player 0xc0; BO1's tools the same bits
            // (0x400000, utility, BO3's own)
            const bool Player = C & PlayerClip, Monster = C & MonsterClip;
            if (Player && Monster) return C & ClipShot ? "clip_full" : C & NoSight ? "clip_nosight" : "clip";
            if (Player) return C & VehicleClip ? "clip_player_vehicle" : "clip_player";
            if (Monster) return "clip_ai";
            // a window (glass, bullets): the player kept out as BO1 did
            if ((C & Glass) && (C & ClipShot)) return "clip_full";
            if ((C & ClipShot) && (C & MissileClip)) return "clip_weapon";
            // BO1 tells mantle_over from mantle_on by the side's surface flags (0x8000000)
            if (C & Mantle) return S.Surface & 0x8000000 ? "mantle_over" : "mantle_on";
            if (C & Glass) return "clip";
            if (C & VehicleClip) return "clip_vehicle";
            if (C & ItemClip) return "clip_physics";
            if (C & NoSight) return "nosight_noclip";
            if (C & ClipShot) return "clip_weapon";
            if ((C & MissileClip) && (C & CanShootClip)) return "clip_missile_no_player";
            if (C & MissileClip) return "clip_missile";
            if (C & CanShootClip) return "clip_weapon";
            if (C & Structural) return "hint";
            // no content but detail: blocks nothing (BO1's mp_nuked fills its doorways and its
            // yards so, door-sized slabs: as clip they stood as invisible walls)
            if (C == 0) return "nodraw_notsolid";
            return "clip";
        }
    };

    // A drawn material's name in the .map: as the model files name it (its GDT asset name with a
    // GDT, which then lists it)
    // A material's GDT asset name, recorded in Gdt: a stock one (RadiantGdt::Stock) as it is, the
    // others the GDT defines
    std::string GdtMaterialName(RadiantGdt& Gdt, const std::string& Given)
    {
        const std::string* Shared = nullptr;
        if (Gdt.Same)
            if (const auto It = Gdt.Same->find(Given); It != Gdt.Same->end()) Shared = &It->second;
        const std::string& Material = Shared ? *Shared : Given;
        if (Gdt.Stock && MaterialGdt::AssetName(Material) == Material && Gdt.Stock->count(Material))
        {
            Gdt.StockNamed.insert(Material);
            return Material;
        }
        Gdt.Materials.insert(Material);
        return MaterialGdt::PrefixedName(Gdt.Prefix, Material);
    }
    std::string MaterialName(const std::string& Material, RadiantGdt* Gdt)
    {
        if (!Gdt) return Material.empty() ? "default" : ExportNaming::SafeFileName(Material);
        return GdtMaterialName(*Gdt, Material);
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
        bool Grid = false;   // the brush's points on a sixteenth of a unit (a map fitted to BO3's tools)
    };

    // Fn(I, State&) for I below Count on the machine's threads, Batch at a time, each thread its own
    // State. Tick (when given) is told how many are done by the calling thread after each of its
    // batches; false from it stops the work (the batches running end): ParallelFor returns false.
    template <class State, class F>
    bool ParallelFor(size_t Count, size_t Batch, const F& Fn, const std::function<bool(size_t Done)>& Tick = {})
    {
        const size_t Threads = (std::max)(1u, (std::min)(std::thread::hardware_concurrency(), 16u));
        std::atomic<size_t> Next{ 0 }, Done{ 0 };
        std::atomic<bool> Stop{ false };
        const auto Work = [&](bool Calling) {
            State S{};
            for (size_t I; !Stop.load(std::memory_order_relaxed) && (I = Next.fetch_add(Batch)) < Count;)
            {
                const size_t End = (std::min)(I + Batch, Count);
                for (size_t J = I; J < End; J++) Fn(J, S);
                const size_t Now = Done.fetch_add(End - I) + (End - I);
                if (Calling && Tick && !Tick(Now)) Stop = true;
            }
        };
        std::vector<std::thread> Pool;
        for (size_t T = 1; T < (std::min)(Threads, (Count + Batch - 1) / Batch); T++) Pool.emplace_back(Work, false);
        Work(true);
        for (auto& T : Pool) T.join();
        return !Stop;
    }

    // The world's solid brushes, to tell a point inside one (what hides a side's face): each
    // brush's planes, its bounds in a grid of Cell units
    class Solids
    {
    public:
        explicit Solids(const Collision& C)
        {
            // the faces worked out on the machine's threads, the grid filled in the brushes' order
            std::vector<std::vector<BrushFace>> AllFaces(C.Brushes.size());
            struct None {};
            ParallelFor<None>(C.Brushes.size(), 64, [&](size_t I, None&) {
                if (C.Brushes[I].Contents & 1) AllFaces[I] = BrushFaces(C.Brushes[I]);
            });
            for (size_t Bi = 0; Bi < C.Brushes.size(); Bi++)
            {
                const Brush& B = C.Brushes[Bi];
                if (!(B.Contents & 1)) continue;
                const std::vector<BrushFace>& Faces = AllFaces[Bi];
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
                // the cells of its bounds but those wholly outside one of its planes (past Inside's
                // 0.01, with room for the rounding): no point there is inside it (a sloping brush's
                // bounds are mostly outside it)
                const auto Outside = [&P](int64_t X, int64_t Y, int64_t Z) {
                    constexpr double Half = Cell / 2;
                    const V3 C = { ((double)X + 0.5) * Cell, ((double)Y + 0.5) * Cell, ((double)Z + 0.5) * Cell };
                    for (const Plane& S : P)
                        if (Dot(S.N, C) - S.D - Half * (std::fabs(S.N[0]) + std::fabs(S.N[1]) + std::fabs(S.N[2])) > 0.011) return true;
                    return false;
                };
                const uint32_t Number = (uint32_t)Planes.size();
                for (int64_t X = CellOf(Lo[0]); X <= CellOf(Hi[0]); X++)
                    for (int64_t Y = CellOf(Lo[1]); Y <= CellOf(Hi[1]); Y++)
                        for (int64_t Z = CellOf(Lo[2]); Z <= CellOf(Hi[2]); Z++)
                            if (!Outside(X, Y, Z)) Grid[Key(X, Y, Z)].push_back(Number);
                Planes.push_back(std::move(P));
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

    // A brush worked out (PrepareBrush, threads may run it at once) before it is written in turn
    // (CommitBrush): its faces, their three points, materials and projections, its texture counts
    struct PreparedBrush
    {
        bool Written = false;
        std::vector<BrushFace> Faces;
        std::vector<std::array<V3, 3>> Points;
        std::vector<std::string> Names;
        std::vector<SideTexture> Textures;
        std::vector<char> Textured;
        RadiantTextureStats Counts;
    };

    // Flat: a brush kept out of Black Ops III's BSP (MAX_MAP_NODES): no side textured, caulk clip_full
    // (a clip side splits no node), its drawn triangles left to the world's models
    PreparedBrush PrepareBrush(const Brush& B, const float* Origin, const float* Axis, const ToolNames& Tools, const std::string& Only,
                               const Texturing& Tex, DrawnWorld::Scratch* Scratch = nullptr, bool Flat = false)
    {
        PreparedBrush R;
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
        std::vector<BrushFace>& Faces = R.Faces;
        std::vector<std::array<V3, 3>>& Points = R.Points;
        bool& Written = R.Written;
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
        if (!Written) return R;
        // the points on a sixteenth of a unit: cod2map's brush edges (brush_edges.cpp) assert
        // "isCycle" on neighbours whose corners lie a few ten-thousandths apart (IW's mp_flip's
        // rotating brush models, triangles made prisms); on a grid they meet exactly. Kept as they
        // were when the grid would leave the brush unsound.
        if (Tex.Grid)
        {
            std::vector<std::array<V3, 3>> Grid = Points;
            for (auto& T : Grid)
                for (V3& P : T)
                    for (double& C : P) C = std::round(C * 16.0) / 16.0;
            bool Ok = Sound(Grid);
            for (const auto& T : Grid) Ok = Ok && Length(Cross(Sub(T[0], T[1]), Sub(T[2], T[1]))) > 1e-3;
            if (Ok) Points = std::move(Grid);
        }
        // each side's material and projection
        std::vector<std::string>& Names = R.Names;
        for (const BrushFace& F : Faces) Names.push_back(Only.empty() ? Tools.Of(Moved, Moved.Sides[F.Side]) : Only);
        if (Flat)
            for (std::string& N : Names)
                if (N == "caulk") N = "clip_full";
        std::vector<SideTexture>& Textures = R.Textures;
        std::vector<char>& Textured = R.Textured;
        Textures.resize(Faces.size());
        Textured.assign(Faces.size(), 0);
        if (Tex.Drawn && Tex.Stats && !Flat)
        {
            RadiantTextureStats& TS = R.Counts;
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
                if (Tex.Drawn->Texture(Faces[K].Points, N, S.Dist / L, Back, Tex.Owner, Tex.Hidden, Textures[K], TS, Scratch)) Textured[K] = 1, Any = true;
            }
            if (Any)
            {
                for (size_t K = 0; K < Faces.size(); K++)
                    if (!Textured[K] && Solid(Names[K])) Names[K] = "caulk";
                TS.BrushesTextured++;
            }
        }
        return R;
    }

    // the draw surfaces cod2map makes of what is written so far: one a brush side and one a patch
    // (MAX_MAP_DRAW_SURFS; measured on MW4's mp_rex_rm_shipment: 275098 patches with 16000 floor
    // boxes past it, 13500 not, a box's sides all counting whatever their tool)
    size_t DrawSurfaces(const RadiantStats& Stats)
    {
        return Stats.Patches + Stats.OverlayPatches[0] + Stats.OverlayPatches[1] + Stats.OverlayPatches[2] + Stats.TerrainPatches +
               Stats.CollisionPatches + Stats.Faces;
    }

    // A prepared brush written as brush Number, counted in Stats; false when it is not written
    bool CommitBrush(std::string& Out, const PreparedBrush& R, size_t Number, RadiantStats& Stats, const Texturing& Tex)
    {
        const auto& Faces = R.Faces;
        const auto& Points = R.Points;
        const auto& Names = R.Names;
        const auto& Textures = R.Textures;
        const auto& Textured = R.Textured;
        if (!R.Written)
        {
            (Faces.size() < 4 ? Stats.BrushesDropped : Stats.BrushesUnsound)++;
            return false;
        }
        // a portal brush (the source's visibility cells) is left out: Black Ops III's compiler
        // has no use for it and warns of each
        if (std::all_of(Names.begin(), Names.end(), [](const std::string& N) { return N == "portal" || N == "portal_nodraw"; }))
        {
            Stats.PortalBrushes++;
            return false;
        }
        if (Tex.Drawn && Tex.Stats) Add(*Tex.Stats, R.Counts);
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
            {
                Out += Names[K] + " 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n";
                if (Names[K] == "caulk") Stats.CaulkVertices += Faces[K].Points.size();
            }
        }
        Out += "}\n";
        Stats.Brushes++;
        Stats.Faces += Faces.size();
        return true;
    }

    // Writes one brush, Origin and Axis moving it (a brush model's, to its entity), all its sides
    // of material Only when set; false when its planes bound no closed volume. Its solid sides
    // (caulk, clip_full) lying on Tex's drawn triangles take their material, its other solid sides
    // then caulk.
    bool WriteBrush(std::string& Out, const Brush& B, size_t Number, const float* Origin, const float* Axis, const ToolNames& Tools,
                    const std::string& Only, RadiantStats& Stats, const Texturing& Tex = {}, bool Flat = false)
    {
        return CommitBrush(Out, PrepareBrush(B, Origin, Axis, Tools, Only, Tex, nullptr, Flat), Number, Stats, Tex);
    }

    // Brushes prepared on the machine's threads (each its own scratch), in their order; Tick as
    // ParallelFor's (false: stopped, Out incomplete). Skip: those left out; Flat: as PrepareBrush's.
    bool PrepareBrushes(const std::vector<const Brush*>& Brushes, const ToolNames& Tools, const Texturing& Tex, std::vector<PreparedBrush>& Out,
                        const std::function<bool(size_t Done)>& Tick, const std::set<const Brush*>& Flat)
    {
        Out.assign(Brushes.size(), PreparedBrush());
        return ParallelFor<DrawnWorld::Scratch>(Brushes.size(), 64, [&](size_t I, DrawnWorld::Scratch& Scratch) {
            Out[I] = PrepareBrush(*Brushes[I], nullptr, nullptr, Tools, "", Tex, &Scratch, Flat.count(Brushes[I]) != 0);
        }, Tick);
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

    // Whether drawn triangle K (indices 3K..3K+2) of S is a floor: its corners' normals (the side it
    // is drawn on) up, no steeper than a player walks (the pmove's 0.7)
    bool Floor(const Surface& S, size_t K)
    {
        V3 N{};
        for (int J = 0; J < 3; J++)
        {
            const uint32_t I = S.Indices[K * 3 + J];
            if (I >= S.Vertices.size()) return false;
            N = Add(N, V3{ S.Vertices[I].Normal[0], S.Vertices[I].Normal[1], S.Vertices[I].Normal[2] });
        }
        const double L = Length(N);
        return std::isfinite(L) && L > 1e-6 && N[2] / L >= 0.7;
    }

    // The drawn triangles of Owner's meshes (DrawnWorld::Owner) no brush side draws, each a patch:
    // a 2x2 mesh whose fourth vertex repeats the second, one triangle whichever way Radiant splits
    // the cell, facing as the game's (a compiled mesh's triangles face along (row 1 - row 0) x
    // (column 1 - column 0), against the drawn triangle's own (1 - 0) x (2 - 0)); nonColliding (the clip map's brushes collide); each vertex its
    // position, colour ("c", left out when white; white for layer weights: Weights), UV ("t": UV x 1024) and lightmap coordinates
    // (the plane's texture axes, a texel per 16 units, as "2 2 16 8" says)
    // Drawn triangle K of S (a Fate::Patch one) as patch Number, its material Name. With Over, the
    // blend layer Over drawn over it (MapRadiantLayers.h): its corners white, the layer's weight as
    // alpha, the layer's UVs, a decal in the layer's decal layer as the mod tools' blend patches are
    // ("c 255 255 255 <a>", decalLayerSort), on the triangle itself (BO3 sorts a decal over the
    // coplanar surface under it: Materials.pdf, "authored on coplanar geometry")
    // A terrain's layers by texel (MapScene's TexelLayers) the material of S has, null when none
    const TexelLayers* TexelsOf(const MapScene& Scene, const Surface& S)
    {
        const auto It = Scene.Materials.find(S.Material);
        return It == Scene.Materials.end() ? nullptr : It->second.Texels.get();
    }
    // The layer drawn at triangle K's (indices K..K + 2) centre, -1 none: the terrain's own material
    // (its baked colour map) draws it
    int TexelLayer(const TexelLayers* T, const Surface& S, size_t K)
    {
        if (T == nullptr) return -1;
        float U = 0, V = 0;
        for (int J = 0; J < 3; J++) U += S.Vertices[S.Indices[K + J]].Uv[0] / 3.0f, V += S.Vertices[S.Indices[K + J]].Uv[1] / 3.0f;
        return T->At(U, V);
    }
    // vertex I of S as layer L draws it: its UV times the layer's scale
    Vertex TexelCorner(const Surface& S, uint32_t I, const TexelLayers::Layer& L)
    {
        Vertex V = S.Vertices[I];
        V.Uv[0] *= L.Scale[0], V.Uv[1] *= L.Scale[1];
        return V;
    }
    // A tiled surface's UVs moved by whole repeats to start at 0 (a terrain layer's run to
    // hundreds over the terrain: Black Ops III keeps a model's UVs as half floats, a patch's within
    // texCoordHalfLimit)
    void UvTowardZero(std::vector<Vertex>& Vs)
    {
        if (Vs.empty()) return;
        float Lo[2] = { Vs[0].Uv[0], Vs[0].Uv[1] };
        for (const Vertex& V : Vs)
            for (int A = 0; A < 2; A++) Lo[A] = (std::min)(Lo[A], V.Uv[A]);
        for (int A = 0; A < 2; A++) Lo[A] = std::floor(Lo[A]);
        for (Vertex& V : Vs)
            for (int A = 0; A < 2; A++) V.Uv[A] -= Lo[A];
    }

    // Layer: a terrain's layer drawing it (TexelLayers), its UVs scaled and moved toward 0
    void WriteTrianglePatch(std::string& Out, const Surface& S, size_t K, const std::string& Name, bool Weights, RadiantStats& Stats,
                            size_t& Number, const BlendLayer* Over = nullptr, const TexelLayers::Layer* Layer = nullptr, const float* Place = nullptr)
    {
        const uint32_t Ix[3] = { S.Indices[K], S.Indices[K + 1], S.Indices[K + 2] };
        const Vertex* V[3] = { &S.Vertices[Ix[0]], &S.Vertices[Ix[1]], &S.Vertices[Ix[2]] };
        std::vector<Vertex> Layered;
        if (Layer)
        {
            for (int J = 0; J < 3; J++) Layered.push_back(TexelCorner(S, Ix[J], *Layer));
            UvTowardZero(Layered);
            for (int J = 0; J < 3; J++) V[J] = &Layered[J];
        }
        V3 P[3];
        for (int J = 0; J < 3; J++) P[J] = { V[J]->Pos[0], V[J]->Pos[1], V[J]->Pos[2] };
        // a brush model's triangles in its own space moved where its entity stands (Place: origin,
        // then axis rows): p' = Axis^T p + O, as its brushes
        if (Place)
            for (V3& Q : P)
            {
                const V3 L = Q;
                for (int A = 0; A < 3; A++) Q[A] = L[0] * Place[3 + A] + L[1] * Place[6 + A] + L[2] * Place[9 + A] + Place[A];
            }
        const V3 N = Cross(Sub(P[1], P[0]), Sub(P[2], P[0]));
        V3 Ls, Lt;
        RadiantBaseAxes(Mul(N, 1.0 / Length(N)), Ls, Lt);
        bool Coloured = false;
        const auto Text = [&](int J) {
            std::string T = "\tv " + Num(P[J][0]) + " " + Num(P[J][1]) + " " + Num(P[J][2]);
            uint8_t Own[4] = { 255, 255, 255, Over ? LayerWeight(*V[J], Over->Layer) : (uint8_t)255 };
            const uint8_t* C = Over ? Own : Tint(*V[J], Weights);
            if (C[0] != 255 || C[1] != 255 || C[2] != 255 || C[3] != 255)
            {
                T += " c " + std::to_string(C[0]) + " " + std::to_string(C[1]) + " " + std::to_string(C[2]) + " " + std::to_string(C[3]);
                Coloured = true;
            }
            float Uv[2] = { V[J]->Uv[0], V[J]->Uv[1] };
            if (Over) LayerUv(S, Ix[J], Over->Layer, Uv);
            return T + " t " + Num(Uv[0] * 1024.0) + " " + Num(Uv[1] * 1024.0) + " " + Num(Dot(P[J], Ls) / 16.0) + " " +
                   Num(Dot(P[J], Lt) / 16.0) + "\n";
        };
        Out += "// brush " + std::to_string(Number++) + "\n {\n  mesh\n  {\n  contents nonColliding;\n   " + Name +
               "\n   lightmap_gray\n   2 2 16 8\n   (\n" + Text(0) + Text(1) + "   )\n   (\n" + Text(2) + Text(1) + "   )\n";
        if (Over)
        {
            Out += std::string("   decalLayerSort \"") + LayerSort(Over->Layer) + "\"\n   decalEditorSort -1\n";
            Stats.OverlayPatches[Over->Layer - 2]++;
        }
        Out += "  }\n }\n";
        if (Over) return;
        Stats.Patches++;
        if (Coloured) Stats.PatchesColoured++;
    }

    // The blend layers drawn over the layered materials (MapRadiantLayers.h), null Layers without
    // the material GDT (it defines their materials); the (material, layer) pairs drawn somewhere
    struct Overlaying
    {
        const BlendLayers* Layers = nullptr;
        std::set<std::pair<std::string, int>> Carried;
    };
    const std::vector<const BlendLayer*> NoLayers;

    // The drawn triangles another of the same sort key covers exactly (the same three corners to
    // 1/16 unit, facing the same way, one owner's: DrawnWorld::Owner). The game draws both at one
    // depth and the later one wins (its world surfaces in draw order); Black Ops III's depth test
    // lets them fight (BO1 mp_firingrange's red hut wall on wood panels). The earlier is left out,
    // with its overlays; a material of no known sort key (-1) is left alone (a decal of such a
    // game lies on the surface under it).
    class CoveredTriangles
    {
    public:
        explicit CoveredTriangles(const MapScene& Scene)
        {
            using Key = std::array<int64_t, 13>;   // owner, sort key, the corners sorted, the facing
            std::map<Key, std::array<size_t, 3>> Latest;
            Of.resize(Scene.Objects.size());
            for (size_t O = 0; O < Scene.Objects.size(); O++)
            {
                const Mesh& M = Scene.Objects[O];
                Of[O].resize(M.Surfaces.size());
                for (size_t Si = 0; Si < M.Surfaces.size(); Si++)
                {
                    const Surface& S = M.Surfaces[Si];
                    const auto Mat = Scene.Materials.find(S.Material);
                    if (Mat == Scene.Materials.end() || Mat->second.SortKey < 0 || NoSurface(Scene, S.Material) ||
                        RadiantTool(ExportNaming::Lower(S.Material)))
                        continue;
                    Of[O][Si].assign(S.Indices.size() / 3, 0);
                    for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                    {
                        if (S.Indices[K] >= S.Vertices.size() || S.Indices[K + 1] >= S.Vertices.size() || S.Indices[K + 2] >= S.Vertices.size()) continue;
                        std::array<std::array<int64_t, 3>, 3> C;
                        V3 P[3];
                        for (int J = 0; J < 3; J++)
                        {
                            const Vertex& V = S.Vertices[S.Indices[K + J]];
                            P[J] = { V.Pos[0], V.Pos[1], V.Pos[2] };
                            for (int A = 0; A < 3; A++) C[J][A] = std::llround(V.Pos[A] * 16.0);
                        }
                        const V3 N = Cross(Sub(P[1], P[0]), Sub(P[2], P[0]));
                        int Axis = 0;
                        for (int A = 1; A < 3; A++)
                            if (std::fabs(N[A]) > std::fabs(N[Axis])) Axis = A;
                        if (N[Axis] == 0) continue;
                        std::sort(C.begin(), C.end());
                        Key Kk{ (int64_t)DrawnWorld::Owner(M.Name), Mat->second.SortKey };
                        for (int J = 0; J < 3; J++)
                            for (int A = 0; A < 3; A++) Kk[2 + J * 3 + A] = C[J][A];
                        Kk[11] = Axis, Kk[12] = N[Axis] > 0;
                        const auto [It, New] = Latest.emplace(Kk, std::array<size_t, 3>{ O, Si, K / 3 });
                        if (New) continue;
                        Of[It->second[0]][It->second[1]][It->second[2]] = 1;
                        It->second = { O, Si, K / 3 };
                        Count++;
                    }
                }
            }
        }
        // whether triangle K of Scene.Objects[Object].Surfaces[Surf] is covered
        bool Covered(size_t Object, size_t Surf, size_t K) const
        {
            return Object < Of.size() && Surf < Of[Object].size() && K < Of[Object][Surf].size() && Of[Object][Surf][K];
        }
        size_t Count = 0;

    private:
        std::vector<std::vector<std::vector<char>>> Of;   // per object, surface, triangle
    };

    // L as a material the files name: its GDT entry's source (RadiantGdt::Overlays)
    void Overlaid(const BlendLayer& L, RadiantGdt* Gdt)
    {
        if (Gdt) Gdt->Overlays.emplace(L.Name, L);
    }

    // whether Layer's weight is 0 at the three corners of triangle K (indices K..K + 2) of S
    bool Unweighted(const Surface& S, size_t K, int Layer)
    {
        for (int J = 0; J < 3; J++)
            if (LayerWeight(S.Vertices[S.Indices[K + J]], Layer)) return false;
        return true;
    }

    // vertex I of S as an overlay's of Layer: white, the layer's weight as alpha, the layer's UVs
    Vertex OverlayCorner(const Surface& S, uint32_t I, int Layer)
    {
        Vertex V = S.Vertices[I];
        V.Color[0] = V.Color[1] = V.Color[2] = 255;
        V.Color[3] = LayerWeight(S.Vertices[I], Layer);
        LayerUv(S, I, Layer, V.Uv);
        return V;
    }

    // the blend layers over drawn triangle K of S (a Fate::Patch one), a patch each where its weight is not 0
    void WriteOverlayPatches(std::string& Out, const Surface& S, size_t K, Overlaying& Ov, RadiantGdt* Gdt, RadiantStats& Stats, size_t& Number,
                             const float* Place = nullptr)
    {
        if (!Ov.Layers) return;
        for (const BlendLayer* L : Ov.Layers->Of(S.Material))
        {
            if (!L) continue;
            if (Unweighted(S, K, L->Layer))
            {
                Stats.OverlayUnweighted++;
                continue;
            }
            Overlaid(*L, Gdt);
            WriteTrianglePatch(Out, S, K, MaterialName(L->Name, Gdt), false, Stats, Number, L, nullptr, Place);
            Ov.Carried.insert({ S.Material, L->Layer });
        }
    }

    // a drawn triangle (object, surface, triangle) as a key of FloorsInModels
    uint64_t TriangleKey(size_t Oi, size_t Si, size_t T) { return ((uint64_t)Oi << 44) | ((uint64_t)Si << 28) | (uint64_t)T; }

    // Owner's drawn triangles no brush side draws as patches, then the blend layers over all its
    // drawn triangles (those a brush side draws too); FloorsOnly: its floors' only (Floor) but those
    // in InModels (TriangleKey: left to the world's models), each also added to Floors (where it
    // stands, facing up) when given
    void WritePatches(std::string& Out, const MapScene& Scene, const DrawnWorld& Drawn, uint32_t Owner, RadiantGdt* Gdt, RadiantStats& Stats,
                      size_t& Number, Overlaying& Ov, const CoveredTriangles& Cover, const float* Place = nullptr, bool FloorsOnly = false,
                      std::vector<std::array<V3, 3>>* Floors = nullptr, const std::unordered_set<uint64_t>* InModels = nullptr)
    {
        for (size_t Oi = 0; Oi < Scene.Objects.size(); Oi++)
        {
            const Mesh& M = Scene.Objects[Oi];
            if (DrawnWorld::Owner(M.Name) != Owner) continue;
            for (size_t Si = 0; Si < M.Surfaces.size(); Si++)
            {
                const Surface& S = M.Surfaces[Si];
                if (NoSurface(Scene, S.Material)) continue;
                std::string Name;
                const TexelLayers* Texels = TexelsOf(Scene, S);
                for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                {
                    if (Drawn.Removed(Oi, Si, K / 3) || Cover.Covered(Oi, Si, K / 3)) continue;
                    if (FloorsOnly && (!Floor(S, K / 3) || (InModels && InModels->count(TriangleKey(Oi, Si, K / 3))))) continue;
                    const Fate F = FateOf(S, K / 3);
                    if (F == Fate::None) Stats.TrianglesDegenerate++;
                    if (F != Fate::Patch) continue;
                    if (FloorsOnly) Stats.FloorPatches++;
                    if (FloorsOnly && Floors)
                    {
                        std::array<V3, 3> T;
                        for (int J = 0; J < 3; J++)
                        {
                            const auto& P = S.Vertices[S.Indices[K + J]].Pos;
                            T[J] = { P[0], P[1], P[2] };
                            if (Place)
                            {
                                const V3 L = T[J];
                                for (int A = 0; A < 3; A++) T[J][A] = L[0] * Place[3 + A] + L[1] * Place[6 + A] + L[2] * Place[9 + A] + Place[A];
                            }
                        }
                        if (Cross(Sub(T[1], T[0]), Sub(T[2], T[0]))[2] < 0) std::swap(T[1], T[2]);
                        Floors->push_back(T);
                    }
                    if (const int L = TexelLayer(Texels, S, K); L >= 0)
                    {
                        Stats.TexelLayerTriangles++;
                        WriteTrianglePatch(Out, S, K, MaterialName(Texels->Layers[L].Material, Gdt), false, Stats, Number, nullptr, &Texels->Layers[L], Place);
                        continue;
                    }
                    if (Name.empty()) Name = MaterialName(S.Material, Gdt);
                    WriteTrianglePatch(Out, S, K, Name, Scene.LayerWeights(S.Material), Stats, Number, nullptr, nullptr, Place);
                }
                if (Ov.Layers && !Ov.Layers->Of(S.Material).empty())
                    for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                        if (FateOf(S, K / 3) == Fate::Patch && !Cover.Covered(Oi, Si, K / 3) && (!FloorsOnly || (Floor(S, K / 3) && !(InModels && InModels->count(TriangleKey(Oi, Si, K / 3))))))
                            WriteOverlayPatches(Out, S, K, Ov, Gdt, Stats, Number, Place);
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

    // Whether triangle K / 3 of S has an area (its corners not one point or one line)
    bool HasArea(const Surface& S, size_t K)
    {
        const auto P = [&](int J) {
            const uint32_t I = S.Indices[K + J];
            return I < S.Vertices.size() ? V3{ S.Vertices[I].Pos[0], S.Vertices[I].Pos[1], S.Vertices[I].Pos[2] } : V3{ 0, 0, 0 };
        };
        return Length(Cross(Sub(P(1), P(0)), Sub(P(2), P(0)))) > 1e-9;
    }
    bool HasArea(const Surface& S)
    {
        for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
            if (HasArea(S, K)) return true;
        return false;
    }

    // One rigid model of Surfaces (each split below the per-surface vertex limit); with ForGdt, its
    // materials named by their GDT asset names and added to ForGdt->Materials
    bool WriteXmodel(const std::vector<const Surface*>& Surfaces, const MapScene& Scene, const std::string& Name,
                     const fs::path& Path, RadiantGdt* ForGdt)
    {
        // Black Ops III's linker reads XMODEL_BIN (version 7); XMODEL_EXPORT version 6 it refuses
        const bool Bin = ExportNaming::Lower(Path.extension().string()) == ".xmodel_bin";
        WraithModel Model;
        Model.AssetName = Name;
        WraithBone& Bone = Model.AddBone();
        Bone.TagName = "tag_origin";
        std::map<std::string, int32_t> MaterialIndex;   // by the name the file gives (materials may share one)
        for (const Surface* S : Surfaces)
        {
            if (S->Indices.size() < 3) continue;
            std::string Named = S->Material.empty() ? "default" : ExportNaming::SafeFileName(S->Material);
            if (ForGdt && !S->Material.empty()) Named = GdtMaterialName(*ForGdt, S->Material);
            auto [It, New] = MaterialIndex.emplace(Named, (int32_t)Model.Materials.size());
            if (New)
            {
                WraithMaterial M;
                M.MaterialName = Named;
                if (const auto Mat = Scene.Materials.find(S->Material); Mat != Scene.Materials.end() && !Mat->second.ColorMap.empty())
                    M.DiffuseMapName = Mat->second.ColorMap;   // the image's name, as model exports give it
                Model.AddMaterial(M);
            }
            // pieces of at most Limit vertices, each vertex copied once per piece
            constexpr size_t Limit = 65000;
            const bool Weights = Scene.LayerWeights(S->Material);   // its vertices' colours are no colour: white
            WraithSubmesh* Mesh = nullptr;
            std::map<uint32_t, uint32_t> Remap;
            for (size_t K = 0; K + 2 < S->Indices.size(); K += 3)
            {
                // a triangle of no area the linker drops: a model of only such ends it
                // ("baseFaceCount > 0", BO6's tm_debris_paper_towel_01, its corners one point)
                if (Bin && !HasArea(*S, K)) continue;
                if (!Mesh || Mesh->Verticies.size() + 3 > Limit)
                {
                    Mesh = &Model.AddSubmesh();
                    Mesh->AddMaterial(It->second);
                    Remap.clear();
                }
                uint32_t Tri[3];
                for (int J = 0; J < 3; J++)
                    if (S->Indices[K + J] >= S->Vertices.size()) return false;
                // For the mod tools' linker, which derives the tangents from the normals and UVs and
                // asserts on a tangent not square to its normal: unit normals (BO2's are not all
                // unit), the triangle's for one of no length; a triangle whose UVs have no area (no
                // tangent) or whose corner normal lies near its plane gets corners of its own, the
                // triangle's normal and, for no UV area, UVs a tiny planar mapping adds to (the
                // same texel drawn). The scene's triangles turn clockwise seen from their front (the
                // game's order; MapGlbWriter reverses them for glTF): the face normal is (P2 - P0) x
                // (P1 - P0). Taken the other way, every triangle's corner normals lay behind it and
                // each got corners of its own and its back as their normal (every placed model
                // faceted and lit from inside)
                V3 Face{}, P[3];
                bool Own = false;
                if (Bin)
                {
                    for (int J = 0; J < 3; J++)
                    {
                        const Vertex& V = S->Vertices[S->Indices[K + J]];
                        P[J] = { V.Pos[0], V.Pos[1], V.Pos[2] };
                    }
                    Face = Cross(Sub(P[2], P[0]), Sub(P[1], P[0]));
                    const double L = Length(Face);
                    Face = L > 0 ? Mul(Face, 1.0 / L) : V3{ 0, 0, 1 };
                    const Vertex* C[3] = { &S->Vertices[S->Indices[K]], &S->Vertices[S->Indices[K + 1]], &S->Vertices[S->Indices[K + 2]] };
                    const double UvArea = (C[1]->Uv[0] - C[0]->Uv[0]) * (C[2]->Uv[1] - C[0]->Uv[1]) - (C[2]->Uv[0] - C[0]->Uv[0]) * (C[1]->Uv[1] - C[0]->Uv[1]);
                    Own = !(std::fabs(UvArea) > 1e-12);
                    for (int J = 0; J < 3 && !Own; J++)
                    {
                        const V3 N = { C[J]->Normal[0], C[J]->Normal[1], C[J]->Normal[2] };
                        const double Ln = Length(N);
                        Own = !(Ln > 1e-4) || Dot(Mul(N, 1.0 / Ln), Face) < 0.1;
                    }
                }
                V3 Ls{}, Lt{};
                if (Own) RadiantBaseAxes(Face, Ls, Lt);
                const auto UnitNormal = [&](const Vertex& V) {
                    const V3 N = { V.Normal[0], V.Normal[1], V.Normal[2] };
                    const double L = Length(N);
                    const V3 U = std::isfinite(L) && L > 1e-4 ? Mul(N, 1.0 / L) : Face;
                    return Vector3((float)U[0], (float)U[1], (float)U[2]);
                };
                for (int J = 0; J < 3; J++)
                {
                    const uint32_t I = S->Indices[K + J];
                    const Vertex& V = S->Vertices[I];
                    if (Own)
                    {
                        Tri[J] = (uint32_t)Mesh->Verticies.size();
                        WraithVertex& W = Mesh->AddVertex();
                        W.Position = Vector3(V.Pos[0], V.Pos[1], V.Pos[2]);
                        W.Normal = Vector3((float)Face[0], (float)Face[1], (float)Face[2]);
                        W.AddUVLayer(V.Uv[0] + (float)(Dot(P[J], Ls) * 1e-5), V.Uv[1] + (float)(Dot(P[J], Lt) * 1e-5));
                        W.AddVertexWeight(0, 1.0f);
                        for (int Ch = 0; Ch < 4; Ch++) W.Color[Ch] = Tint(V, Weights)[Ch];
                        continue;
                    }
                    auto [R, Added] = Remap.emplace(I, (uint32_t)Mesh->Verticies.size());
                    if (Added)
                    {
                        WraithVertex& W = Mesh->AddVertex();
                        W.Position = Vector3(V.Pos[0], V.Pos[1], V.Pos[2]);
                        W.Normal = Bin ? UnitNormal(V) : Vector3(V.Normal[0], V.Normal[1], V.Normal[2]);
                        W.AddUVLayer(V.Uv[0], V.Uv[1]);
                        W.AddVertexWeight(0, 1.0f);
                        for (int Ch = 0; Ch < 4; Ch++) W.Color[Ch] = Tint(V, Weights)[Ch];
                    }
                    Tri[J] = R->second;
                }
                Mesh->AddFace(Tri[0], Tri[1], Tri[2]);
            }
        }
        if (Model.Submeshes.empty()) return false;
        try
        {
            if (Bin) CodXMB::ExportXMB(Model, Path.string());
            else CodXME::ExportXME(Model, Path.string());
        }
        catch (const std::exception&)
        {
            return false;
        }
        return true;
    }

    // One model file to write: its surfaces, its name, its GDT entry's, its path
    struct XmodelJob
    {
        std::vector<const Surface*> Surfaces;
        std::string Name, Entry;
        fs::path Path;
    };

    // The jobs' files written on the machine's threads; Written whether each was, ForGdt's
    // materials added in the jobs' order; Tick as ParallelFor's (false: stopped)
    bool WriteXmodels(const std::vector<XmodelJob>& Jobs, const MapScene& Scene, RadiantGdt* ForGdt, std::vector<char>& Written,
                      const std::function<bool(size_t Done)>& Tick)
    {
        Written.assign(Jobs.size(), 0);
        std::vector<std::set<std::string>> Materials(Jobs.size()), Stock(Jobs.size());
        struct None {};
        const bool Whole = ParallelFor<None>(Jobs.size(), 1, [&](size_t I, None&) {
            RadiantGdt Own;
            if (ForGdt) Own.Prefix = ForGdt->Prefix, Own.Stock = ForGdt->Stock, Own.Same = ForGdt->Same;
            Written[I] = WriteXmodel(Jobs[I].Surfaces, Scene, Jobs[I].Name, Jobs[I].Path, ForGdt ? &Own : nullptr);
            Materials[I] = std::move(Own.Materials);
            Stock[I] = std::move(Own.StockNamed);
        }, Tick);
        if (ForGdt)
            for (size_t I = 0; I < Jobs.size(); I++)
            {
                ForGdt->Materials.insert(Materials[I].begin(), Materials[I].end());
                ForGdt->StockNamed.insert(Stock[I].begin(), Stock[I].end());
            }
        return Whole;
    }

    // Surfaces as the models a target takes (RadiantTarget.h): parts of at most MostSurfaces
    // surfaces and MostVertices vertices of a material, a surface past MostVertices cut into
    // pieces (kept in Store). A vertex is counted per triangle corner: WriteXmodel may give a
    // triangle corners of its own.
    std::vector<std::vector<const Surface*>> ModelParts(const std::vector<const Surface*>& Surfaces, size_t MostSurfaces,
                                                       size_t MostVertices, std::deque<Surface>& Store)
    {
        MostSurfaces = (std::max)(size_t(1), MostSurfaces);
        MostVertices = (std::max)(size_t(3), MostVertices);
        std::vector<const Surface*> Cut;
        for (const Surface* S : Surfaces)
        {
            if (S->Indices.size() <= MostVertices)
            {
                Cut.push_back(S);
                continue;
            }
            const size_t More = S->UvSets > 1 ? (S->UvSets - 1) * 2 : 0;
            std::unordered_map<uint32_t, uint32_t> Remap;
            Surface* Into = nullptr;
            for (size_t K = 0; K + 2 < S->Indices.size(); K += 3)
            {
                if (!Into || Into->Indices.size() + 3 > MostVertices)
                {
                    Into = &Store.emplace_back();
                    Into->Material = S->Material;
                    Into->UvSets = S->UvSets;
                    Cut.push_back(Into);
                    Remap.clear();
                }
                for (int J = 0; J < 3; J++)
                {
                    const uint32_t I = S->Indices[K + J];
                    if (I >= S->Vertices.size()) continue;
                    auto [R, Added] = Remap.emplace(I, (uint32_t)Into->Vertices.size());
                    if (Added)
                    {
                        Into->Vertices.push_back(S->Vertices[I]);
                        if (More && S->MoreUvs.size() >= (I + 1) * More)
                            Into->MoreUvs.insert(Into->MoreUvs.end(), S->MoreUvs.begin() + I * More, S->MoreUvs.begin() + (I + 1) * More);
                    }
                    Into->Indices.push_back(R->second);
                }
            }
        }
        // first fit: the first part with room for the surface and its material's vertices
        std::vector<std::vector<const Surface*>> Parts;
        std::vector<std::map<std::string, size_t>> Held;
        for (const Surface* S : Cut)
        {
            size_t P = 0;
            for (; P < Parts.size(); P++)
                if (Parts[P].size() < MostSurfaces && Held[P][S->Material] + S->Indices.size() <= MostVertices) break;
            if (P == Parts.size())
            {
                Parts.emplace_back();
                Held.emplace_back();
            }
            Parts[P].push_back(S);
            Held[P][S->Material] += S->Indices.size();
        }
        return Parts;
    }
    // a model's part P's name: "<name>", "<name>_2"...
    std::string PartName(const std::string& Name, size_t P) { return P == 0 ? Name : Name + "_" + std::to_string(P + 1); }

    class RadiantSceneWriter final : public ISceneWriter
    {
    public:
        RadiantSceneWriter(bool Materials, std::string Into, bool AsPatches, std::string Of, std::string Kept, std::string Fitted)
            : WithMaterials(Materials), Tree(std::move(Into)), Patches(AsPatches), Game(std::move(Of)), Props(std::move(Kept)), Fit(std::move(Fitted)) {}
        const char* Extension() const override { return ".map"; }
        bool Write(const MapScene& Scene, const std::string& Path, const Materials::IAdapter& Adapter, SceneWriteResult& Result,
                   std::string& Error, const WriteProgress& Progress) const override
        {
            RadiantStats Stats;
            RadiantGdt G;
            if (!WriteRadiant(Scene, Path, Stats, Error, WithMaterials ? &G : nullptr, Tree, Patches, Game, Progress, Props, Fit))
            {
                Error = "radiant map: " + Error;
                return false;
            }
            // layered materials named as another whose GDT entry theirs is (SharedFirstLayers)
            const size_t Shared = G.Same ? G.Same->size() : 0;
            if (WithMaterials)
            {
                if (!Tell(Progress, "describing the materials"))
                {
                    Error = "cancelled";
                    return false;
                }
                Result.LaterCounters = MaterialsLater(Scene, Adapter, std::move(G));
                Result.LaterWhat = "the material GDT (the colour maps read for their alpha, the images linked beside it)";
            }
            Result.Warnings = Stats.Warnings;
            Result.Counters = { { "map_radiant_maps", 1 },
                                { "map_radiant_clip_brushes", Scene.Collision.ClipBrushes },
                                { "map_radiant_brushes", Stats.Brushes },
                                { "map_radiant_brushes_dropped", Stats.BrushesDropped },
                                { "map_radiant_brushes_unsound", Stats.BrushesUnsound },
                                { "map_radiant_trigger_brushes", Stats.TriggerBrushes },
                                { "map_radiant_portal_brushes", Stats.PortalBrushes },
                                { "map_radiant_skybox_brushes", Stats.SkyboxBrushes },
                                { "map_radiant_entities_left_out", Stats.EntitiesLeftOut },
                                { "map_radiant_origins_as_structs", Stats.OriginsAsStructs },
                                { "map_radiant_models_left_out", Stats.ModelsLeftOut },
                                { "map_radiant_vehicles_as_models", Stats.VehiclesAsModels },
                                { "map_radiant_beyond_world", Stats.BeyondWorld },
                                { "map_radiant_triangles_covered", Stats.TrianglesCovered },
                                { "map_radiant_beyond_sky_box", Stats.BeyondSkyBox },
                                { "map_radiant_clipped_to_sky_box", Stats.ClippedToSkyBox },
                                { "map_radiant_beyond_sky_box_placements", Stats.BeyondSkyBoxPlacements },
                                { "map_radiant_beyond_sky_box_entities", Stats.BeyondSkyBoxEntities },
                                { "map_radiant_sky_box_reach", Stats.SkyBoxReach },
                                { "map_radiant_sky_box_size", Stats.SkyBoxSize },
                                { "map_radiant_oversize_patches", Stats.OversizePatches },
                                { "map_radiant_floor_patches", Stats.FloorPatches },
                                { "map_radiant_floor_occluder_brushes", Stats.FloorOccluderBrushes },
                                { "map_radiant_floor_occluder_cell", Stats.FloorOccluderCell },
                                { "map_radiant_floors_in_models", Stats.FloorsInModels },
                                { "map_radiant_draw_surfaces", DrawSurfaces(Stats) },
                                { "map_radiant_caulk_vertices", Stats.CaulkVertices },
                                { "map_radiant_floor_occluder_left_out", Stats.FloorOccluderLeftOut },
                                { "map_radiant_scaled_to_nothing", Stats.ScaledToNothing },
                                { "map_radiant_collision_patches", Stats.CollisionPatches },
                                { "map_radiant_brushes_over_budget", Stats.BrushesOverBudget },
                                { "map_radiant_limits_over", Stats.LimitsOver },
                                { "map_radiant_patches_refused", Stats.PatchesRefused },
                                { "map_radiant_foliage_left_out", Stats.FoliageLeftOut },
                                { "map_radiant_small_left_out", Stats.SmallLeftOut },
                                { "map_radiant_plane_slots", Stats.PlaneSlots },
                                { "map_radiant_plane_side_planes", Stats.PlaneSidePlanes },
                                { "map_radiant_box_bevels", Stats.BoxBevels },
                                { "map_radiant_edge_bevels", Stats.EdgeBevels },
                                { "map_radiant_collision_brush_sides", Stats.CollisionBrushSides },
                                { "map_radiant_prism_plane_slots", Stats.PrismPlaneSlots },
                                { "map_radiant_patch_collision_triangles", Stats.PatchCollisionTriangles },
                                { "map_radiant_patch_collision_vertices", Stats.PatchCollisionVertices },
                                { "map_radiant_brushes_as_patches", Stats.BrushesAsPatches },
                                { "map_radiant_plane_slots_planned", Stats.PlaneSlotsPlanned },
                                { "map_radiant_brush_models_placed", Stats.BrushModelsPlaced },
                                { "map_radiant_brushes_flat", Stats.BrushesFlat },
                                { "map_radiant_structural_sides", Stats.StructuralSides },
                                { "map_radiant_collision_fitted", Stats.CollisionFitted },
                                { "map_radiant_collision_noncolliding", Stats.CollisionNonColliding },
                                { "map_radiant_collision_not_player", Stats.CollisionNotPlayer },
                                { "map_radiant_collision_coplanar_left_out", Stats.CollisionCoplanarLeftOut },
                                { "map_radiant_placements_fitted", Stats.PlacementsFitted },
                                { "map_radiant_exploders_dropped", Stats.ExplodersDropped },
                                { "map_radiant_brush_entities_dropped", Stats.BrushEntitiesDropped },
                                { "map_radiant_brushes_many_sided", Stats.BrushesManySided },
                                { "map_radiant_collision_outside_play", Stats.CollisionOutsidePlay },
                                { "map_radiant_triangles", Stats.Triangles },
                                { "map_radiant_triangles_dropped", Stats.TrianglesDropped },
                                { "map_radiant_triangles_duplicate", Stats.TrianglesDuplicate },
                                { "map_radiant_triangle_brushes", Stats.TriangleBrushes },
                                { "map_radiant_terrain_patches", Stats.TerrainPatches },
                                { "map_radiant_entities", Stats.Entities },
                                { "map_radiant_brush_entities", Stats.BrushEntities },
                                { "map_radiant_brush_entities_empty", Stats.BrushEntitiesEmpty },
                                { "map_radiant_placements", Stats.Placements },
                                { "map_radiant_world_models", Stats.WorldModels },
                                { "map_radiant_model_files", Stats.ModelFiles },
                                { "map_radiant_models_elsewhere", Stats.ModelsElsewhere } };
            Result.Counters.insert(Result.Counters.end(), { { "map_radiant_patches", Stats.Patches },
                                                            { "map_radiant_patches_coloured", Stats.PatchesColoured },
                                                            { "map_radiant_vertex_weights_white", Stats.VertexWeightsWhite },
                                                            { "map_radiant_layers", Stats.Layers },
                                                            { "map_radiant_layers_carried", Stats.LayersCarried },
                                                            { "map_radiant_layers_no_colour", Stats.LayersNoColour },
                                                            { "map_radiant_layers_undrawn", Stats.LayersUndrawn },
                                                            { "map_radiant_layers_no_gdt", Stats.LayersNoGdt },
                                                            { "map_radiant_layered_shared", Shared },
                                                            { "map_radiant_overlay_materials", Stats.OverlayMaterials },
                                                            { "map_radiant_overlay_patches_layer2", Stats.OverlayPatches[0] },
                                                            { "map_radiant_overlay_patches_layer3", Stats.OverlayPatches[1] },
                                                            { "map_radiant_overlay_patches_layer4", Stats.OverlayPatches[2] },
                                                            { "map_radiant_overlay_triangles_in_models_layer2", Stats.OverlayInModels[0] },
                                                            { "map_radiant_overlay_triangles_in_models_layer3", Stats.OverlayInModels[1] },
                                                            { "map_radiant_overlay_triangles_in_models_layer4", Stats.OverlayInModels[2] },
                                                            { "map_radiant_overlay_triangles_unweighted", Stats.OverlayUnweighted },
                                                            { "map_radiant_triangles_degenerate", Stats.TrianglesDegenerate },
                                                            { "map_radiant_triangles_in_models", Stats.TrianglesInModels },
                                                            { "map_radiant_texel_layer_triangles", Stats.TexelLayerTriangles } });
            if (Stats.StockModels || Stats.StockMaterials)
                Result.Counters.insert(Result.Counters.end(), { { "map_radiant_stock_models", Stats.StockModels },
                                                                { "map_radiant_stock_materials", Stats.StockMaterials } });
            if (Stats.EffectsMissing || Stats.GradesMissing)
                Result.Counters.insert(Result.Counters.end(), { { "map_radiant_effects_missing", Stats.EffectsMissing },
                                                                { "map_radiant_grades_missing", Stats.GradesMissing } });
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
        std::string Tree;   // a mod tools tree, "" for beside the map
        bool Patches;       // the drawn world as patches rather than models
        std::string Game;   // the source game's id
        std::string Props;  // the placements kept (map_props)
        std::string Fit;    // the fitting to BO3's tools (map_fit)

        // The GDT with the material and image entries of the materials its model files name, once
        // the run has written the images (some come from other zones): each image hard-linked (else
        // copied) to <map>_radiant/model_export/<map>/images/, which the image entries name.
        static std::function<void(std::map<std::string, size_t>&)> MaterialsLater(const MapScene& Scene, const Materials::IAdapter& Adapter,
                                                                                 RadiantGdt G)
        {
            std::vector<Materials::Description> Described;
            std::vector<char> Layered;   // a compiled layered material (MapScene's LayerWeights): never a decal
            std::vector<const char*> Sorts;   // a blend layer's overlay (MapRadiantLayers.h): its decal layer
            for (const std::string& Name : G.Materials)
            {
                const auto Over = G.Overlays.find(Name);
                if (Over == G.Overlays.end())
                {
                    Described.push_back(DescribeMaterial(Scene, Name, Adapter, G.Root).second);
                    Layered.push_back(Scene.LayerWeights(Name));
                    Sorts.push_back(nullptr);
                    continue;
                }
                // its images under the base semantics, as the game's adapter describes the base; its
                // reveal map in place of its colour's alpha
                Materials::Description D = DescribeMaterial(Scene, Over->second.Mat, Adapter, G.Root).second;
                if (const auto R = Scene.Images.find(Over->second.Reveal); !Over->second.Reveal.empty() && R != Scene.Images.end() && !R->second.File.empty())
                    D.AlphaMap.Image = { RelativeTo(R->second.File, G.Root) };
                D.Multiply = Over->second.Op == 'm';
                Described.push_back(std::move(D));
                Layered.push_back(0);
                Sorts.push_back(LayerSort(Over->second.Layer));
            }
            return [G = std::move(G), Described = std::move(Described), Layered = std::move(Layered),
                    Sorts = std::move(Sorts)](std::map<std::string, size_t>& Counters) {
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
                }, "material_gdt_", G.Prefix);
                std::vector<MaterialGdt::Builder::Item> Items;
                Items.reserve(Described.size());
                for (size_t I = 0; I < Described.size(); I++) Items.push_back({ &Described[I], G.Root, Layered[I] != 0, Sorts[I] });
                B.AddAll(Items);
                std::vector<std::string> Entries = G.ModelEntries;
                const std::vector<std::string> Mine = B.Entries("\n");
                Entries.insert(Entries.end(), Mine.begin(), Mine.end());
                std::error_code Ec;
                fs::create_directories(Images, Ec);
                for (const auto& [File, Name] : Named)
                {
                    const fs::path Target = Images / Name;
                    if (G.InTree)
                    {
                        Counters[ModToolsTree::Place(File, Target) ? "material_gdt_images_copied" : "material_gdt_images_not_copied"]++;
                        continue;
                    }
                    fs::remove(Target, Ec);
                    fs::create_hard_link(File, Target, Ec);
                    if (Ec) fs::copy_file(File, Target, fs::copy_options::overwrite_existing, Ec);
                    Counters[Ec ? "material_gdt_images_not_copied" : "material_gdt_images_copied"]++;
                }
                for (const auto& [K, N] : B.Counters()) Counters[K] += N;
                fs::create_directories(G.GdtFile.parent_path(), Ec);
                if (G.InTree) Counters["material_gdt_already_in_tree"] += ModToolsTree::LeaveOutDefined(G.GdtFile, Entries);
                if (std::string Why; G.InTree && !ModToolsTree::WriteKept(G.GdtFile, Gdt::Document(Entries, "\n"), &Why))
                {
                    // the map's models and materials are in this GDT: without it Radiant shows none
                    Counters["material_gdt_not_written"]++;
                    ModToolsTree::Warn("The map's GDT " + G.GdtFile.string() + " was not written (" + Why +
                                       "): its models and materials will be missing in Radiant and the linker");
                }
                else if (!G.InTree) std::ofstream(G.GdtFile, std::ios::binary) << Gdt::Document(Entries, "\n");
                std::ofstream(fs::path(G.GdtFile).replace_extension(".names.json"), std::ios::binary) << B.NamesJson();
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
    // B's side on the plane Normal . p = Dist
    void AddSide(Brush& B, const V3& Normal, double Dist)
    {
        BrushSide S;
        for (int J = 0; J < 3; J++) S.Normal[J] = (float)Normal[J];
        S.Dist = (float)Dist;
        B.Sides.push_back(S);
    }

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
        AddSide(B, N, Dot(N, P[0]));
        AddSide(B, Mul(N, -1), -(Dot(N, P[0]) - Thick));
        for (int J = 0; J < 3; J++)
        {
            V3 E = Cross(Sub(P[(J + 1) % 3], P[J]), N);
            E = Mul(E, 1.0 / Length(E));
            if (Dot(E, Sub(P[(J + 2) % 3], P[J])) > 0) E = Mul(E, -1);
            AddSide(B, E, Dot(E, P[J]));
        }
        return true;
    }

    // A convex polygon (counter-clockwise around N, on N . p = D) as a brush: its face, the face
    // Thick units behind, a side per edge
    void PolygonPrism(const std::vector<V3>& Loop, const V3& N, double D, double Thick, Brush& B)
    {
        AddSide(B, N, D);
        AddSide(B, Mul(N, -1), -(D - Thick));
        V3 Centre = { 0, 0, 0 };
        for (const V3& P : Loop) Centre = Add(Centre, Mul(P, 1.0 / (double)Loop.size()));
        for (size_t J = 0; J < Loop.size(); J++)
        {
            const V3& A = Loop[J];
            V3 E = Cross(Sub(Loop[(J + 1) % Loop.size()], A), N);
            E = Mul(E, 1.0 / Length(E));
            if (Dot(E, Sub(Centre, A)) > 0) E = Mul(E, -1);
            AddSide(B, E, Dot(E, A));
        }
    }

    // Whether Loop (around N) turns the same way at every corner (straight allowed)
    bool Convex(const std::vector<V3>& Loop, const V3& N)
    {
        for (size_t J = 0; J < Loop.size(); J++)
        {
            const V3& A = Loop[(J + Loop.size() - 1) % Loop.size()];
            const V3& B = Loop[J];
            const V3& C = Loop[(J + 1) % Loop.size()];
            const V3 E1 = Sub(B, A), E2 = Sub(C, B);
            if (Dot(Cross(E1, E2), N) < -1e-6 * Length(E1) * Length(E2)) return false;
        }
        return true;
    }

    // Loop without its straight corners (sine below 1e-5): no two sides on one plane
    std::vector<V3> Corners(const std::vector<V3>& Loop)
    {
        std::vector<V3> Out;
        for (size_t J = 0; J < Loop.size(); J++)
        {
            const V3& A = Loop[(J + Loop.size() - 1) % Loop.size()];
            const V3& B = Loop[J];
            const V3& C = Loop[(J + 1) % Loop.size()];
            const V3 E1 = Sub(B, A), E2 = Sub(C, B);
            if (Length(Cross(E1, E2)) > 1e-5 * Length(E1) * Length(E2)) Out.push_back(B);
        }
        return Out;
    }

    // Collision triangles merged plane by plane into convex polygons: each grows by the triangles
    // across its edges (sharing both corners, facing the same way within MinDot, their third corner
    // on its plane within Tol) while it stays convex, up to MaxCorners corners, its corners put on
    // its plane. Each polygon its triangles' count;
    // Dropped the triangles a prism would not take (Prism's test: too thin).
    struct Polygon
    {
        std::vector<V3> Loop;
        V3 N;
        double D = 0;
        size_t Triangles = 0;
    };
    std::vector<Polygon> CoplanarPolygons(const std::vector<std::array<V3, 3>>& Tris, size_t& Dropped, double Tol = 0.01, double MinDot = 1.0 - 1e-6)
    {
        constexpr size_t MaxCorners = 24;
        using Key = std::array<int64_t, 3>;
        const auto KeyOf = [](const V3& P) { return Key{ std::llround(P[0] * 1000.0), std::llround(P[1] * 1000.0), std::llround(P[2] * 1000.0) }; };
        const auto EdgeOf = [&](const V3& A, const V3& B) {
            const Key Ka = KeyOf(A), Kb = KeyOf(B);
            return Ka < Kb ? std::make_pair(Ka, Kb) : std::make_pair(Kb, Ka);
        };
        std::vector<V3> Normals(Tris.size());
        std::vector<char> Usable(Tris.size(), 0), Used(Tris.size(), 0);
        std::map<std::pair<Key, Key>, std::vector<size_t>> Edges;
        for (size_t T = 0; T < Tris.size(); T++)
        {
            const auto& P = Tris[T];
            const V3 N = Cross(Sub(P[1], P[0]), Sub(P[2], P[0]));
            const double Twice = Length(N);
            double Longest = 0;
            for (int J = 0; J < 3; J++) Longest = (std::max)(Longest, Length(Sub(P[(J + 1) % 3], P[J])));
            if (Twice < 0.01 || Twice / Longest < 0.05) continue;
            Normals[T] = Mul(N, 1.0 / Twice);
            Usable[T] = 1;
            for (int J = 0; J < 3; J++) Edges[EdgeOf(P[J], P[(J + 1) % 3])].push_back(T);
        }
        std::vector<Polygon> Out;
        for (size_t T = 0; T < Tris.size(); T++)
        {
            if (!Usable[T])
            {
                Dropped++;
                continue;
            }
            if (Used[T]) continue;
            Used[T] = 1;
            Polygon G{ { Tris[T][0], Tris[T][1], Tris[T][2] }, Normals[T], Dot(Normals[T], Tris[T][0]), 1 };
            // the loop's corners on G's plane
            const auto Flat = [&G](std::vector<V3> Loop) {
                for (V3& P : Loop) P = Sub(P, Mul(G.N, Dot(G.N, P) - G.D));
                return Loop;
            };
            for (bool Grew = true; Grew && G.Loop.size() < MaxCorners;)
            {
                Grew = false;
                for (size_t E = 0; E < G.Loop.size() && !Grew; E++)
                {
                    const V3& A = G.Loop[E];
                    const V3& B = G.Loop[(E + 1) % G.Loop.size()];
                    const auto It = Edges.find(EdgeOf(A, B));
                    if (It == Edges.end()) continue;
                    for (const size_t U : It->second)
                    {
                        if (Used[U] || Dot(Normals[U], G.N) < MinDot) continue;
                        const Key Ka = KeyOf(A), Kb = KeyOf(B);
                        const V3* C = nullptr;
                        for (const V3& Q : Tris[U])
                            if (const Key K = KeyOf(Q); K != Ka && K != Kb) C = &Q;
                        if (!C || std::fabs(Dot(G.N, *C) - G.D) > Tol) continue;
                        std::vector<V3> Next = G.Loop;
                        Next.insert(Next.begin() + (std::ptrdiff_t)E + 1, *C);
                        if (!Convex(Flat(Next), G.N)) continue;
                        G.Loop = std::move(Next);
                        Used[U] = 1;
                        G.Triangles++;
                        Grew = true;
                        break;
                    }
                }
            }
            G.Loop = Corners(Flat(G.Loop));
            if (G.Loop.size() >= 3) Out.push_back(std::move(G));
            else Dropped += G.Triangles;
        }
        return Out;
    }

    // Collision triangles as terrain patches: paired into quads across their longest edge (facing
    // the same way), the quads grown into grids (a cell's right and top neighbours sharing its edges
    // and one another's), MaxCells by MaxCells cells at most. A grid's Ctrl[x][y] (x across its
    // rows, y along them, as the .map writes them: a patch's triangles face along (Ctrl[x + 1][y] -
    // Ctrl[x][y]) x (Ctrl[x][y + 1] - Ctrl[x][y]), as the collision triangles do: the stock
    // prefabs' terrain and clip patches, Black Ops III's traces) and Turned[x][y], the cell from
    // Ctrl[x][y] split from it to Ctrl[x + 1][y + 1] ("f 1") rather than across: each cell the
    // game's two triangles. The triangles no quad takes are left in Rest, a sliver (Sliver, below
    // MinArea) among them.
    struct TerrainGrid
    {
        std::vector<std::vector<V3>> Ctrl;
        std::vector<std::vector<char>> Turned;
    };
    // a triangle cod2map drops from a patch (asserting "tot_faces == triCount", AW): under MinArea
    // square units, or thinner than a tenth of a unit; none when MinArea is 0
    bool Sliver(const V3& A, const V3& B, const V3& C, double MinArea)
    {
        if (!(MinArea > 0)) return false;
        const double Area = Length(Cross(Sub(B, A), Sub(C, A))) / 2;
        const double Longest = (std::max)({ Length(Sub(B, A)), Length(Sub(C, B)), Length(Sub(A, C)) });
        return Area < MinArea || 2 * Area / Longest < 0.1;
    }
    std::vector<TerrainGrid> TerrainGrids(const std::vector<std::array<V3, 3>>& Tris, std::vector<std::array<V3, 3>>& Rest, double MinArea)
    {
        constexpr size_t MaxCells = 16;
        using Key = std::array<int64_t, 3>;
        const auto KeyOf = [](const V3& P) { return Key{ std::llround(P[0] * 1000.0), std::llround(P[1] * 1000.0), std::llround(P[2] * 1000.0) }; };
        using Edge = std::pair<Key, Key>;
        const auto EdgeOf = [](const Key& A, const Key& B) { return A < B ? Edge(A, B) : Edge(B, A); };
        std::map<Key, V3> Points;
        std::vector<std::array<Key, 3>> Corners(Tris.size());
        std::vector<V3> Normals(Tris.size());
        std::map<Edge, std::vector<size_t>> Edges;
        for (size_t T = 0; T < Tris.size(); T++)
        {
            for (int J = 0; J < 3; J++)
            {
                Corners[T][J] = KeyOf(Tris[T][J]);
                Points.emplace(Corners[T][J], Tris[T][J]);
            }
            Normals[T] = Cross(Sub(Tris[T][1], Tris[T][0]), Sub(Tris[T][2], Tris[T][0]));
            for (int J = 0; J < 3; J++) Edges[EdgeOf(Corners[T][J], Corners[T][(J + 1) % 3])].push_back(T);
        }
        // the quads: each triangle, longest edge first, with an unpaired one across that edge
        const auto Longest = [&](size_t T) {
            int Best = 0;
            double Len = -1;
            for (int J = 0; J < 3; J++)
                if (const double L = Length(Sub(Tris[T][(J + 1) % 3], Tris[T][J])); L > Len) Len = L, Best = J;
            return std::make_pair(Len, Best);
        };
        std::vector<size_t> Order(Tris.size());
        for (size_t T = 0; T < Order.size(); T++) Order[T] = T;
        std::stable_sort(Order.begin(), Order.end(), [&](size_t A, size_t B) { return Longest(A).first > Longest(B).first; });
        std::vector<char> Paired(Tris.size(), 0);
        std::vector<std::array<Key, 4>> Loops;     // corners counter-clockwise as the triangles turn: p, r, q, s
        std::vector<std::pair<Key, Key>> Diagonals;
        for (size_t T = 0; T < Tris.size(); T++)
            if (Sliver(Tris[T][0], Tris[T][1], Tris[T][2], MinArea)) Paired[T] = 2;   // to Rest
        for (const size_t T : Order)
        {
            if (Paired[T]) continue;
            const int J = Longest(T).second;
            const Key P = Corners[T][J], Q = Corners[T][(J + 1) % 3], R = Corners[T][(J + 2) % 3];
            if (P == Q || Q == R || R == P) continue;
            for (const size_t U : Edges[EdgeOf(P, Q)])
            {
                if (U == T || Paired[U] || Dot(Normals[U], Normals[T]) <= 0) continue;
                Key S{};
                int Others = 0;
                for (const Key& K : Corners[U])
                    if (K != P && K != Q) S = K, Others++;
                if (Others != 1 || S == R) continue;
                Paired[T] = Paired[U] = 1;
                // T turns P, Q, R and U Q, P, S: the quad turns P, S, Q, R
                Loops.push_back({ P, S, Q, R });
                Diagonals.push_back(EdgeOf(P, Q));
                break;
            }
        }
        for (size_t T = 0; T < Tris.size(); T++)
            if (Paired[T] != 1) Rest.push_back(Tris[T]);
        std::map<Edge, std::vector<size_t>> QuadEdges;
        for (size_t Q = 0; Q < Loops.size(); Q++)
            for (int K = 0; K < 4; K++) QuadEdges[EdgeOf(Loops[Q][K], Loops[Q][(K + 1) % 4])].push_back(Q);
        // the cell across side 1 (right) or 2 (top) of cell (Q, K): K its bottom-left corner
        using Cell = std::pair<size_t, int>;
        const auto Across = [&](const Cell& C, int Side, Cell& Out) {
            const auto& L = Loops[C.first];
            const Key& A = L[(C.second + Side) % 4];
            const Key& B = L[(C.second + Side + 1) % 4];
            for (const size_t Q : QuadEdges[EdgeOf(A, B)])
            {
                if (Q == C.first) continue;
                const auto& M = Loops[Q];
                for (int K = 0; K < 4; K++)
                    if (Side == 1 ? M[K] == A && M[(K + 3) % 4] == B : M[K] == B && M[(K + 1) % 4] == A)
                    {
                        Out = { Q, K };
                        return true;
                    }
            }
            return false;
        };
        // grids grown from the quads with the fewest neighbours (corners, edges) first, the largest
        // of the four ways round kept
        std::vector<size_t> Seeds(Loops.size()), Neighbours(Loops.size(), 0);
        for (size_t Q = 0; Q < Loops.size(); Q++)
        {
            Seeds[Q] = Q;
            for (int K = 0; K < 4; K++) Neighbours[Q] += QuadEdges[EdgeOf(Loops[Q][K], Loops[Q][(K + 1) % 4])].size() - 1;
        }
        std::stable_sort(Seeds.begin(), Seeds.end(), [&](size_t A, size_t B) { return Neighbours[A] < Neighbours[B]; });
        std::vector<char> Used(Loops.size(), 0);
        std::vector<TerrainGrid> Out;
        for (const size_t Seed : Seeds)
        {
            if (Used[Seed]) continue;
            std::vector<std::vector<Cell>> Best;
            size_t BestCells = 0;
            for (int K0 = 0; K0 < 4; K0++)
            {
                std::set<size_t> Taken = { Seed };
                std::vector<std::vector<Cell>> Rows(1, { { Seed, K0 } });
                for (Cell Next; Rows[0].size() < MaxCells && Across(Rows[0].back(), 1, Next) && !Used[Next.first] && Taken.insert(Next.first).second;)
                    Rows[0].push_back(Next);
                while (Rows.size() < MaxCells)
                {
                    std::vector<Cell> Up;
                    std::set<size_t> Adding;
                    bool Fits = true;
                    for (const Cell& C : Rows.back())
                    {
                        Cell T;
                        if (!Across(C, 2, T) || Used[T.first] || Taken.count(T.first) || !Adding.insert(T.first).second)
                        {
                            Fits = false;
                            break;
                        }
                        Up.push_back(T);
                    }
                    for (size_t I = 0; Fits && I + 1 < Up.size(); I++)
                    {
                        Cell R;
                        Fits = Across(Up[I], 1, R) && R == Up[I + 1];
                    }
                    if (!Fits) break;
                    Taken.insert(Adding.begin(), Adding.end());
                    Rows.push_back(std::move(Up));
                }
                if (Taken.size() > BestCells) BestCells = Taken.size(), Best = std::move(Rows);
            }
            // cells (i, along) of rows of cells (j, across), the .map's rows along i: Ctrl[i][j] the
            // bottom-left corner of cell (i, j). (row 1 - row 0) x (column 1 - column 0) is then
            // (Br - Bl) x (Tl - Bl), the way the triangles face (Ctrl[j][i], rows across, wrote
            // every collision patch facing down: traces from above went through mp_nuked's ground)
            const size_t Width = Best[0].size(), Height = Best.size();
            TerrainGrid G;
            G.Ctrl.assign(Width + 1, std::vector<V3>(Height + 1));
            G.Turned.assign(Width + 1, std::vector<char>(Height + 1, 0));
            for (size_t J = 0; J < Height; J++)
                for (size_t I = 0; I < Width; I++)
                {
                    const auto& [Q, K] = Best[J][I];
                    Used[Q] = 1;
                    const auto& L = Loops[Q];
                    const Key& Bl = L[K];
                    const Key& Br = L[(K + 1) % 4];
                    const Key& Tr = L[(K + 2) % 4];
                    const Key& Tl = L[(K + 3) % 4];
                    G.Ctrl[I][J] = Points[Bl];
                    G.Ctrl[I + 1][J] = Points[Br];
                    G.Ctrl[I][J + 1] = Points[Tl];
                    G.Ctrl[I + 1][J + 1] = Points[Tr];
                    G.Turned[I][J] = Diagonals[Q] == EdgeOf(Bl, Tr);
                }
            Out.push_back(std::move(G));
        }
        return Out;
    }

    // Boxes under floor triangles: the plane split in square cells, the triangles clipped to each;
    // per cell and layer of floor a box two units deep, its top a unit under the layer's lowest point
    // (a whole unit, so that flat floors give equal tops), equal tops merged into rectangles along
    // x then y. Cells of Finest units, twice as wide until at most Most boxes; still too many at
    // Coarsest, those farthest from play (Far) left out.
    struct FloorBoxing
    {
        static constexpr double Finest = 16.0, Coarsest = 4096.0;
        double Cell = Finest;
        std::vector<std::array<double, 6>> Boxes;   // low corner, high corner
        size_t LeftOut = 0;
    };
    FloorBoxing FloorBoxes(const std::vector<std::array<V3, 3>>& Tris, double Most, const std::function<double(const V3&)>& Far, const V3& Mid)
    {
        FloorBoxing R;
        for (R.Cell = FloorBoxing::Finest;; R.Cell *= 2)
        {
            const double C = R.Cell;
            std::map<std::pair<int64_t, int64_t>, std::vector<std::array<double, 3>>> Parts;   // cell -> the triangles' parts in it: lowest z, highest z, area
            for (const auto& T : Tris)
            {
                double Lo[3] = { 1e300, 1e300, 1e300 }, Hi[2] = { -1e300, -1e300 };
                for (const V3& P : T)
                {
                    for (int A = 0; A < 3; A++) Lo[A] = (std::min)(Lo[A], P[A]);
                    for (int A = 0; A < 2; A++) Hi[A] = (std::max)(Hi[A], P[A]);
                }
                // the cells the triangle itself covers (clipped to each, xy), each its part's lowest z
                for (int64_t X = (int64_t)std::floor(Lo[0] / C); X <= (int64_t)std::floor(Hi[0] / C); X++)
                    for (int64_t Y = (int64_t)std::floor(Lo[1] / C); Y <= (int64_t)std::floor(Hi[1] / C); Y++)
                    {
                        std::vector<V3> Poly(T.begin(), T.end());
                        const double Bound[4] = { X * C, (X + 1) * C, Y * C, (Y + 1) * C };
                        for (int E = 0; E < 4 && Poly.size() >= 3; E++)
                        {
                            const int A = E / 2;
                            const double Edge = Bound[E], Sign = E % 2 ? -1.0 : 1.0;   // keep Sign * (p[A] - Edge) >= 0
                            std::vector<V3> Next;
                            for (size_t I = 0; I < Poly.size(); I++)
                            {
                                const V3& P = Poly[I];
                                const V3& Q = Poly[(I + 1) % Poly.size()];
                                const double Dp = Sign * (P[A] - Edge), Dq = Sign * (Q[A] - Edge);
                                if (Dp >= 0) Next.push_back(P);
                                if ((Dp >= 0) != (Dq >= 0)) Next.push_back(Add(P, Mul(Sub(Q, P), Dp / (Dp - Dq))));
                            }
                            Poly = std::move(Next);
                        }
                        if (Poly.size() < 3) continue;
                        double Area = 0, Z = 1e300, Zh = -1e300;
                        for (size_t I = 0; I < Poly.size(); I++)
                        {
                            const V3& P = Poly[I];
                            const V3& Q = Poly[(I + 1) % Poly.size()];
                            Area += P[0] * Q[1] - Q[0] * P[1];
                            Z = (std::min)(Z, P[2]), Zh = (std::max)(Zh, P[2]);
                        }
                        if (std::abs(Area) < 1e-3) continue;
                        Parts[{ X, Y }].push_back({ Z, Zh, std::abs(Area) / 2 });
                    }
            }
            // by top, then row (y), then x: runs along x, then runs stacked along y
            std::map<int64_t, std::map<int64_t, std::vector<int64_t>>> ByTop;
            // a cell's parts in layers (one starting more than Gap over the highest of the one under it):
            // the lowest layer a box, a higher one only when its parts fill the cell (a box over the
            // open edge of a raised floor would lid the floor under it)
            constexpr double Gap = 8.0;
            for (auto& [Cell, Of] : Parts)
            {
                std::sort(Of.begin(), Of.end());
                double Low = Of[0][0], High = Of[0][1], Area = 0;
                bool Lowest = true;
                const auto Flush = [&]() {
                    if (Lowest || Area >= 0.98 * C * C) ByTop[(int64_t)std::floor(Low) - 1][Cell.second].push_back(Cell.first);
                    Lowest = false;
                };
                for (const auto& P : Of)
                {
                    if (P[0] > High + Gap)
                    {
                        Flush();
                        Low = P[0], High = P[1], Area = 0;
                    }
                    High = (std::max)(High, P[1]);
                    Area += P[2];
                }
                Flush();
            }
            R.Boxes.clear();
            for (auto& [Top, Rows] : ByTop)
            {
                struct Run { int64_t X0, X1, Y0, Y1; };
                std::vector<Run> Open, Done;
                int64_t LastY = 0;
                bool First = true;
                for (auto& [Y, Xs] : Rows)
                {
                    std::sort(Xs.begin(), Xs.end());
                    std::vector<std::pair<int64_t, int64_t>> Runs;
                    for (const int64_t X : Xs)
                        if (!Runs.empty() && Runs.back().second + 1 == X) Runs.back().second = X;
                        else Runs.push_back({ X, X });
                    std::vector<Run> Next;
                    for (const auto& [X0, X1] : Runs)
                    {
                        bool Grown = false;
                        if (!First && LastY + 1 == Y)
                            for (auto& O : Open)
                                if (O.X0 == X0 && O.X1 == X1 && O.Y1 == LastY)
                                {
                                    Next.push_back({ X0, X1, O.Y0, Y });
                                    O.Y1 = INT64_MIN;   // taken
                                    Grown = true;
                                    break;
                                }
                        if (!Grown) Next.push_back({ X0, X1, Y, Y });
                    }
                    for (const Run& O : Open)
                        if (O.Y1 != INT64_MIN) Done.push_back(O);
                    Open = std::move(Next);
                    LastY = Y;
                    First = false;
                }
                Done.insert(Done.end(), Open.begin(), Open.end());
                for (const Run& B : Done)
                    R.Boxes.push_back({ B.X0 * C, B.Y0 * C, (double)Top - 2, (B.X1 + 1) * C, (B.Y1 + 1) * C, (double)Top });
            }
            size_t InPlay = 0;
            for (const auto& B : R.Boxes) InPlay += Far(V3{ (B[0] + B[3]) / 2, (B[1] + B[4]) / 2, B[5] }) <= 0;
            if ((double)InPlay <= Most || R.Cell >= FloorBoxing::Coarsest) break;
        }
        if ((double)R.Boxes.size() > Most)
        {
            const size_t Keep = Most > 0 ? (size_t)Most : 0;
            std::vector<std::tuple<double, double, size_t>> Order;
            for (size_t I = 0; I < R.Boxes.size(); I++)
            {
                const auto& B = R.Boxes[I];
                const V3 C = { (B[0] + B[3]) / 2, (B[1] + B[4]) / 2, B[5] };
                Order.push_back({ Far(C), Length(Sub(C, Mid)), I });
            }
            std::stable_sort(Order.begin(), Order.end());
            std::vector<std::array<double, 6>> Kept;
            for (size_t I = 0; I < Keep && I < Order.size(); I++) Kept.push_back(R.Boxes[std::get<2>(Order[I])]);
            R.LeftOut = R.Boxes.size() - Kept.size();
            R.Boxes = std::move(Kept);
        }
        return R;
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

bool WriteRadiant(const MapScene& Scene, const std::string& MapPath, RadiantStats& Stats, std::string& Error, RadiantGdt* ForGdt,
                  const std::string& Tree, bool Patches, const std::string& Game, const WriteProgress& Progress, const std::string& Props,
                  const std::string& Fit)
{
    // the stages as they go (their times in the run's log); false once the run is cancelled
    const auto Stage = [&](const char* Name, size_t Done = 0, size_t Count = 0) {
        if (Tell(Progress, Name, Done, Count)) return true;
        Error = "cancelled";
        return false;
    };
    const bool InTree = !Tree.empty();
    // what the .map is for (RadiantTarget.h): in a tree a level the mod tools build
    const std::string Stem = ExportNaming::SafeFileName(fs::path(MapPath).stem().string());
    // a map of the game the tree's tools are for: their own assets, named as they are (RadiantTarget.h)
    std::unordered_map<std::string, std::string> Stock = Bo3StockAssets(Scene, Tree);
    auto StockMaterials = std::make_shared<std::unordered_set<std::string>>();
    for (const auto& [Name, Gdf] : Stock)
        if (Gdf == "material.gdf") StockMaterials->insert(Name);
    const std::unique_ptr<IRadiantTarget> Target =
        InTree ? Bo3ModToolsTarget(ExportNaming::SafeFileName(Game), Stem, std::move(Stock), Tree) : PlainTarget(Stem);
    const std::string& Map = Target->MapName();
    const fs::path Root = InTree ? fs::path(Tree) : fs::path(MapPath).parent_path() / (Map + "_radiant");
    // the models' folder below model_export, as the GDT names their files
    const std::string Folder = InTree ? "graywolf/" + Map : Map;
    const fs::path Path = InTree ? Root / "map_source" / "graywolf" / (Map + ".map") : fs::path(MapPath);
    const fs::path GdtFile = Root / "source_data" / ((InTree ? "graywolf_" : "") + Map + ".gdt");
    const fs::path ModelDir = Root / "model_export" / fs::path(Folder);
    const std::string ModelExtension = Target->ModelExtension();
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

    // the entities the target takes, as it rewrites them; the names it gives the map's assets
    if (ForGdt)
    {
        ForGdt->Prefix = Target->NamePrefix();
        if (!StockMaterials->empty()) ForGdt->Stock = StockMaterials;
        auto Same = std::make_shared<const std::map<std::string, std::string>>(SharedFirstLayers(Scene));
        if (!Same->empty()) ForGdt->Same = std::move(Same);
    }
    // a model the map's export writes, by the name its GDT entry has
    // a model's (its part's, ModelParts) GDT entry: the target's name for one the export writes
    const auto ModelName = [&](const std::string& Model, size_t Part = 0) {
        if (Target->Stock(Model, "xmodel.gdf")) return Model;
        return Scene.Models.count(Model) ? Target->ModelName(PartName(Model, Part)) : PartName(Model, Part);
    };
    std::erase_if(Ents, [&](Entity& E) { return !Target->Admit(E, Scene, Stats); });
    // fitting the map to Black Ops III's tools (a target that Fits, the map_fit setting): at Room of each
    // limit ("fit" 0.85, "strict" 0.7), what a player misses least first; "off": only told
    const bool Fitting = Target->Fits() && Fit != "off";
    const double Room = Fit == "strict" ? 0.7 : 0.85;
    // Radiant's 256 exploders ("This map will fail to link"): past Room of them, the exploders of
    // the highest numbers let go (their entities kept, without "script_exploder")
    if (const size_t Most = (size_t)(Room * Target->HardLimits().Exploders); Fitting && Most)
    {
        std::set<std::pair<long long, std::string>> Numbers;   // by number, then name
        for (const Entity& E : Ents)
            if (const auto X = E.find("script_exploder"); X != E.end()) Numbers.insert({ std::atoll(X->second.c_str()), X->second });
        if (Numbers.size() > Most)
        {
            std::set<std::string> Kept;
            for (const auto& [N, Name] : Numbers)
                if (Kept.size() < Most) Kept.insert(Name);
            for (Entity& E : Ents)
                if (const auto X = E.find("script_exploder"); X != E.end() && !Kept.count(X->second)) E.erase(X);
            Stats.ExplodersDropped = Numbers.size() - Kept.size();
            Stats.Warnings.push_back(std::to_string(Numbers.size()) + " exploders, the " + std::to_string(Stats.ExplodersDropped) +
                                     " of the highest numbers let go (Radiant: 256 at most or the map fails to link)");
        }
    }
    // where players start (info_player_start*, mp_*spawn*), grown by the target's margins: what a
    // crop of the collision keeps (a triangle with a corner in it)
    struct
    {
        bool Found = false;
        double Lo[3] = { 1e30, 1e30, 1e30 }, Hi[3] = { -1e30, -1e30, -1e30 };
        bool Holds(const std::array<V3, 3>& T) const
        {
            for (const V3& P : T)
                if (P[0] >= Lo[0] && P[0] <= Hi[0] && P[1] >= Lo[1] && P[1] <= Hi[1] && P[2] >= Lo[2] && P[2] <= Hi[2]) return true;
            return false;
        }
    } PlayArea;
    for (const Entity& E : Ents)
    {
        const auto Class = E.find("classname");
        if (Class == E.end()) continue;
        const std::string& C = Class->second;
        float O[3];
        if (!(C.rfind("info_player_start", 0) == 0 || (C.rfind("mp_", 0) == 0 && C.find("spawn") != std::string::npos)) || !EntityFloats(E, "origin", O, 3))
            continue;
        PlayArea.Found = true;
        for (int A = 0; A < 3; A++) PlayArea.Lo[A] = (std::min)(PlayArea.Lo[A], (double)O[A]), PlayArea.Hi[A] = (std::max)(PlayArea.Hi[A], (double)O[A]);
    }
    double StartLo[3], StartHi[3];   // the starts themselves, no margin
    for (int A = 0; A < 3; A++) StartLo[A] = PlayArea.Lo[A], StartHi[A] = PlayArea.Hi[A];
    for (int A = 0; A < 3; A++)
    {
        const double Margin = A < 2 ? Target->PlayMargin().Across : Target->PlayMargin().Up;
        PlayArea.Lo[A] -= Margin, PlayArea.Hi[A] += Margin;
    }
    // a point within the target's world (WorldLimit on every axis; a coordinate not a number is
    // judged where it is used, as before)
    const double Limit = Target->WorldLimit();
    const auto Within = [Limit](const auto& P) {
        return !(std::fabs((double)P[0]) > Limit) && !(std::fabs((double)P[1]) > Limit) && !(std::fabs((double)P[2]) > Limit);
    };

    // where the level ends (MapRadiantBound.h): its world cut to that box while the map is written,
    // the entities standing outside it left out. The cut world is swapped into the scene and given
    // back on the way out: the scene is the export's own, written by one writer at a time, its
    // models and images too large to copy for a few triangles.
    const SkyBound Bound = UsefulBound(Scene, Ents, PlayArea.Found, StartLo, StartHi, Limit);
    BoundedWorld Bounded;
    struct Swapped
    {
        MapScene& S;
        BoundedWorld& W;
        bool On = false;
        void Flip()
        {
            std::swap(S.Objects, W.Objects);
            std::swap(S.Collision.Vertices, W.CollisionVertices);
            std::swap(S.Collision.Triangles, W.CollisionTriangles);
            std::swap(S.Collision.TriangleMaterials, W.CollisionMaterials);
            std::swap(S.StaticInstances, W.StaticInstances);
            std::swap(S.DynamicInstances, W.DynamicInstances);
            std::swap(S.SplinedInstances, W.SplinedInstances);
            On = !On;
        }
        ~Swapped()
        {
            if (On) Flip();
        }
    } Swap{ const_cast<MapScene&>(Scene), Bounded };
    if (Bound.Cuts)
    {
        if (!Stage("cutting the world to its sky box")) return false;
        CutToBound(Scene, Bound, Bounded);
        Stats.BeyondSkyBox = Bounded.TrianglesCut + Bounded.CollisionCut;
        Stats.ClippedToSkyBox = Bounded.TrianglesClipped + Bounded.CollisionClipped;
        Stats.BeyondSkyBoxPlacements = Bounded.PlacementsCut;
        Stats.BeyondSkyBoxEntities = (size_t)std::erase_if(Ents, [&](const Entity& E) {
            const auto Class = E.find("classname");
            float O[3];
            if ((Class != E.end() && Class->second == "worldspawn") || !EntityFloats(E, "origin", O, 3) || !Within(O)) return false;
            return !Bound.Holds(O);
        });
        Swap.Flip();
    }
    Stats.SkyBoxReach = (size_t)std::llround(Bound.Reach);

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
    if (!Stage("reading the drawn world")) return false;
    const ToolNames Tools(Scene.Collision);
    const CoveredTriangles Cover(Scene);
    Stats.TrianglesCovered = Cover.Count;
    DrawnWorld Drawn(Scene, Stats.Textures, [&Cover](size_t O, size_t Si, size_t K) { return Cover.Covered(O, Si, K); });
    Stats.Textures.TrianglesDrawn = Drawn.Count();
    // a patch per triangle only for a world Radiant can hold so: each patch is an object of its own
    // (MW4's mp_rex_transit, 5.8 million triangles: a 1.3 GB .map that Radiant could not open in
    // 32 GB; and cod2map stops at 3 072 000 drawn triangles, MAX_MAP_TRIANGLES). Past MostPatches,
    // the world's models as without patches.
    if (Patches && Drawn.Count() > MostPatches)
    {
        Stats.PatchesRefused = Drawn.Count();
        Stats.Warnings.push_back("a patch per triangle turned down: " + std::to_string(Drawn.Count()) + " drawn triangles, " +
                                 std::to_string(MostPatches) + " at most as patches (Radiant could not hold the .map); written as the world's models");
        Patches = false;
    }
    // the layered materials' later layers, drawn over them when the GDT defines their materials
    const BlendLayers Layers(Scene);
    Overlaying Ov;
    if (ForGdt) Ov.Layers = &Layers;
    // MAX_MAP_MATERIALS (11776) and the linker's 65536 assets of a fast file (a material about four:
    // itself, its images): the materials the map may name (the world's and the models', the
    // overlays'); past Room, the overlays (the blend layers drawn over the world) left out first
    if (Fitting && ForGdt)
    {
        std::set<std::string> Named;
        for (const Mesh& M : Scene.Objects)
            for (const Surface& S : M.Surfaces) Named.insert(S.Material);
        for (const auto& [Name, Model] : Scene.Models)
            for (const Surface& S : Model.Surfaces) Named.insert(S.Material);
        const double Most = (std::min)(Room * Target->HardLimits().Materials, Room * Target->HardLimits().Assets / 4.0);
        if ((double)(Named.size() + Layers.Layers) > Most && Layers.Layers)
        {
            Ov.Layers = nullptr;
            Stats.Warnings.push_back(std::to_string(Named.size()) + " materials and " + std::to_string(Layers.Layers) +
                                     " blend layers: the layers left out (Black Ops III's tools: " + std::to_string(Target->HardLimits().Materials) +
                                     " materials, " + std::to_string(Target->HardLimits().Assets) + " assets in a fast file)");
        }
    }
    Stats.Layers = Layers.Layers, Stats.LayersNoColour = Layers.LayersNoColour;
    for (const Mesh& M : Scene.Objects)
        for (const Surface& S : M.Surfaces)
            if (Scene.LayerWeights(S.Material))
                for (const Vertex& V : S.Vertices)
                    Stats.VertexWeightsWhite += V.Color[0] != 255 || V.Color[1] != 255 || V.Color[2] != 255 || V.Color[3] != 255;
    if (!Stage("the world's solid brushes")) return false;
    const Solids SolidBrushes(Scene.Collision);
    const DrawnWorld::HiddenTest Hidden = [&SolidBrushes](const V3& P) { return SolidBrushes.Inside(P); };
    const Texturing Tex{ &Drawn, ForGdt, 0, &Stats.Textures, &Hidden, Fitting };
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
    // the clip map's brushes, those of more sides than the target's compiler takes left out
    std::vector<Brush> FewSided;
    const std::vector<Brush>* WorldBrushes = &Scene.Collision.Brushes;
    if (const size_t Most = Target->MaxBrushSides();
        std::any_of(WorldBrushes->begin(), WorldBrushes->end(), [Most](const Brush& B) { return B.Sides.size() > Most; }))
    {
        for (const Brush& B : *WorldBrushes)
            if (B.Sides.size() <= Most) FewSided.push_back(B);
            else Stats.BrushesManySided++;
        WorldBrushes = &FewSided;
    }
    // ---- the brushes fitted to Black Ops III's compiler (graywolf-tools/docs/bo3-modtools-limits.md):
    // past Room of a limit, what counts less to a player goes first, each step told in a warning
    const IRadiantTarget::Limits Lim = Target->HardLimits();
    // how far a point is from where players play (0 within it); without starts, from the
    // collision's middle
    V3 Middle = { 0, 0, 0 };
    if (!PlayArea.Found && !Scene.Collision.Vertices.empty())
    {
        const auto& Cv = Scene.Collision.Vertices;
        for (size_t I = 0; I + 2 < Cv.size(); I += 3)
            for (int A = 0; A < 3; A++) Middle[A] += Cv[I + A] / (double)(Cv.size() / 3);
    }
    const auto Far = [&](const V3& P) {
        if (!PlayArea.Found) return Length(Sub(P, Middle));
        double D2 = 0;
        for (int A = 0; A < 3; A++)
        {
            const double D = P[A] < PlayArea.Lo[A] ? PlayArea.Lo[A] - P[A] : P[A] > PlayArea.Hi[A] ? P[A] - PlayArea.Hi[A] : 0.0;
            D2 += D * D;
        }
        return std::sqrt(D2);
    };
    // the brush models the entities name, with their brushes of few enough sides
    std::vector<size_t> ModelsNamed;
    {
        std::set<size_t> Seen;
        for (const Entity& E : Ents)
        {
            const auto Model = E.find("model");
            if (&E == World || Model == E.end() || Model->second.empty() || Model->second[0] != '*') continue;
            const size_t N = (size_t)std::strtoul(Model->second.c_str() + 1, nullptr, 10);
            if (N < Scene.Collision.Models.size() && !Scene.Collision.Models[N].empty() && Seen.insert(N).second) ModelsNamed.push_back(N);
        }
    }
    const auto FewEnough = [&](const Brush& B) { return B.Sides.size() <= Target->MaxBrushSides(); };
    // each brush's planes as cod2map makes them (MapRadiantPlanes.h); the world's, then the models'
    // (where they stand), then the triggers'
    struct Pooled
    {
        const Brush* B = nullptr;
        size_t Model = 0;   // 0: the world's
        V3 Centre = { 0, 0, 0 };
        CompilerBrush Planes;
    };
    std::vector<Pooled> Pool;
    for (const Brush& B : *WorldBrushes) Pool.push_back({ &B, 0 });
    // where each brush model stands (its entity's origin, then its axis): its brushes are counted as
    // written there
    std::map<size_t, std::array<float, 12>> ModelPlace;
    for (const Entity& E : Ents)
        if (const auto M = E.find("model"); M != E.end() && !M->second.empty() && M->second[0] == '*')
        {
            float O[3] = { 0, 0, 0 }, Ang[3] = { 0, 0, 0 }, Axis[9];
            EntityFloats(E, "origin", O, 3);
            EntityFloats(E, "angles", Ang, 3);
            AnglesToAxis(Ang, Axis);
            std::array<float, 12> P;
            for (int K = 0; K < 3; K++) P[K] = O[K];
            for (int K = 0; K < 9; K++) P[3 + K] = Axis[K];
            ModelPlace.try_emplace((size_t)std::strtoul(M->second.c_str() + 1, nullptr, 10), P);
        }
    for (const size_t N : ModelsNamed)
        for (const Brush& B : Scene.Collision.Models[N])
            if (FewEnough(B)) Pool.push_back({ &B, N });
    const size_t Movable = Pool.size();
    for (const auto& Volumes : Scene.Collision.Triggers)
        for (const Brush& B : Volumes)
            if (FewEnough(B)) Pool.push_back({ &B, (size_t)-1 });
    {
        struct None {};
        ParallelFor<None>(Pool.size(), 64, [&](size_t I, None&) {
            Brush Moved = *Pool[I].B;
            if (const auto At = ModelPlace.find(Pool[I].Model); Pool[I].Model && At != ModelPlace.end())
                for (auto& S : Moved.Sides)
                {
                    // as PrepareBrush moves it: N' = Axis^T N, D' = D + N' . O
                    const float* Axis = At->second.data() + 3;
                    const float N[3] = { S.Normal[0], S.Normal[1], S.Normal[2] };
                    for (int K = 0; K < 3; K++) S.Normal[K] = N[0] * Axis[K] + N[1] * Axis[3 + K] + N[2] * Axis[6 + K];
                    S.Dist += S.Normal[0] * At->second[0] + S.Normal[1] * At->second[1] + S.Normal[2] * At->second[2];
                }
            Pool[I].Planes = CompilerPlanes(Moved);
            const std::vector<BrushFace> Faces = BrushFaces(Moved);
            size_t N = 0;
            for (const BrushFace& F : Faces)
                for (const V3& P : F.Points) Pool[I].Centre = Add(Pool[I].Centre, P), N++;
            if (N) Pool[I].Centre = Mul(Pool[I].Centre, 1.0 / (double)N);
        });
    }
    // Converted: world brushes written as collision patches instead (no plane, no node);
    // Stripped: brush models left their drawn triangles only
    std::vector<char> Converted(WorldBrushes->size(), 0);
    std::set<size_t> Stripped;
    const auto Count = [&]() {
        // a brush model's brushes are planes twice to cod2map: as written (where it stands) and
        // found again at its origin (0x1400a45c0)
        std::vector<const CompilerBrush*> Kept;
        std::vector<CompilerPlane> Again;
        for (size_t I = 0; I < Pool.size(); I++)
        {
            if (I < Converted.size() ? Converted[I] : Stripped.count(Pool[I].Model) != 0) continue;
            Kept.push_back(&Pool[I].Planes);
            if (const auto At = ModelPlace.find(Pool[I].Model); I >= Converted.size() && At != ModelPlace.end())
                for (const auto* List : { &Pool[I].Planes.Sides, &Pool[I].Planes.Bevels })
                    for (CompilerPlane P : *List)
                    {
                        P.D -= P.N[0] * At->second[0] + P.N[1] * At->second[1] + P.N[2] * At->second[2];
                        Again.push_back(P);
                    }
        }
        return CountPlanes(Kept, Again);
    };
    // the count planned here runs about 6 % under the .map's (the points written on a grid, the
    // triggers counted as they stand): planned counts taken 10 % higher
    constexpr double Planned = 1.1;
    const auto OverPlanes = [&](const PlaneCount& C) {
        return Fitting && ((double)C.Slots * Planned > Room * Lim.PlaneSlots || (double)C.SidePlanes * Planned > Room * 0x20000 ||
                           (double)C.Brushes + 6 > Room * Lim.Brushes || (double)C.CollisionSides * Planned > Room * Lim.BrushSides);
    };
    PlaneCount Planes = Count();
    const PlaneCount PlanesBefore = Planes;
    {
        // the world's brushes by distance from play, farthest first (those outside it, then those
        // inside), a twentieth of the world at a time; then, last, the brush models (most planes
        // first): doors, lifts and movers keep their collision while the world can give
        std::vector<size_t> Outside, Inside;
        for (size_t I = 0; I < Converted.size(); I++) (Far(Pool[I].Centre) > 0 ? Outside : Inside).push_back(I);
        const auto Farthest = [&](std::vector<size_t>& L, bool FromMiddle) {
            const V3 Mid = PlayArea.Found ? V3{ (PlayArea.Lo[0] + PlayArea.Hi[0]) / 2, (PlayArea.Lo[1] + PlayArea.Hi[1]) / 2, (PlayArea.Lo[2] + PlayArea.Hi[2]) / 2 } : Middle;
            std::stable_sort(L.begin(), L.end(), [&](size_t A, size_t B) {
                const double Da = FromMiddle ? Length(Sub(Pool[A].Centre, Mid)) : Far(Pool[A].Centre);
                const double Db = FromMiddle ? Length(Sub(Pool[B].Centre, Mid)) : Far(Pool[B].Centre);
                return Da > Db;
            });
        };
        const size_t Step = (std::max)((size_t)1, Converted.size() / 20);
        const auto Convert = [&](std::vector<size_t>& L) {
            for (size_t At = 0; At < L.size() && OverPlanes(Planes);)
            {
                for (const size_t End = (std::min)(L.size(), At + Step); At < End; At++) Converted[L[At]] = 1;
                Planes = Count();
            }
        };
        if (OverPlanes(Planes))
        {
            Farthest(Outside, false);
            Convert(Outside);
        }
        if (OverPlanes(Planes))
        {
            Farthest(Inside, true);
            Convert(Inside);
        }
        if (OverPlanes(Planes))
        {
            std::vector<std::pair<size_t, size_t>> Own;   // a model's own slots, the model
            for (const size_t N : ModelsNamed)
            {
                std::vector<const CompilerBrush*> Of;
                for (size_t I = Converted.size(); I < Movable; I++)
                    if (Pool[I].Model == N) Of.push_back(&Pool[I].Planes);
                Own.push_back({ CountPlanes(Of).Slots, N });
            }
            std::stable_sort(Own.begin(), Own.end(), [](const auto& A, const auto& B) { return A.first > B.first; });
            for (const auto& [Taken, N] : Own)
            {
                if (!OverPlanes(Planes)) break;
                Stripped.insert(N);
                Stats.BrushesOverBudget += Scene.Collision.Models[N].size();
                Planes = Count();
            }
        }
    }    Stats.BrushesAsPatches = (size_t)std::count(Converted.begin(), Converted.end(), 1);
    Stats.PlaneSlotsPlanned = Planes.Slots;
    if (Stats.BrushesAsPatches || !Stripped.empty())
        Stats.Warnings.push_back("brushes fitted to Black Ops III's cod2map: " + std::to_string(PlanesBefore.Slots) + " plane slots, " +
                                 std::to_string(PlanesBefore.SidePlanes) + " planes written, " + std::to_string(PlanesBefore.Brushes) + " brushes -> " +
                                 std::to_string(Planes.Slots) + ", " + std::to_string(Planes.SidePlanes) + ", " + std::to_string(Planes.Brushes) + ": " +
                                 std::to_string(Stats.BrushesAsPatches) + " world brushes made collision patches (farthest from play first), " +
                                 std::to_string(Stripped.size()) + " brush models left their drawn triangles (limits " + std::to_string(Lim.PlaneSlots) +
                                 " slots, 131072 planes, " + std::to_string(Lim.Brushes) + " brushes)");
    // the collision a converted brush keeps: its faces' triangles, as the collision triangles' patches
    std::vector<std::array<V3, 3>> BrushCollision;
    for (size_t I = 0; I < Converted.size(); I++)
    {
        if (!Converted[I]) continue;
        const Brush& B = *Pool[I].B;
        bool Collides = false;
        for (const BrushSide& S : B.Sides)
        {
            const std::string N = Tools.Of(B, S);
            Collides = Collides || N == "caulk" || N.rfind("clip", 0) == 0;
        }
        if (!Collides) continue;
        for (const BrushFace& F : BrushFaces(B))
            for (size_t K = 1; K + 1 < F.Points.size(); K++) BrushCollision.push_back({ F.Points[0], F.Points[K], F.Points[K + 1] });
    }
    // a level whose game gives no collision (MW4's clip map is not read): its world's drawn triangles
    // collide instead (as drawn, facing their front), fitted below like the rest, nearest play first.
    // A drawn triangle turns clockwise seen from its front, a collision one counter-clockwise (it
    // faces along (b - a) x (c - a): the patches and prisms below): each turned round. Taken as
    // drawn, every patch faced down and every prism stood 2 units out of its surface.
    size_t DrawnAsCollision = 0;
    if (Fitting && Scene.Collision.Triangles.empty() && Scene.Collision.Brushes.empty())
        for (const Mesh& M : Scene.Objects)
        {
            if (DrawnWorld::Owner(M.Name)) continue;
            for (const Surface& S : M.Surfaces)
            {
                if (NoSurface(Scene, S.Material)) continue;
                for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                {
                    const uint32_t* I = &S.Indices[K];
                    if (I[0] >= S.Vertices.size() || I[1] >= S.Vertices.size() || I[2] >= S.Vertices.size()) continue;
                    std::array<V3, 3> T;
                    for (int J = 0; J < 3; J++) T[J] = { S.Vertices[I[J]].Pos[0], S.Vertices[I[J]].Pos[1], S.Vertices[I[J]].Pos[2] };
                    if (!Within(T[0]) || !Within(T[1]) || !Within(T[2])) continue;
                    std::swap(T[1], T[2]);
                    BrushCollision.push_back(T);
                    DrawnAsCollision++;
                }
            }
        }
    if (DrawnAsCollision)
        Stats.Warnings.push_back("no collision read for this game: the world's " + std::to_string(DrawnAsCollision) + " drawn triangles collide instead");
    // MAX_MAP_NODES (0x8000) and MAX_MAP_LEAFS: Black Ops III's BSP is split by the brushes' sides but
    // clip ones (a field of clip brushes made 6 nodes where caulk ones made more than 32768). Its
    // nodes measured against those sides: up to 1.48 a side (BO4's mp_frenetic); past Room of the
    // limit at 1.6 a side, the brushes farthest from play keep clip sides only (Flat), untextured.
    std::set<const Brush*> Flat;
    {
        constexpr double NodesPerSide = 1.6;
        const auto Structural = [&](const Brush& B) {
            for (const BrushSide& S : B.Sides)
                if (Tools.Of(B, S).rfind("clip", 0) != 0) return true;
            return false;
        };
        std::vector<std::pair<double, size_t>> Kept;   // distance from play, pool index
        size_t Sides = 6 * 6;   // the sky box's
        for (size_t I = 0; I < Pool.size(); I++)
        {
            if (I < Converted.size() ? Converted[I] : Stripped.count(Pool[I].Model) != 0) continue;
            if (!Structural(*Pool[I].B)) continue;
            Sides += Pool[I].B->Sides.size();
            Kept.push_back({ Far(Pool[I].Centre), I });
        }
        const double Budget = Room * 0x8000 / NodesPerSide;
        Stats.StructuralSides = Sides;
        if (Fitting && (double)Sides > Budget)
        {
            std::stable_sort(Kept.begin(), Kept.end());
            size_t Running = 6 * 6;
            for (const auto& [D, I] : Kept)
            {
                Running += Pool[I].B->Sides.size();
                if ((double)Running > Budget) Flat.insert(Pool[I].B);
            }
            Stats.BrushesFlat = Flat.size();
            Stats.Warnings.push_back("brushes kept out of Black Ops III's BSP: " + std::to_string(Sides) + " sides would split it (MAX_MAP_NODES " +
                                     std::to_string(0x8000) + ", 1.6 a side at most): " +
                                     std::to_string(Flat.size()) + " brushes farthest from play made clip only, their faces drawn by the world's models");
        }
    }
    {
        constexpr const char* Texturing = "texturing the world's brushes";
        std::vector<const Brush*> Written;
        for (size_t I = 0; I < WorldBrushes->size(); I++)
            if (!Converted[I]) Written.push_back(&(*WorldBrushes)[I]);
        std::vector<PreparedBrush> Prepared;
        if (!Stage(Texturing, 0, Written.size()) ||
            !PrepareBrushes(Written, Tools, Tex, Prepared, [&](size_t Done) { return Stage(Texturing, Done, Written.size()); }, Flat))
            return false;
        for (const PreparedBrush& B : Prepared)
            if (CommitBrush(Out, B, BrushNumber, Stats, Tex)) BrushNumber++;
    }
    // an outer sky box around everything the level holds (cut to its bound above, the entities
    // too): Black Ops III's compiler seals the level with sky brushes alone (a source sealed by
    // its structural brushes too leaks there)
    {
        double Lo[3] = { 1e30, 1e30, 1e30 }, Hi[3] = { -1e30, -1e30, -1e30 };
        const auto Grow = [&](double X, double Y, double Z) {
            const double P[3] = { X, Y, Z };
            if (!Within(P)) return;   // left out below too
            for (int A = 0; A < 3; A++)
                if (std::isfinite(P[A])) Lo[A] = (std::min)(Lo[A], P[A]), Hi[A] = (std::max)(Hi[A], P[A]);
        };
        const auto& Cv = Scene.Collision.Vertices;
        for (size_t I = 0; I + 2 < Cv.size(); I += 3) Grow(Cv[I], Cv[I + 1], Cv[I + 2]);
        for (const Mesh& M : Scene.Objects)
            for (const Surface& S : M.Surfaces)
                if (!NoSurface(Scene, S.Material))
                    for (const Vertex& V : S.Vertices) Grow(V.Pos[0], V.Pos[1], V.Pos[2]);
        for (const auto* List : { &Scene.StaticInstances, &Scene.DynamicInstances, &Scene.SplinedInstances })
            for (const Instance& I : *List) Grow(I.Origin[0], I.Origin[1], I.Origin[2]);
        // the entities too: each stands within the level BO3 seals
        for (const Entity& E : Ents)
            if (float O[3]; &E != World && EntityFloats(E, "origin", O, 3)) Grow(O[0], O[1], O[2]);
        if (Lo[0] <= Hi[0])
        {
            constexpr double Margin = 1024, Thick = 64;
            for (int A = 0; A < 3; A++) Lo[A] = std::floor(Lo[A] - Margin), Hi[A] = std::ceil(Hi[A] + Margin);
            Stats.SkyBoxSize = (size_t)(std::max)({ Hi[0] - Lo[0], Hi[1] - Lo[1], Hi[2] - Lo[2] });
            // a slab per side of the box, each the box's full size across, Thick deep outside it
            for (int A = 0; A < 3; A++)
                for (int Far = 0; Far < 2; Far++)
                {
                    double SLo[3], SHi[3];
                    for (int K = 0; K < 3; K++) SLo[K] = Lo[K] - Thick, SHi[K] = Hi[K] + Thick;
                    if (Far) SLo[A] = Hi[A];
                    else SHi[A] = Lo[A];
                    Brush Slab;
                    Slab.Contents = 0x800;   // sky
                    for (int K = 0; K < 3; K++)
                        for (int Sign = -1; Sign <= 1; Sign += 2)
                        {
                            BrushSide S;
                            S.Normal[0] = S.Normal[1] = S.Normal[2] = 0;
                            S.Normal[K] = (float)Sign;
                            S.Dist = (float)(Sign > 0 ? SHi[K] : -SLo[K]);
                            S.Material = "sky";
                            Slab.Sides.push_back(S);
                        }
                    if (WriteBrush(Out, Slab, BrushNumber, nullptr, nullptr, Tools, "sky", Stats))
                    {
                        BrushNumber++;
                        Stats.Brushes--, Stats.SkyboxBrushes++;   // not the clip map's
                    }
                }
        }
    }
    Out += "}\n";

    // the collision triangles (patches, terrain): each a prism Thick units deep behind its face
    // (they face out), in a func_group (the world's at compile time). A triangle given twice (the
    // same corners) is written once: facing the same way, a copy; facing the other way (a sheet
    // colliding on both sides), the one facing up, else the first.
    constexpr double Thick = 2.0;   // how deep a prism is
    const auto& Tv = Scene.Collision.Vertices;
    bool Stopped = false;   // cancelled within Prisms
    // the patches' collision corners, once each (to a thousandth of a unit): MAX_MAP_COLLISIONVERTS's count
    std::set<std::array<int64_t, 3>> CollisionCorners;
    const auto Corner3 = [&CollisionCorners](const V3& P) {
        CollisionCorners.insert({ std::llround(P[0] * 1000.0), std::llround(P[1] * 1000.0), std::llround(P[2] * 1000.0) });
    };
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
        // the kept triangles as terrain patches (TerrainGrids), clip_full, colliding; those no
        // grid takes merged plane by plane (CoplanarPolygons), a prism per polygon. Only ground
        // goes to the grids: the compiler makes a terrain patch's collision a height over x, y, so
        // a wall written as one turns into a lid at its top (a ring of walls round shipment's yard
        // roofed it at 272): the steep triangles go straight to the prisms.
        constexpr double Steep = 0.5;   // |normal z| below this (60 degrees from up): a wall
        // the triangles (the converted brushes' faces after the collision's), within Room of
        // cod2map's collision triangles (its indices, three a triangle: 0xc0000) and vertices
        // (0x40000): past it, those farthest from play left out (no player reaches them)
        // Each triangle's tool: what Black Ops III's tools give its material's content flags, as a
        // brush's (ToolNames), when the game's reader names the triangles' materials (the clip
        // map's AABB tree). A surface the player stood on (solid) clip_full; one that only stopped
        // bullets (weaponClip, 0x2080: BO2's drawn terrain, railings, rubble and trims, the player
        // standing on the clip brushes under them) clip_weapon: written clip_full, it laid a bumpy
        // player clip over every floor, invisible walls on every detail, and no room to go prone.
        // Those colliding with nothing left out. clip_full when unknown (the drawn world as
        // collision, MW4; the games whose AABB tree is not read).
        const auto& Tm = Scene.Collision.TriangleMaterials;
        const bool HasMaterials = !Tm.empty() && Tm.size() * 3 == Tris.size();
        std::vector<std::string> ToolList = { "clip_full" };
        std::map<uint32_t, int> OfMaterial;   // material number -> tool (ToolList), -1 left out
        const auto ToolOf = [&](size_t T) {
            if (!HasMaterials || Tm[T] >= Scene.Collision.Materials.size()) return 0;
            const auto [It, New] = OfMaterial.try_emplace(Tm[T], 0);
            if (New)
            {
                // by its flags alone: BO2 names drawn materials and even tools ("caulk", "grass_clip")
                // for leaves its flags make weaponClip (0x2080), and the flags are what the game used
                Brush B;
                B.Contents = Scene.Collision.Materials[Tm[T]].second;
                std::string N = Tools.Of(B, BrushSide());
                // a triangle no bit of which stops anything (Ghosts' water sheets and grass cards)
                // collides with nothing
                if (!Collision::Stops(B.Contents)) N = "nodraw_notsolid";
                if (N == "nodraw_notsolid" || N == "trigger" || N == "hint" || N == "portal" || N == "portal_nodraw")
                    It->second = -1;
                else
                {
                    if (N == "caulk" || N == "sky") N = "clip_full";   // solid, as the drawn terrain's
                    const auto Found = std::find(ToolList.begin(), ToolList.end(), N);
                    It->second = (int)(Found - ToolList.begin());
                    if (Found == ToolList.end()) ToolList.push_back(N);
                }
            }
            return It->second;
        };
        std::vector<std::array<V3, 3>> Tris3;
        std::vector<int> Tool3;   // Tris3's tools
        for (size_t T = 0; T < Kept.size(); T++)
        {
            if (!Kept[T]) continue;
            const int Tool = ToolOf(T);
            if (Tool < 0)
            {
                Stats.CollisionNonColliding++;
                continue;
            }
            Tris3.push_back({ Corner(T, 0), Corner(T, 1), Corner(T, 2) });
            Tool3.push_back(Tool);
        }
        Tris3.insert(Tris3.end(), BrushCollision.begin(), BrushCollision.end());
        Tool3.resize(Tris3.size(), 0);
        {
            const double MostTris = Room * (std::min)((double)Lim.CollisionTriangles, 0xc0000 / 3.0), MostCorners = Room * Lim.CollisionVertices;
            std::set<std::array<int64_t, 3>> Corners;
            const auto Key = [](const V3& P) { return std::array<int64_t, 3>{ std::llround(P[0] * 1000.0), std::llround(P[1] * 1000.0), std::llround(P[2] * 1000.0) }; };
            for (const auto& T : Tris3)
                for (const V3& P : T) Corners.insert(Key(P));
            if (Fitting && (double)Tris3.size() > MostTris || (double)Corners.size() > MostCorners)
            {
                std::vector<std::pair<double, size_t>> Order;
                for (size_t I = 0; I < Tris3.size(); I++)
                {
                    const auto& T = Tris3[I];
                    Order.push_back({ (std::min)({ Far(T[0]), Far(T[1]), Far(T[2]) }), I });
                }
                std::stable_sort(Order.begin(), Order.end());
                std::vector<std::array<V3, 3>> Fit;
                std::vector<int> FitTool;
                Corners.clear();
                for (const auto& [D, I] : Order)
                {
                    size_t New = 0;
                    for (const V3& P : Tris3[I]) New += !Corners.count(Key(P));
                    if ((double)Fit.size() + 1 > MostTris || (double)(Corners.size() + New) > MostCorners) break;
                    for (const V3& P : Tris3[I]) Corners.insert(Key(P));
                    Fit.push_back(Tris3[I]);
                    FitTool.push_back(Tool3[I]);
                }
                Tool3 = std::move(FitTool);
                Stats.CollisionFitted = Tris3.size() - Fit.size();
                Stats.CollisionOutsidePlay = Stats.CollisionFitted;
                Stats.Warnings.push_back("collision fitted to Black Ops III's cod2map: " + std::to_string(Tris3.size()) + " triangles -> " +
                                         std::to_string(Fit.size()) + ", those farthest from play left out (" + std::to_string((size_t)MostTris) +
                                         " triangles, " + std::to_string((size_t)MostCorners) + " vertices at most)");
                Tris3 = std::move(Fit);
            }
        }
        // the triangles of one plane and one tool: cod2map merges them into concave windings as a
        // group of at most 32768 (MAX_WINDINGS_PER_CONCAVE_GROUP, "Try -blocksize 0", its fragments
        // counted), the compile ending past it. A level's flat ground in one mesh crosses it (MW4
        // shipment's Havok ground at z 0: 26000 patches over 6700 by 6700 units): past half of it,
        // those farthest from play left out.
        if (Fitting)
        {
            constexpr size_t MostCoplanar = 16384;
            using PlaneKey = std::tuple<int, int64_t, int64_t, int64_t, int64_t>;   // tool, normal, distance
            std::map<PlaneKey, std::vector<size_t>> OnPlane;
            for (size_t I = 0; I < Tris3.size(); I++)
            {
                const auto& T = Tris3[I];
                V3 N = Cross(Sub(T[1], T[0]), Sub(T[2], T[0]));
                const double L = Length(N);
                if (!(L > 0)) continue;
                N = Mul(N, 1.0 / L);
                // a plane either way round: the same plane to cod2map
                if (N[2] < 0 || (N[2] == 0 && (N[1] < 0 || (N[1] == 0 && N[0] < 0)))) N = Mul(N, -1.0);
                OnPlane[{ Tool3[I], std::llround(N[0] * 100.0), std::llround(N[1] * 100.0), std::llround(N[2] * 100.0), std::llround(Dot(N, T[0])) }]
                    .push_back(I);
            }
            std::vector<char> Drop(Tris3.size(), 0);
            size_t Dropped = 0;
            for (auto& [Key, Of] : OnPlane)
            {
                if (Of.size() <= MostCoplanar) continue;
                std::vector<std::pair<double, size_t>> Order;
                for (const size_t I : Of) Order.push_back({ (std::min)({ Far(Tris3[I][0]), Far(Tris3[I][1]), Far(Tris3[I][2]) }), I });
                std::stable_sort(Order.begin(), Order.end());
                for (size_t K = MostCoplanar; K < Order.size(); K++) Drop[Order[K].second] = 1, Dropped++;
            }
            if (Dropped)
            {
                std::vector<std::array<V3, 3>> Kept;
                std::vector<int> KeptTool;
                for (size_t I = 0; I < Tris3.size(); I++)
                    if (!Drop[I]) Kept.push_back(Tris3[I]), KeptTool.push_back(Tool3[I]);
                Stats.CollisionCoplanarLeftOut = Dropped;
                Stats.Warnings.push_back("collision fitted to Black Ops III's cod2map: " + std::to_string(Dropped) + " triangles left out of planes holding over " +
                                         std::to_string(MostCoplanar) + " of one tool (those farthest from play; its concave windings stop at 32768 a plane)");
                Tris3 = std::move(Kept);
                Tool3 = std::move(KeptTool);
            }
        }
        // a tool's triangles apart from another's: a grid or a polygon of one tool
        const auto Blocks = [](const std::string& Tool) {
            return Tool == "clip_full" || Tool == "clip" || Tool == "clip_player" || Tool == "clip_player_vehicle" || Tool == "clip_nosight";
        };
        std::vector<std::vector<std::array<V3, 3>>> AllOf(ToolList.size()), RestOf(ToolList.size());
        for (size_t I = 0; I < Tris3.size(); I++)
        {
            const auto& Tri = Tris3[I];
            const V3 N = Cross(Sub(Tri[1], Tri[0]), Sub(Tri[2], Tri[0]));
            const double L = Length(N);
            (L > 0 && std::abs(N[2]) / L < Steep ? RestOf : AllOf)[(size_t)Tool3[I]].push_back(Tri);
            if (!Blocks(ToolList[(size_t)Tool3[I]])) Stats.CollisionNotPlayer++;
        }
        if (!Stage("collision: terrain patches"))
        {
            Stopped = true;
            return;
        }
        std::vector<std::pair<std::vector<TerrainGrid>, size_t>> Grids;   // a tool's grids, the tool
        for (size_t K = 0; K < ToolList.size(); K++)
            if (!AllOf[K].empty()) Grids.emplace_back(TerrainGrids(AllOf[K], RestOf[K], Target->MinPatchTriangleArea()), K);
        for (const auto& [Of, K] : Grids)
        for (const TerrainGrid& G : Of)
        {
            Out += "// brush " + std::to_string(Number++) + "\n {\n  mesh\n  {\n   " + ToolList[K] + "\n   lightmap_gray\n   " + std::to_string(G.Ctrl.size()) + " " +
                   std::to_string(G.Ctrl[0].size()) + " 16 8\n";
            for (size_t X = 0; X < G.Ctrl.size(); X++)
            {
                Out += "   (\n";
                for (size_t Y = 0; Y < G.Ctrl[X].size(); Y++)
                {
                    const V3& P = G.Ctrl[X][Y];
                    Out += "\tv " + Num(P[0]) + " " + Num(P[1]) + " " + Num(P[2]) + " t 0 0 0 0" + (G.Turned[X][Y] ? " f 1\n" : "\n");
                }
                Out += "   )\n";
            }
            Out += "  }\n }\n";
            Stats.TerrainPatches++;
            Stats.Triangles += 2 * (G.Ctrl.size() - 1) * (G.Ctrl[0].size() - 1);
            Stats.PatchCollisionTriangles += 2 * (G.Ctrl.size() - 1) * (G.Ctrl[0].size() - 1);
            for (const auto& Row : G.Ctrl)
                for (const V3& P : Row) Corner3(P);
        }
        if (!Stage("collision: triangles merged plane by plane"))
        {
            Stopped = true;
            return;
        }
        const IRadiantTarget::Merge M = Target->CollisionMerge();
        std::vector<Polygon> Polygons;
        std::vector<size_t> PolygonTool;
        for (size_t K = 0; K < ToolList.size(); K++)
        {
            if (RestOf[K].empty()) continue;
            std::vector<Polygon> Of = CoplanarPolygons(RestOf[K], Stats.TrianglesDropped, M.Distance, M.MinDot);
            PolygonTool.insert(PolygonTool.end(), Of.size(), K);
            Polygons.insert(Polygons.end(), std::make_move_iterator(Of.begin()), std::make_move_iterator(Of.end()));
        }
        // Past the target's budget of plane slots (the prisms' with the clip map's brushes', as
        // cod2map makes them: MapRadiantPlanes.h), the polygons as patches instead, a triangle
        // each (a fan of the polygon), which take no plane: cod2map ends at MAX_MAP_PLANES (131072)
        // for Ghosts' and AW's collision, at MAX_MAP_PLANES_COMPILE for BO2's zm_transit. A patch
        // of one triangle cannot roof anything as a grid of walls did.
        const double Budget = Target->CollisionPlaneBudget();
        bool AsPatches = !Polygons.empty() && Budget <= 0;
        if (!Polygons.empty() && Budget > 0 && std::isfinite(Budget))
        {
            std::vector<CompilerBrush> Of(Scene.Collision.Brushes.size() + Polygons.size());
            struct None {};
            ParallelFor<None>(Of.size(), 64, [&](size_t I, None&) {
                if (I < Scene.Collision.Brushes.size())
                    Of[I] = CompilerPlanes(Scene.Collision.Brushes[I]);
                else
                {
                    const Polygon& G = Polygons[I - Scene.Collision.Brushes.size()];
                    Brush B;
                    PolygonPrism(G.Loop, G.N, G.D, Thick, B);
                    Of[I] = CompilerPlanes(B);
                }
            });
            std::vector<const CompilerBrush*> All;
            for (const CompilerBrush& B : Of) All.push_back(&B);
            Stats.PrismPlaneSlots = CountPlanes(All).Slots;
            AsPatches = (double)Stats.PrismPlaneSlots > Budget;
            if (AsPatches)
                Stats.Warnings.push_back("collision as patches, not prisms: " + std::to_string(Polygons.size()) + " prisms would take " +
                                         std::to_string(Stats.PrismPlaneSlots) + " plane slots with the brushes (Black Ops III's cod2map: " +
                                         std::to_string(0xa0000) + ")");
        }
        if (AsPatches)
        {
            std::vector<std::pair<std::array<const V3*, 3>, size_t>> Fan;   // a triangle, its tool
            for (size_t Gi = 0; Gi < Polygons.size(); Gi++)
            {
                const Polygon& G = Polygons[Gi];
                for (size_t J = 1; J + 1 < G.Loop.size(); J++)
                {
                    const std::array<const V3*, 3> C = { &G.Loop[0], &G.Loop[J], &G.Loop[J + 1] };
                    if (Sliver(*C[0], *C[1], *C[2], Target->MinPatchTriangleArea()))
                    {
                        Stats.TrianglesDropped++;
                        continue;
                    }
                    Fan.emplace_back(C, PolygonTool[Gi]);
                }
                Stats.Triangles += G.Triangles;
            }
            const auto V = [&](const V3& P) { return "\tv " + Num(P[0]) + " " + Num(P[1]) + " " + Num(P[2]) + " t 0 0 0 0\n"; };
            for (const auto& [C, K] : Fan)
            {
                // faces as the game's: the grid's triangle faces along (row 1 - row 0) x
                // (column 1 - column 0) = (C[1] - C[0]) x (C[2] - C[0]), the polygon's facing (rows
                // C[0], C[1] then C[2], C[1] wrote it facing back), the fourth corner repeating the second
                Out += "// brush " + std::to_string(Number++) + "\n {\n  mesh\n  {\n   " + ToolList[K] + "\n   lightmap_gray\n   2 2 16 8\n   (\n" +
                       V(*C[0]) + V(*C[2]) + "   )\n   (\n" + V(*C[1]) + V(*C[2]) + "   )\n  }\n }\n";
                Stats.CollisionPatches++;
                Stats.PatchCollisionTriangles++;
                for (const V3* P : C) Corner3(*P);
            }
            return;
        }
        for (size_t Gi = 0; Gi < Polygons.size(); Gi++)
        {
            if (!Stage("texturing the collision prisms", Gi, Polygons.size()))
            {
                Stopped = true;
                return;
            }
            const Polygon& G = Polygons[Gi];
            Brush B;
            PolygonPrism(G.Loop, G.N, G.D, Thick, B);
            // counted as triangles, not as the clip map's brushes
            const size_t Dropped = Stats.BrushesDropped, Unsound = Stats.BrushesUnsound;
            if (WriteBrush(Out, B, Number, nullptr, nullptr, Tools, ToolList[PolygonTool[Gi]], Stats,
                           { Tex.Drawn, ForGdt, 0, &Stats.PrismTextures, &Hidden, Fitting }))
            {
                Number++;
                Stats.Brushes--, Stats.Triangles += G.Triangles, Stats.TriangleBrushes++;
            }
            else
            {
                Stats.BrushesDropped = Dropped, Stats.BrushesUnsound = Unsound;
                Stats.TrianglesDropped += G.Triangles;
            }
        }
    };
    if (!Scene.Collision.Triangles.empty() || !BrushCollision.empty())
    {
        Begin(nullptr, "func_group");
        BrushNumber = 0;
        Prisms(Scene.Collision.Triangles, BrushNumber);
        if (Stopped) return false;
        Stats.PatchCollisionVertices = CollisionCorners.size();
        Out += "}\n";
    }

    // the other entities; a brush entity's brushes moved to where it stands. Named: the models
    // they and the placements name, written as files below
    std::set<std::string> Named;
    // cod2map's 1024 brush models that do not move (MAX non dyn entity brush models; which move is
    // not known here, so every one counted): past Room of it, the later brush entities let go,
    // kept as points, their drawn triangles the world's
    // the brush models whose drawn triangles are in their own space (IW's mp_flip: a rotating
    // object's at 0, its entity at 12242 -26537 -6906), not where their entity stands: their
    // patches and models moved there (a patch left at the origin, with the entity's brushes, made
    // cod2map assert "isCycle" in brush_edges.cpp). Judged by where the triangles' middle is: by the
    // model's brushes in their own space, or by the entity's origin.
    struct LocalPlace
    {
        float Place[12];
        std::string Origin, Angles;
    };
    std::map<uint32_t, LocalPlace> Local;
    {
        std::map<uint32_t, std::pair<V3, size_t>> Middle3;   // owner -> sum of its triangles' corners, their count
        for (const Mesh& M : Scene.Objects)
            if (const uint32_t O = DrawnWorld::Owner(M.Name))
                for (const Surface& S : M.Surfaces)
                    for (const Vertex& V : S.Vertices)
                    {
                        auto& [Sum, N] = Middle3[O];
                        Sum = Add(Sum, V3{ V.Pos[0], V.Pos[1], V.Pos[2] });
                        N++;
                    }
        for (const Entity& E : Ents)
        {
            const auto Model = E.find("model");
            if (&E == World || Model == E.end() || Model->second.size() < 2 || Model->second[0] != '*') continue;
            const uint32_t N = (uint32_t)std::strtoul(Model->second.c_str() + 1, nullptr, 10);
            const auto It = Middle3.find(N);
            if (It == Middle3.end() || !It->second.second || Local.count(N)) continue;
            const V3 Cm = Mul(It->second.first, 1.0 / (double)It->second.second);
            LocalPlace L;
            float Ang[3] = { 0, 0, 0 }, Axis[9];
            for (int K = 0; K < 3; K++) L.Place[K] = 0;
            EntityFloats(E, "origin", L.Place, 3);
            EntityFloats(E, "angles", Ang, 3);
            AnglesToAxis(Ang, Axis);
            for (int K = 0; K < 9; K++) L.Place[3 + K] = Axis[K];
            const V3 O = { L.Place[0], L.Place[1], L.Place[2] };
            if (Length(O) < 1.0) continue;   // at the origin: either space is the same
            V3 Cb = { 0, 0, 0 };
            size_t Nb = 0;
            if (N < Scene.Collision.Models.size())
                for (const Brush& B : Scene.Collision.Models[N])
                    for (const BrushFace& F : BrushFaces(B))
                        for (const V3& P : F.Points) Cb = Add(Cb, P), Nb++;
            bool Own;
            if (Nb)
            {
                Cb = Mul(Cb, 1.0 / (double)Nb);
                V3 Placed;
                for (int A = 0; A < 3; A++) Placed[A] = Cb[0] * Axis[A] + Cb[1] * Axis[3 + A] + Cb[2] * Axis[6 + A] + O[A];
                Own = Length(Sub(Cm, Cb)) < Length(Sub(Cm, Placed));
            }
            else
                Own = Length(Cm) < Length(Sub(Cm, O));
            if (!Own) continue;
            const auto Text = [&E](const char* Key) { const auto V = E.find(Key); return V == E.end() ? std::string("0 0 0") : V->second; };
            L.Origin = Text("origin"), L.Angles = Text("angles");
            Local.emplace(N, L);
        }
        Stats.BrushModelsPlaced = Local.size();
    }
    std::set<const Entity*> LetGo;
    {
        size_t Held = 0;
        for (const Entity& E : Ents)
        {
            const auto Model = E.find("model");
            if (&E == World || Model == E.end() || Model->second.empty() || (Model->second[0] != '*' && Model->second[0] != '?')) continue;
            const size_t N = (size_t)std::strtoul(Model->second.c_str() + 1, nullptr, 10);
            const bool Brushy = Model->second[0] == '?' ? N < Scene.Collision.Triggers.size() && !Scene.Collision.Triggers[N].empty()
                                                         : (N < Scene.Collision.Models.size() && !Scene.Collision.Models[N].empty() && !Stripped.count(N)) ||
                                                               (N > 0 && Owners.count((uint32_t)N));
            if (!Brushy) continue;
            if ((double)++Held > Room * Lim.BrushModels && Fitting) LetGo.insert(&E);
        }
        Stats.BrushEntitiesDropped = LetGo.size();
        if (!LetGo.empty())
            Stats.Warnings.push_back(std::to_string(Held) + " brush entities, " + std::to_string(LetGo.size()) + " let go (kept as points) under cod2map's " +
                                     std::to_string(Lim.BrushModels) + " brush models that do not move");
    }
    for (const Entity& E : Ents)
    {
        if (!Stage("the entities", (size_t)(&E - Ents.data()), Ents.size())) return false;
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
            if (((N >= From.size() || From[N].empty() || (Kind == '*' && Stripped.count(N))) && !HasDrawn) || LetGo.count(&E))
            {
                // no brush to give it: kept without its "*N", which would name a brush model the
                // compiler never makes (it skips an entity without brushes) and BO3 reads past its
                // brush models' end as the level loads (a crash)
                Stats.BrushEntitiesEmpty++;
                Entity Keys = E;
                Keys.erase("model");
                Begin(&Keys, "info_null");
                Out += "}\n";
                Stats.Entities++;
                continue;
            }
        }
        Entity Keys = E;
        if (BrushEntity) Keys.erase("model");
        else if (Model != E.end() && Target->Stock(Model->second, "xmodel.gdf"))
            Stats.StockModels++;   // the tools' own model, as it is: no file
        else if (Model != E.end() && Scene.Models.count(Model->second))
        {
            // a model of the map's (a script_model): written with the placed ones, by its GDT name
            Keys["model"] = ModelName(Model->second);
            Named.insert(Model->second);
        }
        const size_t Start = Out.size();
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
            const Texturing Own{ Kind == '*' ? Tex.Drawn : nullptr, ForGdt, (uint32_t)N, &Stats.EntityTextures, &Hidden, Fitting };
            if (N < From.size() && !(Kind == '*' && Stripped.count(N)))
                for (const Brush& B : From[N])
                    if (FewEnough(B) && WriteBrush(Out, B, Number, Origin, Axis, Tools, Trigger ? "trigger" : "", Stats, Own, Flat.count(&B) != 0))
                    {
                        Number++;
                        if (Kind == '?') Stats.Brushes--, Stats.TriggerBrushes++;   // not the clip map's
                    }
            if (Kind == '*' && N > 0 && Patched.insert((uint32_t)N).second)
            {
                const auto L = Local.find((uint32_t)N);
                WritePatches(Out, Scene, Drawn, (uint32_t)N, ForGdt, Stats, Number, Ov, Cover, L == Local.end() ? nullptr : L->second.Place);
            }
            if (Number == 0)
            {
                // every brush left out (portals, unsound): an empty entity, as above
                Out.resize(Start);
                EntityNumber--;
                Begin(&Keys, "info_null");
                Out += "}\n";
                Stats.BrushEntitiesEmpty++;
                Stats.Entities++;
                continue;
            }
            Stats.BrushEntities++;
        }
        Out += "}\n";
        Stats.Entities++;
    }

    // with Patches, the world's drawn triangles no brush side draws, and those of brush models no
    // entity holds, as patches; else they go to the world's models below, but for their floors
    // where the target takes those as patches (IRadiantTarget::FloorsAsPatches)
    const bool FloorPatches = !Patches && Target->FloorsAsPatches();
    // the floors as patches MostPatches at most, nearest play first (MW4's mp_rex_rm_shipment has
    // 2.15 million floor triangles: a .map Radiant cannot hold); the others stay in the world's models
    std::unordered_set<uint64_t> FloorsInModels;
    if (FloorPatches)
    {
        std::vector<std::pair<double, uint64_t>> All;
        for (size_t Oi = 0; Oi < Scene.Objects.size(); Oi++)
            for (size_t Si = 0; Si < Scene.Objects[Oi].Surfaces.size(); Si++)
            {
                const Surface& S = Scene.Objects[Oi].Surfaces[Si];
                if (NoSurface(Scene, S.Material)) continue;
                for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                {
                    if (Drawn.Removed(Oi, Si, K / 3) || Cover.Covered(Oi, Si, K / 3) || !Floor(S, K / 3) || FateOf(S, K / 3) != Fate::Patch) continue;
                    V3 C = { 0, 0, 0 };
                    for (int J = 0; J < 3; J++)
                        for (int A = 0; A < 3; A++) C[A] += S.Vertices[S.Indices[K + J]].Pos[A] / 3.0;
                    All.push_back({ Far(C), TriangleKey(Oi, Si, K / 3) });
                }
            }
        // and, fitted, half the draw surfaces at most (cod2map's MAX_MAP_DRAW_SURFS), the other half
        // left to the brushes and the floors' occluder (a prism or a box under them, DrawSurfaces)
        const size_t Most = Fitting ? (std::min)(MostPatches, (size_t)(Room * Lim.DrawSurfaces / 2)) : MostPatches;
        if (All.size() > Most)
        {
            std::nth_element(All.begin(), All.begin() + Most, All.end());
            for (size_t I = Most; I < All.size(); I++) FloorsInModels.insert(All[I].second);
            Stats.FloorsInModels = FloorsInModels.size();
            Stats.Warnings.push_back("floors as patches fitted: " + std::to_string(All.size()) + " floor triangles, the " + std::to_string(Most) +
                                     " nearest play as patches, " + std::to_string(FloorsInModels.size()) + " left in the world's models");
        }
    }
    if (Patches || FloorPatches)
    {
        if (!Stage("the world's patches")) return false;
        std::string Group;
        size_t Number = 0;
        std::vector<std::array<V3, 3>> Floors;
        const std::string Under = FloorPatches ? Target->FloorOccluder() : std::string();
        std::vector<std::array<V3, 3>>* Into = Under.empty() ? nullptr : &Floors;
        WritePatches(Group, Scene, Drawn, 0, ForGdt, Stats, Number, Ov, Cover, nullptr, FloorPatches, Into, &FloorsInModels);
        for (const uint32_t O : Owners)
            if (!Patched.count(O))
            {
                const auto L = Local.find(O);
                WritePatches(Group, Scene, Drawn, O, ForGdt, Stats, Number, Ov, Cover, L == Local.end() ? nullptr : L->second.Place, FloorPatches, Into, &FloorsInModels);
            }
        // under the floors (patches: cod2map makes a patch an Umbra target, never an occluder, a
        // caulk patch included) the target's occluder as brushes: without it Umbra had no ground
        // where the floors were patches, and a camera near it fell in view cells that culled
        // everything (BO1's mp_nuked). The floor's own planes, a unit lower, merged plane by plane,
        // each polygon a caulk prism two units thick (exact: what fixed mp_nuked in game), when the
        // map has room for them; else a heightfield of boxes (FloorBoxes), a unit under the lowest
        // floor of each cell, as fine as the brushes left to the map allow
        bool Prisms = false;
        // the share of cod2map's caulk corners ("layered geometry") our caulk may take: on MW4's
        // mp_rex_rm_shipment it counted some 170000 to 200000 more than the caulk we write
        constexpr double CaulkShare = 0.7;
        if (!Floors.empty())
        {
            std::vector<std::array<V3, 3>> Lowered = Floors;
            for (auto& T : Lowered)
                for (V3& P : T) P[2] -= 1.0;
            const IRadiantTarget::Merge M = Target->CollisionMerge();
            size_t Dropped = 0;
            const std::vector<Polygon> Polygons = CoplanarPolygons(Lowered, Dropped, M.Distance, M.MinDot);
            // a prism of n corners: n + 2 sides (draw surfaces), 6n caulk corners
            double Sides = 0, Corners = 0;
            for (const Polygon& G : Polygons) Sides += (double)G.Loop.size() + 2, Corners += 6.0 * (double)G.Loop.size();
            Prisms = !Fitting || ((double)Polygons.size() <= Room * Lim.Brushes - (double)Stats.Brushes - 64.0 &&
                                  Sides <= Room * Lim.BrushSides - (double)Stats.Faces && Sides <= Room * Lim.DrawSurfaces - (double)DrawSurfaces(Stats) &&
                                  Corners <= CaulkShare * Room * Lim.CaulkVertices - (double)Stats.CaulkVertices);
            const auto Write = [&](const Polygon& G) {
                Brush B;
                PolygonPrism(G.Loop, G.N, G.D, 2.0, B);
                if (WriteBrush(Group, B, Number, nullptr, nullptr, Tools, Under, Stats))
                {
                    Number++;
                    Stats.Brushes--, Stats.FloorOccluderBrushes++;   // not the clip map's
                }
            };
            if (Prisms)
                for (const Polygon& G : Polygons) Write(G);
            else if (Fitting)
            {
                // too many: the largest polygons in play as prisms within a third of what is left
                // (a raised floor's edges, which the heightfield's cells leave open), the boxes after
                std::vector<std::pair<double, size_t>> InPlay;
                for (size_t I = 0; I < Polygons.size(); I++)
                {
                    const Polygon& G = Polygons[I];
                    V3 C = { 0, 0, 0 };
                    double Area = 0;
                    for (size_t K = 0; K < G.Loop.size(); K++)
                    {
                        C = Add(C, Mul(G.Loop[K], 1.0 / (double)G.Loop.size()));
                        if (K >= 2) Area += Length(Cross(Sub(G.Loop[K - 1], G.Loop[0]), Sub(G.Loop[K], G.Loop[0]))) / 2;
                    }
                    if (Far(C) <= 0 && Area >= 1024.0) InPlay.push_back({ -Area, I });
                }
                std::sort(InPlay.begin(), InPlay.end());
                const double Third = 1.0 / 3.0;
                const double MostBrushes = Third * (Room * Lim.Brushes - (double)Stats.Brushes - 64.0),
                             MostSides = Third * (std::min)(Room * Lim.BrushSides - (double)Stats.Faces, Room * Lim.DrawSurfaces - (double)DrawSurfaces(Stats)),
                             MostCorners = Third * (CaulkShare * Room * Lim.CaulkVertices - (double)Stats.CaulkVertices);
                double Brushes = 0, Taken = 0, Caulk = 0;
                for (const auto& [Area, I] : InPlay)
                {
                    const double N = (double)Polygons[I].Loop.size();
                    if (Brushes + 1 > MostBrushes || Taken + N + 2 > MostSides || Caulk + 6 * N > MostCorners) break;
                    Brushes += 1, Taken += N + 2, Caulk += 6 * N;
                    Write(Polygons[I]);
                }
            }
        }
        if (!Floors.empty() && !Prisms)
        {
            // a box: one brush, six sides, six draw surfaces (cod2map makes one of every brush side
            // and patch: MAX_MAP_DRAW_SURFS), 24 caulk corners (all caulk: with clip_full sides, its
            // caulk top never reached Umbra's scene)
            const double MostBoxes =
                Fitting ? (std::min)({ Room * Lim.Brushes - (double)Stats.Brushes - 64.0, (Room * Lim.BrushSides - (double)Stats.Faces) / 6.0,
                                       (Room * Lim.DrawSurfaces - (double)DrawSurfaces(Stats)) / 6.0,
                                       (CaulkShare * Room * Lim.CaulkVertices - (double)Stats.CaulkVertices) / 24.0 })
                        : std::numeric_limits<double>::infinity();
            const V3 Mid = PlayArea.Found ? V3{ (PlayArea.Lo[0] + PlayArea.Hi[0]) / 2, (PlayArea.Lo[1] + PlayArea.Hi[1]) / 2, (PlayArea.Lo[2] + PlayArea.Hi[2]) / 2 } : Middle;
            const FloorBoxing Boxes = FloorBoxes(Floors, MostBoxes, Far, Mid);
            Stats.FloorOccluderCell = (size_t)Boxes.Cell;
            Stats.FloorOccluderLeftOut = Boxes.LeftOut;
            for (const auto& Box : Boxes.Boxes)
            {
                Brush B;
                for (int A = 0; A < 3; A++)
                    for (int Sign = -1; Sign <= 1; Sign += 2)
                    {
                        BrushSide S;
                        S.Normal[0] = S.Normal[1] = S.Normal[2] = 0;
                        S.Normal[A] = (float)Sign;
                        S.Dist = (float)(Sign > 0 ? Box[A + 3] : -Box[A]);
                        S.Material = Under;
                        B.Sides.push_back(S);
                    }
                if (WriteBrush(Group, B, Number, nullptr, nullptr, Tools, "", Stats))
                {
                    Number++;
                    Stats.Brushes--, Stats.FloorOccluderBrushes++;   // not the clip map's
                }
            }
            if (Boxes.Cell > FloorBoxing::Finest || Boxes.LeftOut)
                Stats.Warnings.push_back("floor occluder fitted to Black Ops III's cod2map: cells of " + std::to_string((int)Boxes.Cell) + " units, " +
                                         std::to_string(Boxes.Boxes.size()) + " boxes" +
                                         (Boxes.LeftOut ? ", " + std::to_string(Boxes.LeftOut) + " farthest from play left out" : std::string()) +
                                         " (" + std::to_string((size_t)(std::max)(0.0, MostBoxes)) + " brushes left)");
        }
        if (!Group.empty())
        {
            Begin(nullptr, "func_group");
            Out += Group + "}\n";
        }
    }

    // the placements, then the world geometry left, as misc_models; the models they name as files,
    // a model larger than the target takes as its parts (ModelParts), a placement of each
    std::deque<Surface> Cuts;
    std::map<std::string, std::vector<std::vector<const Surface*>>> PartsOf;
    const auto Parts = [&](const std::string& Model) -> const std::vector<std::vector<const Surface*>>& {
        auto [It, New] = PartsOf.try_emplace(Model);
        if (const auto M = Scene.Models.find(Model); New && M != Scene.Models.end())
        {
            // a level to build places only what draws: a surface of no area left out (a model of
            // only such then has no part, and no placement)
            std::vector<const Surface*> All;
            for (const Surface& S : M->second.Surfaces)
                if (Target->PlacesUnwrittenModels() || HasArea(S)) All.push_back(&S);
            It->second = ModelParts(All, Target->MaxModelSurfaces(), Target->MaxMaterialVertices(), Cuts);        }
        return It->second;
    };
    const auto Place = [&](const Instance& I) {
        // the tools' own model, whole and as it is: no file of it (nor of its parts) written
        if (Target->Stock(I.Model, "xmodel.gdf"))
        {
            Out += "// entity " + std::to_string(EntityNumber++) + "\n{\n";
            Out += Quote("classname", "misc_model") + Quote("model", I.Model) + Quote("origin", Vec(I.Origin)) + Quote("angles", Angles(I.Axis));
            if (std::fabs(I.Scale - 1.0f) > 1e-4f) Out += Quote("modelscale", Num(I.Scale));
            Out += "}\n";
            Stats.Placements++;
            Stats.StockModels++;
            return;
        }
        if (Scene.Models.count(I.Model) && Parts(I.Model).empty() && !Target->PlacesUnwrittenModels())
        {
            Stats.ModelsLeftOut++;
            return;
        }
        for (size_t P = 0; P < (std::max)(size_t(1), Parts(I.Model).size()); P++)
        {
            Out += "// entity " + std::to_string(EntityNumber++) + "\n{\n";
            Out += Quote("classname", "misc_model") + Quote("model", ModelName(I.Model, P)) + Quote("origin", Vec(I.Origin)) +
                   Quote("angles", Angles(I.Axis));
            if (std::fabs(I.Scale - 1.0f) > 1e-4f) Out += Quote("modelscale", Num(I.Scale));
            Out += "}\n";
        }
        Stats.Placements++;
        Named.insert(I.Model);
    };
    if (!Stage("the placements")) return false;
    // map_props: foliage left out, then the small props (a model's largest side, scaled). Past
    // Room of cod2map's static models (each misc_model, a model's part each) or entities, with
    // room for the world's tiles, the next setting, then the smallest props, until they fit.
    std::map<std::string, std::pair<bool, double>> PropOf;   // model -> (foliage, its largest side)
    const auto PropInfo = [&](const std::string& Model) -> const std::pair<bool, double>& {
        auto [It, New] = PropOf.try_emplace(Model);
        if (New)
        {
            It->second.first = FoliageName(Model);
            if (const auto M = Scene.Models.find(Model); M != Scene.Models.end())
            {
                float Lo[3] = { 1e30f, 1e30f, 1e30f }, Hi[3] = { -1e30f, -1e30f, -1e30f };
                for (const Surface& S : M->second.Surfaces)
                    for (const Vertex& V : S.Vertices)
                        for (int A = 0; A < 3; A++) Lo[A] = (std::min)(Lo[A], V.Pos[A]), Hi[A] = (std::max)(Hi[A], V.Pos[A]);
                for (int A = 0; A < 3; A++) It->second.second = (std::max)(It->second.second, (double)(Hi[A] - Lo[A]));
            }
            else
                It->second.second = 1e30;   // not ours to measure: kept
        }
        return It->second;
    };
    struct Candidate
    {
        const Instance* I;
        size_t Entries;   // the misc_models it writes
        bool Foliage;
        double Size;      // its largest side, scaled
    };
    std::vector<Candidate> Candidates;
    for (const auto* List : { &Scene.StaticInstances, &Scene.DynamicInstances, &Scene.SplinedInstances })
        for (const Instance& I : *List)
        {
            if (I.Model.empty()) continue;
            if (!Within(I.Origin)) Stats.BeyondWorld++;
            else if (!Scene.Models.count(I.Model) && !Target->PlacesUnwrittenModels() && !Target->Stock(I.Model, "xmodel.gdf")) Stats.ModelsLeftOut++;
            else if (I.Scale < Target->MinModelScale()) Stats.ScaledToNothing++;
            else
            {
                const bool Stock = Target->Stock(I.Model, "xmodel.gdf");
                if (!Stock && Scene.Models.count(I.Model) && Parts(I.Model).empty() && !Target->PlacesUnwrittenModels())
                {
                    Stats.ModelsLeftOut++;
                    continue;
                }
                const auto& [Foliage, Side] = PropInfo(I.Model);
                Candidates.push_back({ &I, Stock ? 1 : (std::max)(size_t(1), Parts(I.Model).size()), Foliage, Side * std::fabs(I.Scale) });
            }
        }
    int Level = Props == "large" ? 2 : Props == "no_foliage" ? 1 : 0;
    double Least = 0;   // the smallest prop kept past the settings
    const auto Keeps = [&](const Candidate& C) { return !(Level >= 1 && C.Foliage) && !(Level >= 2 && C.Size < SmallProp) && !(C.Size < Least); };
    const auto MiscModels = [&]() {
        size_t N = 0;
        for (const Candidate& C : Candidates) N += Keeps(C) ? C.Entries : 0;
        return N;
    };
    {
        constexpr double TileRoom = 2048;   // the world's tiles, written after
        const double Most = (std::min)(Room * Lim.StaticModels, Room * Lim.Entities - (double)EntityNumber) - TileRoom;
        const size_t Before = MiscModels();
        const int Asked = Level;
        while (Fitting && (double)MiscModels() > Most && Level < 2) Level++;
        if (Fitting && (double)MiscModels() > Most)
        {
            std::vector<std::pair<double, size_t>> BySize;   // size, entries of the kept ones
            for (const Candidate& C : Candidates)
                if (Keeps(C)) BySize.push_back({ C.Size, C.Entries });
            std::sort(BySize.begin(), BySize.end());
            double N = (double)MiscModels();
            for (size_t K = 0; K < BySize.size() && N > Most;)
            {
                const double Size = BySize[K].first;
                for (; K < BySize.size() && BySize[K].first == Size; K++) N -= (double)BySize[K].second;
                Least = std::nextafter(Size, 1e300);
            }
        }
        if (Level != Asked || Least > 0)
        {
            size_t Gone = 0;
            for (const Candidate& C : Candidates)
            {
                const int Was = Level;
                const double WasLeast = Least;
                Level = Asked, Least = 0;
                const bool Before = Keeps(C);
                Level = Was, Least = WasLeast;
                Gone += Before && !Keeps(C);
            }
            Stats.PlacementsFitted = Gone;
            Stats.Warnings.push_back("placements fitted to Black Ops III's cod2map: " + std::to_string(Before) + " misc_models -> " + std::to_string(MiscModels()) +
                                     " (" + std::to_string(Lim.StaticModels) + " static models at most, room kept for the world's tiles): " +
                                     (Level == 2 ? "foliage and props under " + std::to_string((int)SmallProp) + " units left out" : "foliage left out") +
                                     (Least > 0 ? ", then props under " + std::to_string((long long)std::ceil(Least)) + " units" : std::string()));
        }
    }
    for (const Candidate& C : Candidates)
    {
        if (Keeps(C))
        {
            Place(*C.I);
            continue;
        }
        if (Level >= 1 && C.Foliage) Stats.FoliageLeftOut++;
        else if (Level >= 2 && C.Size < SmallProp) Stats.SmallLeftOut++;
    }
    // the GDT listing them, LF as it has always been written
    std::vector<std::string> Entries;
    std::map<std::string, size_t> EntryNames;   // a name twice stops gdtdb for every map of the tree
    const auto Listed = [&](const std::string& Name, const std::string& File) {
        EntryNames[Name]++;
        Entries.push_back(Gdt::Entry(Name, "xmodel.gdf", { { "filename", Gdt::Path(Folder + "/" + File) }, { "type", "rigid" } }, "\n"));
    };
    // the target's hard limits checked on what goes out (RadiantTarget.h): each one passed is a
    // line of <map>.limits.txt, which stops a usermap build before its tools run
    const IRadiantTarget::Limits Hard = Target->HardLimits();
    std::vector<std::string> Over;
    const auto Check = [&](const std::vector<XmodelJob>& Of, const std::function<std::string(const XmodelJob&)>& Entry) {
        for (const XmodelJob& J : Of)
        {
            const std::string Name = Entry(J);
            if (Hard.NameLength && Name.size() > Hard.NameLength)
                Over.push_back("model " + Name + ": a name of " + std::to_string(Name.size()) + " characters (" + std::to_string(Hard.NameLength) + " at most)");
            std::map<std::string, size_t> Held;
            for (const Surface* S : J.Surfaces) Held[S->Material] += S->Indices.size();
            if (Hard.ModelSurfaces && Held.size() > Hard.ModelSurfaces)
                Over.push_back("model " + Name + ": " + std::to_string(Held.size()) + " materials (" + std::to_string(Hard.ModelSurfaces) + " at most)");
            for (const auto& [Material, Corners] : Held)
                if (Hard.MaterialVertices && Corners > Hard.MaterialVertices)
                    Over.push_back("model " + Name + ": " + std::to_string(Corners) + " vertices of " + Material + " (" +
                                   std::to_string(Hard.MaterialVertices) + " at most)");
        }
    };
    std::vector<XmodelJob> Jobs;
    for (const std::string& Name : Named)
    {
        if (!Scene.Models.count(Name))
        {
            Stats.ModelsElsewhere++;
            continue;
        }
        const auto& Of = Parts(Name);
        for (size_t P = 0; P < Of.size(); P++)
        {
            XmodelJob& J = Jobs.emplace_back();
            J.Surfaces = Of[P];
            J.Name = PartName(Name, P);
            J.Entry = ModelName(Name, P);
            J.Path = ModelDir / (ExportNaming::SafeFileName(J.Name) + ModelExtension);
        }
    }
    Check(Jobs, [](const XmodelJob& J) { return J.Entry; });
    std::vector<char> Placed;
    if (!WriteXmodels(Jobs, Scene, ForGdt, Placed, [&](size_t Done) { return Stage("writing the placed models", Done, Jobs.size()); })) return false;
    for (size_t I = 0; I < Jobs.size(); I++)
        if (Placed[I])
        {
            Listed(Jobs[I].Entry, Jobs[I].Path.filename().string());
            Stats.ModelFiles++;
        }

    // the world's triangles neither a brush side nor a patch draws in tiles of Tile units (by
    // triangle centre), each its own model; a brush model's one model each. A model has one
    // surface per material, a new one past MaxVertices (a surface's 16-bit indices).
    if (!Stage("cutting the world's models")) return false;
    constexpr double Tile = 2048.0;
    constexpr size_t MaxVertices = 65000;
    std::map<std::string, std::vector<Surface>> Pieces;
    std::map<std::pair<std::string, std::string>, size_t> Open;   // (piece, material) -> its surface being filled
    // the triangles too large for a model (the target's ModelTriangleSpan), patches of the world
    const double MaxSpan = Target->ModelTriangleSpan();
    std::vector<std::pair<const Surface*, size_t>> Oversize, OversizeOverlays;
    std::set<std::string> TexelMaterials;   // the terrain layers' materials the pieces draw (TexelLayers)
    for (size_t Oi = 0; Oi < Scene.Objects.size(); Oi++)
    {
        const Mesh& M = Scene.Objects[Oi];
        const bool IsWorld = M.Name == "world" || M.Name.compare(0, 11, "brushmodel_") != 0;
        // without Patches, the triangles patches would draw too, but for a brush model's an entity
        // holds (they move with it)
        const bool Merged = !Patches && !Patched.count(DrawnWorld::Owner(M.Name));
        for (size_t Si = 0; Si < M.Surfaces.size(); Si++)
        {
            const Surface& S = M.Surfaces[Si];
            if (NoSurface(Scene, S.Material)) continue;
            // S's vertices in the surface each (piece, material) fills there (an overlay's material its own)
            std::map<std::pair<std::string, std::string>, std::map<uint32_t, uint32_t>> Remaps;
            // the blend layers drawn over S (MapRadiantLayers.h): over its triangles a brush side draws too
            const std::vector<const BlendLayer*>& Over = Ov.Layers ? Ov.Layers->Of(S.Material) : NoLayers;
            // a terrain's layers by texel: each triangle its layer's material (TexelLayers)
            const TexelLayers* Texels = TexelsOf(Scene, S);
            // Tri into piece Piece's surface of Material, each vertex as Corner gives it
            const auto Into = [&](const std::string& Piece, const std::string& Material, const uint32_t (&Tri)[3],
                                  const std::function<Vertex(uint32_t)>& Corner) {
                auto& Surfaces = Pieces[Piece];
                auto& Remap = Remaps[{ Piece, Material }];
                auto [It, First] = Open.emplace(std::make_pair(Piece, Material), Surfaces.size());
                if (First || Surfaces[It->second].Vertices.size() + 3 > MaxVertices)
                {
                    It->second = Surfaces.size();
                    Surfaces.push_back({ Material, {}, {} });
                    Remap.clear();
                }
                Surface& To = Surfaces[It->second];
                for (const uint32_t I : Tri)
                {
                    auto [R, Added] = Remap.emplace(I, (uint32_t)To.Vertices.size());
                    if (Added) To.Vertices.push_back(Corner(I));
                    To.Indices.push_back(R->second);
                }
            };
            for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
            {
                if (Cover.Covered(Oi, Si, K / 3)) continue;
                const bool Gone = Drawn.Removed(Oi, Si, K / 3);   // a brush side draws it: its overlays only
                if (Gone && Over.empty()) continue;
                const Fate F = FateOf(S, K / 3);
                if (F != Fate::Model && !(F == Fate::Patch && Merged)) continue;
                if (F == Fate::Patch && FloorPatches && Floor(S, K / 3) && !FloorsInModels.count(TriangleKey(Oi, Si, K / 3))) continue;   // a floor: a patch, with its overlays (above)
                const uint32_t Tri[3] = { S.Indices[K], S.Indices[K + 1], S.Indices[K + 2] };
                if (Tri[0] >= S.Vertices.size() || Tri[1] >= S.Vertices.size() || Tri[2] >= S.Vertices.size()) continue;
                if (!Within(S.Vertices[Tri[0]].Pos) || !Within(S.Vertices[Tri[1]].Pos) || !Within(S.Vertices[Tri[2]].Pos))
                {
                    if (!Gone) Stats.BeyondWorld++;
                    continue;
                }
                if (F == Fate::Patch)
                {
                    double Span = 0;
                    for (int A = 0; A < 3; A++)
                    {
                        const float P[3] = { S.Vertices[Tri[0]].Pos[A], S.Vertices[Tri[1]].Pos[A], S.Vertices[Tri[2]].Pos[A] };
                        Span = (std::max)(Span, (double)(std::max)({ P[0], P[1], P[2] }) - (std::min)({ P[0], P[1], P[2] }));
                    }
                    if (Span > MaxSpan)
                    {
                        if (!Gone) Oversize.emplace_back(&S, K);
                        if (!Over.empty()) OversizeOverlays.emplace_back(&S, K);
                        continue;
                    }
                }
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
                if (!Gone)
                {
                    Stats.TrianglesInModels++;
                    if (const int L = TexelLayer(Texels, S, K); L >= 0)
                    {
                        const TexelLayers::Layer& Ly = Texels->Layers[L];
                        Stats.TexelLayerTriangles++;
                        TexelMaterials.insert(Ly.Material);
                        Into(Piece, Ly.Material, Tri, [&](uint32_t I) { return TexelCorner(S, I, Ly); });
                    }
                    else
                        Into(Piece, S.Material, Tri, [&](uint32_t I) { return S.Vertices[I]; });
                }
                // each blend layer over it: its own surface, white corners of the layer's weight and UVs
                for (const BlendLayer* L : Over)
                {
                    if (!L) continue;
                    if (Unweighted(S, K, L->Layer))
                    {
                        Stats.OverlayUnweighted++;
                        continue;
                    }
                    Overlaid(*L, ForGdt);
                    Into(Piece, L->Name, Tri, [&](uint32_t I) { return OverlayCorner(S, I, L->Layer); });
                    Stats.OverlayInModels[L->Layer - 2]++;
                    Ov.Carried.insert({ S.Material, L->Layer });
                }
            }
        }
    }
    // a terrain layer's surfaces moved by whole repeats toward UV 0
    for (auto& [Name, Surfaces] : Pieces)
        for (Surface& S : Surfaces)
            if (TexelMaterials.count(S.Material)) UvTowardZero(S.Vertices);
    // a piece larger than the target's models take as its parts (ModelParts)
    Jobs.clear();
    // a brush model's piece whose triangles are in its own space (Local): placed where its entity stands
    std::map<std::string, const LocalPlace*> PieceAt;
    for (const auto& [N, L] : Local) PieceAt[Map + "_brushmodel_" + std::to_string(N)] = &L;
    std::vector<const LocalPlace*> JobAt;
    for (const auto& [Name, Surfaces] : Pieces)
    {
        std::vector<const Surface*> All;
        for (const Surface& S : Surfaces) All.push_back(&S);
        const auto Of = ModelParts(All, Target->MaxModelSurfaces(), Target->MaxMaterialVertices(), Cuts);
        const auto At = PieceAt.find(Name);
        for (size_t P = 0; P < Of.size(); P++)
        {
            JobAt.push_back(At == PieceAt.end() ? nullptr : At->second);
            XmodelJob& J = Jobs.emplace_back();
            J.Surfaces = Of[P];
            J.Name = J.Entry = PartName(Name, P);
            J.Path = ModelDir / (J.Name + ModelExtension);
        }
    }
    Check(Jobs, [](const XmodelJob& J) { return J.Entry; });
    std::vector<char> Tiles;
    if (!WriteXmodels(Jobs, Scene, ForGdt, Tiles, [&](size_t Done) { return Stage("writing the world's models", Done, Jobs.size()); })) return false;
    for (size_t I = 0; I < Jobs.size(); I++)
    {
        if (!Tiles[I]) continue;
        const std::string& Name = Jobs[I].Entry;
        Listed(Name, Jobs[I].Path.filename().string());
        Stats.WorldModels++;
        Out += "// entity " + std::to_string(EntityNumber++) + "\n{\n";
        Out += Quote("classname", "misc_model") + Quote("model", Name) + Quote("origin", JobAt[I] ? JobAt[I]->Origin : std::string("0 0 0")) +
               Quote("angles", JobAt[I] ? JobAt[I]->Angles : std::string("0 0 0"));
        for (const auto& [K, V] : Target->WorldModelKeys()) Out += Quote(K, V);
        Out += "}\n";
    }
    if (!Oversize.empty())
    {
        Begin(nullptr, "func_group");
        size_t Number = 0;
        for (const auto& [S, K] : Oversize)
        {
            const TexelLayers* Texels = TexelsOf(Scene, *S);
            if (const int L = TexelLayer(Texels, *S, K); L >= 0)
            {
                Stats.TexelLayerTriangles++;
                WriteTrianglePatch(Out, *S, K, MaterialName(Texels->Layers[L].Material, ForGdt), false, Stats, Number, nullptr, &Texels->Layers[L]);
            }
            else
                WriteTrianglePatch(Out, *S, K, MaterialName(S->Material, ForGdt), Scene.LayerWeights(S->Material), Stats, Number);
        }
        Out += "}\n";
        Stats.OversizePatches += Oversize.size();
    }
    if (!OversizeOverlays.empty())
    {
        std::string Group;
        size_t Number = 0;
        for (const auto& [S, K] : OversizeOverlays) WriteOverlayPatches(Group, *S, K, Ov, ForGdt, Stats, Number);
        if (!Group.empty())
        {
            Begin(nullptr, "func_group");
            Out += Group + "}\n";
        }
    }
    if (InTree)
        for (const auto& [Name, Count] : EntryNames)
            if (Count > 1) Over.push_back("model " + Name + ": " + std::to_string(Count) + " GDT entries of that name (gdtdb refuses the tree)");
    // cod2map's counts of the whole map: its entities, its static models (each misc_model), its
    // materials (the GDT's), its world's drawn triangles (a patch's one); the planes, brushes and
    // brush sides it makes of the brushes as written (MapRadiantPlanes.h), the patches' collision
    {
        const PlaneCount Pc = CountPlanes(Out);
        Stats.PlaneSlots = Pc.Slots, Stats.PlaneSidePlanes = Pc.SidePlanes;
        Stats.BoxBevels = Pc.BoxBevels, Stats.EdgeBevels = Pc.EdgeBevels, Stats.CollisionBrushSides = Pc.CollisionSides;
        if (Hard.PlaneSlots && Pc.Slots > Hard.PlaneSlots)
            Over.push_back(std::to_string(Pc.Slots) + " plane slots, about (cod2map's MAX_MAP_PLANES_COMPILE: " + std::to_string(Hard.PlaneSlots) + "; " +
                           std::to_string(Pc.Brushes) + " brushes, " + std::to_string(Pc.Sides) + " sides, " +
                           std::to_string(Pc.BoxBevels + Pc.EdgeBevels) + " bevels)");
        if (Hard.Brushes && Pc.Brushes > Hard.Brushes)
            Over.push_back(std::to_string(Pc.Brushes) + " brushes (cod2map's MAX_MAP_BRUSHES: " + std::to_string(Hard.Brushes) + ")");
        if (Hard.BrushSides && Pc.CollisionSides > Hard.BrushSides)
            Over.push_back(std::to_string(Pc.CollisionSides) + " brush sides with their bevels, about (cod2map's MAX_MAP_BRUSHSIDES: " +
                           std::to_string(Hard.BrushSides) + ")");
        if (Hard.CollisionTriangles && Stats.PatchCollisionTriangles > Hard.CollisionTriangles)
            Over.push_back(std::to_string(Stats.PatchCollisionTriangles) + " collision triangles of patches (cod2map's MAX_MAP_COLLISIONTRIS: " +
                           std::to_string(Hard.CollisionTriangles) + ")");
        if (Hard.CollisionVertices && Stats.PatchCollisionVertices > Hard.CollisionVertices)
            Over.push_back(std::to_string(Stats.PatchCollisionVertices) + " collision vertices of patches (cod2map's MAX_MAP_COLLISIONVERTS: " +
                           std::to_string(Hard.CollisionVertices) + ")");
        // Radiant counts the exploders by their number (script_exploder)
        std::set<std::string> Exploders;
        const std::string Key = "\"script_exploder\" \"";
        for (size_t At = Out.find(Key); At != std::string::npos; At = Out.find(Key, At + Key.size()))
        {
            const size_t From = At + Key.size(), To = Out.find('"', From);
            if (To != std::string::npos && To > From) Exploders.insert(Out.substr(From, To - From));
        }
        if (Hard.Exploders && Exploders.size() > Hard.Exploders)
            Over.push_back(std::to_string(Exploders.size()) + " exploders (Radiant: \"" + std::to_string(Hard.Exploders) + "... will fail to link\")");
        if (Hard.BrushModels && Stats.BrushEntities > Hard.BrushModels)
            Stats.Warnings.push_back(std::to_string(Stats.BrushEntities) + " brush entities: cod2map takes " + std::to_string(Hard.BrushModels) +
                                     " that do not move (MAX non dyn entity brush models)");
    }
    {
        size_t MiscModels = 0;
        const std::string Misc = "\"classname\" \"misc_model\"";
        for (size_t At = Out.find(Misc); At != std::string::npos; At = Out.find(Misc, At + Misc.size())) MiscModels++;
        const size_t Named = ForGdt ? ForGdt->Materials.size() + ForGdt->StockNamed.size() : 0;
        const size_t Drawn = Stats.Patches + Stats.OverlayPatches[0] + Stats.OverlayPatches[1] + Stats.OverlayPatches[2];
        if (Hard.Entities && EntityNumber > Hard.Entities)
            Over.push_back(std::to_string(EntityNumber) + " entities (cod2map's MAX_MAP_ENTITIES: " + std::to_string(Hard.Entities) + ")");
        if (Hard.StaticModels && MiscModels > Hard.StaticModels)
            Over.push_back(std::to_string(MiscModels) + " misc_models (cod2map: \"Exceeded " + std::to_string(Hard.StaticModels) + " static map models\")");
        if (Hard.Materials && Named > Hard.Materials)
            Over.push_back(std::to_string(Named) + " materials (cod2map's MAX_MAP_MATERIALS: " + std::to_string(Hard.Materials) + ")");
        // the linker's fast file: its xmodels, materials and their images (three a material, about)
        if (const size_t Assets = Stats.ModelFiles + Stats.WorldModels + 4 * Named; Hard.Assets && Assets > Hard.Assets)
            Over.push_back(std::to_string(Assets) + " assets, about (the linker's \"More than " + std::to_string(Hard.Assets) + " assets in fast file\")");
        if (Hard.DrawSurfaces && DrawSurfaces(Stats) > Hard.DrawSurfaces)
            Over.push_back(std::to_string(DrawSurfaces(Stats)) + " draw surfaces, about: a patch or a brush side each (cod2map's MAX_MAP_DRAW_SURFS: " + std::to_string(Hard.DrawSurfaces) + ")");
        if (Hard.CaulkVertices && Stats.CaulkVertices > Hard.CaulkVertices)
            Over.push_back(std::to_string(Stats.CaulkVertices) + " corners of caulk sides (cod2map's \"Temp vertex limit (" + std::to_string(Hard.CaulkVertices) + ") exceeded for layered geometry\")");
        if (Hard.WorldTriangles && Drawn > Hard.WorldTriangles)
            Over.push_back(std::to_string(Drawn) + " patches (cod2map's MAX_MAP_TRIANGLES: " + std::to_string(Hard.WorldTriangles) + " drawn triangles)");
    }
    Stats.LimitsOver = Over.size();
    for (const std::string& Line : Over) Stats.Warnings.push_back("over a Black Ops III limit: " + Line);
    if (Ov.Layers)
    {
        Stats.LayersCarried = Ov.Carried.size();
        Stats.LayersUndrawn = Stats.Layers - Stats.LayersNoColour - Stats.LayersCarried;
        Stats.OverlayMaterials = ForGdt->Overlays.size();
    }
    else
        Stats.LayersNoGdt = Stats.Layers - Stats.LayersNoColour;
    if (!Stage("writing the file")) return false;
    bool Ok = true;
    if (InTree)
    {
        if (std::string Why; !ModToolsTree::WriteKept(Path, Out, &Why))
        {
            Error = "cannot write " + Path.string() + ": " + Why;
            return false;
        }
        const fs::path LimitsFile = fs::path(Path).replace_extension(".limits.txt");
        if (Over.empty()) fs::remove(LimitsFile, Ec);
        else
        {
            std::ofstream L(LimitsFile, std::ios::binary);
            for (const std::string& Line : Over) L << Line << "\n";
        }
    }
    else
    {
        std::ofstream File(Path, std::ios::binary);
        if (!File)
        {
            Error = "cannot create " + Path.string();
            return false;
        }
        File << Out;
        Ok = File.good();
    }
    if (ForGdt)
    {
        ForGdt->Root = Root;
        ForGdt->Map = Folder;
        ForGdt->GdtFile = GdtFile;
        ForGdt->InTree = InTree;
        ForGdt->ModelEntries = std::move(Entries);
        Stats.StockMaterials = ForGdt->StockNamed.size();
        return Ok;
    }
    fs::create_directories(GdtFile.parent_path(), Ec);
    if (InTree) ModToolsTree::LeaveOutDefined(GdtFile, Entries);
    if (std::string Why; InTree && !ModToolsTree::WriteKept(GdtFile, Gdt::Document(Entries, "\n"), &Why))
    {
        Error = "cannot write " + GdtFile.string() + ": " + Why;
        Ok = false;
    }
    else if (!InTree) std::ofstream(GdtFile, std::ios::binary) << Gdt::Document(Entries, "\n");
    return Ok;
}

std::unique_ptr<ISceneWriter> RadiantWriter(bool Materials, const std::string& Tree, bool Patches, const std::string& Game, const std::string& Props,
                                            const std::string& Fit)
{
    return std::make_unique<RadiantSceneWriter>(Materials, Tree, Patches, Game, Props, Fit);
}

bool FoliageName(const std::string& Model)
{
    static const std::set<std::string> Words = { "grass", "foliage", "weed", "weeds", "bush", "bushes", "shrub", "shrubs", "fern", "ferns",
                                                 "ivy", "vine", "vines", "hedge", "plant", "plants", "flower", "flowers", "leaf", "leaves",
                                                 "reed", "reeds", "moss" };
    std::string Word;
    for (size_t I = 0; I <= Model.size(); I++)
    {
        const char C = I < Model.size() ? (char)std::tolower((unsigned char)Model[I]) : ' ';
        if (C >= 'a' && C <= 'z')
        {
            Word += C;
            continue;
        }
        if (Words.count(Word)) return true;
        Word.clear();
    }
    return false;
}
}
