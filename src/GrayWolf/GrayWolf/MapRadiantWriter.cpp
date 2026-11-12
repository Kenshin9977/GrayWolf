#include "stdafx.h"

#include "MapRadiantWriter.h"
#include "RadiantTarget.h"

#include "ExportNaming.h"
#include "GdtWriter.h"
#include "MapLive.h"
#include "MapMaterials.h"
#include "MapRadiantBound.h"
#include "MapRadiantGeometry.h"
#include "MapRadiantTextures.h"
#include "MaterialGdt.h"
#include "ModToolsTree.h"
#include "MapSceneWriter.h"
#include "WraithModel.h"
#include "XMBExport.h"
#include "XMEExport.h"

#include <algorithm>
#include <atomic>
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
    std::string MaterialName(const std::string& Material, RadiantGdt* Gdt)
    {
        if (!Gdt) return Material.empty() ? "default" : ExportNaming::SafeFileName(Material);
        Gdt->Materials.insert(Material);
        return MaterialGdt::PrefixedName(Gdt->Prefix, Material);
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

    PreparedBrush PrepareBrush(const Brush& B, const float* Origin, const float* Axis, const ToolNames& Tools, const std::string& Only,
                               const Texturing& Tex, DrawnWorld::Scratch* Scratch = nullptr)
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
        // each side's material and projection
        std::vector<std::string>& Names = R.Names;
        for (const BrushFace& F : Faces) Names.push_back(Only.empty() ? Tools.Of(Moved, Moved.Sides[F.Side]) : Only);
        std::vector<SideTexture>& Textures = R.Textures;
        std::vector<char>& Textured = R.Textured;
        Textures.resize(Faces.size());
        Textured.assign(Faces.size(), 0);
        if (Tex.Drawn && Tex.Stats)
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
                Out += Names[K] + " 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n";
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
                    const std::string& Only, RadiantStats& Stats, const Texturing& Tex = {})
    {
        return CommitBrush(Out, PrepareBrush(B, Origin, Axis, Tools, Only, Tex), Number, Stats, Tex);
    }

    // Brushes prepared on the machine's threads (each its own scratch), in their order; Tick as
    // ParallelFor's (false: stopped, Out incomplete)
    bool PrepareBrushes(const std::vector<Brush>& Brushes, const ToolNames& Tools, const Texturing& Tex, std::vector<PreparedBrush>& Out,
                        const std::function<bool(size_t Done)>& Tick)
    {
        Out.assign(Brushes.size(), PreparedBrush());
        return ParallelFor<DrawnWorld::Scratch>(Brushes.size(), 64, [&](size_t I, DrawnWorld::Scratch& Scratch) {
            Out[I] = PrepareBrush(Brushes[I], nullptr, nullptr, Tools, "", Tex, &Scratch);
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

    // The drawn triangles of Owner's meshes (DrawnWorld::Owner) no brush side draws, each a patch:
    // a 2x2 mesh whose fourth vertex repeats the second, one triangle whichever way Radiant splits
    // the cell, facing as the game's (a compiled mesh's triangles face against (row 1 - row 0) x
    // (column 1 - column 0)); nonColliding (the clip map's brushes collide); each vertex its
    // position, colour ("c", left out when white; white for layer weights: Weights), UV ("t": UV x 1024) and lightmap coordinates
    // (the plane's texture axes, a texel per 16 units, as "2 2 16 8" says)
    // Drawn triangle K of S (a Fate::Patch one) as patch Number, its material Name. With Over, the
    // blend layer Over drawn over it (MapRadiantLayers.h): its corners white, the layer's weight as
    // alpha, the layer's UVs, a decal in the layer's decal layer as the mod tools' blend patches are
    // ("c 255 255 255 <a>", decalLayerSort), on the triangle itself (BO3 sorts a decal over the
    // coplanar surface under it: Materials.pdf, "authored on coplanar geometry")
    void WriteTrianglePatch(std::string& Out, const Surface& S, size_t K, const std::string& Name, bool Weights, RadiantStats& Stats,
                            size_t& Number, const BlendLayer* Over = nullptr)
    {
        const uint32_t Ix[3] = { S.Indices[K], S.Indices[K + 1], S.Indices[K + 2] };
        const Vertex* V[3] = { &S.Vertices[Ix[0]], &S.Vertices[Ix[1]], &S.Vertices[Ix[2]] };
        V3 P[3];
        for (int J = 0; J < 3; J++) P[J] = { V[J]->Pos[0], V[J]->Pos[1], V[J]->Pos[2] };
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
    void WriteOverlayPatches(std::string& Out, const Surface& S, size_t K, Overlaying& Ov, RadiantGdt* Gdt, RadiantStats& Stats, size_t& Number)
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
            WriteTrianglePatch(Out, S, K, MaterialName(L->Name, Gdt), false, Stats, Number, L);
            Ov.Carried.insert({ S.Material, L->Layer });
        }
    }

    // Owner's drawn triangles no brush side draws as patches, then the blend layers over all its
    // drawn triangles (those a brush side draws too)
    void WritePatches(std::string& Out, const MapScene& Scene, const DrawnWorld& Drawn, uint32_t Owner, RadiantGdt* Gdt, RadiantStats& Stats,
                      size_t& Number, Overlaying& Ov, const CoveredTriangles& Cover)
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
                for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                {
                    if (Drawn.Removed(Oi, Si, K / 3) || Cover.Covered(Oi, Si, K / 3)) continue;
                    const Fate F = FateOf(S, K / 3);
                    if (F == Fate::None) Stats.TrianglesDegenerate++;
                    if (F != Fate::Patch) continue;
                    if (Name.empty()) Name = MaterialName(S.Material, Gdt);
                    WriteTrianglePatch(Out, S, K, Name, Scene.LayerWeights(S.Material), Stats, Number);
                }
                if (Ov.Layers && !Ov.Layers->Of(S.Material).empty())
                    for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                        if (FateOf(S, K / 3) == Fate::Patch && !Cover.Covered(Oi, Si, K / 3)) WriteOverlayPatches(Out, S, K, Ov, Gdt, Stats, Number);
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
                    M.MaterialName = MaterialGdt::PrefixedName(ForGdt->Prefix, S->Material);
                    ForGdt->Materials.insert(S->Material);
                }
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
                // same texel drawn)
                V3 Face{}, P[3];
                bool Own = false;
                if (Bin)
                {
                    for (int J = 0; J < 3; J++)
                    {
                        const Vertex& V = S->Vertices[S->Indices[K + J]];
                        P[J] = { V.Pos[0], V.Pos[1], V.Pos[2] };
                    }
                    Face = Cross(Sub(P[1], P[0]), Sub(P[2], P[0]));
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
        std::vector<std::set<std::string>> Materials(Jobs.size());
        struct None {};
        const bool Whole = ParallelFor<None>(Jobs.size(), 1, [&](size_t I, None&) {
            RadiantGdt Own;
            if (ForGdt) Own.Prefix = ForGdt->Prefix;
            Written[I] = WriteXmodel(Jobs[I].Surfaces, Scene, Jobs[I].Name, Jobs[I].Path, ForGdt ? &Own : nullptr);
            Materials[I] = std::move(Own.Materials);
        }, Tick);
        if (ForGdt)
            for (const auto& M : Materials) ForGdt->Materials.insert(M.begin(), M.end());
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
        RadiantSceneWriter(bool Materials, std::string Into, bool AsPatches, std::string Of)
            : WithMaterials(Materials), Tree(std::move(Into)), Patches(AsPatches), Game(std::move(Of)) {}
        const char* Extension() const override { return ".map"; }
        bool Write(const MapScene& Scene, const std::string& Path, const Materials::IAdapter& Adapter, SceneWriteResult& Result,
                   std::string& Error, const WriteProgress& Progress) const override
        {
            RadiantStats Stats;
            RadiantGdt G;
            if (!WriteRadiant(Scene, Path, Stats, Error, WithMaterials ? &G : nullptr, Tree, Patches, Game, Progress))
            {
                Error = "radiant map: " + Error;
                return false;
            }
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
                                { "map_radiant_beyond_world", Stats.BeyondWorld },
                                { "map_radiant_triangles_covered", Stats.TrianglesCovered },
                                { "map_radiant_beyond_sky_box", Stats.BeyondSkyBox },
                                { "map_radiant_clipped_to_sky_box", Stats.ClippedToSkyBox },
                                { "map_radiant_beyond_sky_box_placements", Stats.BeyondSkyBoxPlacements },
                                { "map_radiant_beyond_sky_box_entities", Stats.BeyondSkyBoxEntities },
                                { "map_radiant_sky_box_reach", Stats.SkyBoxReach },
                                { "map_radiant_sky_box_size", Stats.SkyBoxSize },
                                { "map_radiant_oversize_patches", Stats.OversizePatches },
                                { "map_radiant_scaled_to_nothing", Stats.ScaledToNothing },
                                { "map_radiant_collision_patches", Stats.CollisionPatches },
                                { "map_radiant_brushes_over_budget", Stats.BrushesOverBudget },
                                { "map_radiant_limits_over", Stats.LimitsOver },
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
                                                            { "map_radiant_overlay_materials", Stats.OverlayMaterials },
                                                            { "map_radiant_overlay_patches_layer2", Stats.OverlayPatches[0] },
                                                            { "map_radiant_overlay_patches_layer3", Stats.OverlayPatches[1] },
                                                            { "map_radiant_overlay_patches_layer4", Stats.OverlayPatches[2] },
                                                            { "map_radiant_overlay_triangles_in_models_layer2", Stats.OverlayInModels[0] },
                                                            { "map_radiant_overlay_triangles_in_models_layer3", Stats.OverlayInModels[1] },
                                                            { "map_radiant_overlay_triangles_in_models_layer4", Stats.OverlayInModels[2] },
                                                            { "map_radiant_overlay_triangles_unweighted", Stats.OverlayUnweighted },
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
        std::string Tree;   // a mod tools tree, "" for beside the map
        bool Patches;       // the drawn world as patches rather than models
        std::string Game;   // the source game's id

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
                for (size_t I = 0; I < Described.size(); I++) B.Add(Described[I], G.Root, Layered[I] != 0, Sorts[I]);
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
                if (G.InTree) ModToolsTree::WriteKept(G.GdtFile, Gdt::Document(Entries, "\n"));
                else std::ofstream(G.GdtFile, std::ios::binary) << Gdt::Document(Entries, "\n");
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
    // rows, y along them, as the .map writes them: the triangles face against (Ctrl[x + 1][y] -
    // Ctrl[x][y]) x (Ctrl[x][y + 1] - Ctrl[x][y]), as the game's) and Turned[x][y], the cell from
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
            // rows of cells (j, across) of cells (i, along): Ctrl[j][i] the bottom-left corner of cell (i, j)
            const size_t Width = Best[0].size(), Height = Best.size();
            TerrainGrid G;
            G.Ctrl.assign(Height + 1, std::vector<V3>(Width + 1));
            G.Turned.assign(Height + 1, std::vector<char>(Width + 1, 0));
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
                    G.Ctrl[J][I] = Points[Bl];
                    G.Ctrl[J][I + 1] = Points[Br];
                    G.Ctrl[J + 1][I] = Points[Tl];
                    G.Ctrl[J + 1][I + 1] = Points[Tr];
                    G.Turned[J][I] = Diagonals[Q] == EdgeOf(Bl, Tr);
                }
            Out.push_back(std::move(G));
        }
        return Out;
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
                  const std::string& Tree, bool Patches, const std::string& Game, const WriteProgress& Progress)
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
    const std::unique_ptr<IRadiantTarget> Target = InTree ? Bo3ModToolsTarget(ExportNaming::SafeFileName(Game), Stem) : PlainTarget(Stem);
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
    if (ForGdt) ForGdt->Prefix = Target->NamePrefix();
    // a model the map's export writes, by the name its GDT entry has
    // a model's (its part's, ModelParts) GDT entry: the target's name for one the export writes
    const auto ModelName = [&](const std::string& Model, size_t Part = 0) {
        return Scene.Models.count(Model) ? Target->ModelName(PartName(Model, Part)) : PartName(Model, Part);
    };
    std::erase_if(Ents, [&](Entity& E) { return !Target->Admit(E, Scene, Stats); });
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
    // the layered materials' later layers, drawn over them when the GDT defines their materials
    const BlendLayers Layers(Scene);
    Overlaying Ov;
    if (ForGdt) Ov.Layers = &Layers;
    Stats.Layers = Layers.Layers, Stats.LayersNoColour = Layers.LayersNoColour;
    for (const Mesh& M : Scene.Objects)
        for (const Surface& S : M.Surfaces)
            if (Scene.LayerWeights(S.Material))
                for (const Vertex& V : S.Vertices)
                    Stats.VertexWeightsWhite += V.Color[0] != 255 || V.Color[1] != 255 || V.Color[2] != 255 || V.Color[3] != 255;
    if (!Stage("the world's solid brushes")) return false;
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
    {
        constexpr const char* Texturing = "texturing the world's brushes";
        std::vector<PreparedBrush> Prepared;
        if (!Stage(Texturing, 0, WorldBrushes->size()) ||
            !PrepareBrushes(*WorldBrushes, Tools, Tex, Prepared, [&](size_t Done) { return Stage(Texturing, Done, WorldBrushes->size()); }))
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
        std::vector<std::array<V3, 3>> All, Rest;
        // Past the target's budget of collision triangles, only those near where players start
        // (PlayArea): the scenery's far off cost cod2map planes (MAX_MAP_PLANES_COMPILE for IW's
        // mp_flip) and no player reaches them
        const bool Crop = PlayArea.Found && (double)std::count(Kept.begin(), Kept.end(), 1) > Target->CollisionTriangleBudget();
        for (size_t T = 0; T < Kept.size(); T++)
        {
            if (!Kept[T]) continue;
            const std::array<V3, 3> Tri{ Corner(T, 0), Corner(T, 1), Corner(T, 2) };
            if (Crop && !PlayArea.Holds(Tri))
            {
                Stats.CollisionOutsidePlay++;
                continue;
            }
            const V3 N = Cross(Sub(Tri[1], Tri[0]), Sub(Tri[2], Tri[0]));
            const double L = Length(N);
            (L > 0 && std::abs(N[2]) / L < Steep ? Rest : All).push_back(Tri);
        }
        if (!Stage("collision: terrain patches"))
        {
            Stopped = true;
            return;
        }
        for (const TerrainGrid& G : TerrainGrids(All, Rest, Target->MinPatchTriangleArea()))
        {
            Out += "// brush " + std::to_string(Number++) + "\n {\n  mesh\n  {\n   clip_full\n   lightmap_gray\n   " + std::to_string(G.Ctrl.size()) + " " +
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
        }
        if (!Stage("collision: triangles merged plane by plane"))
        {
            Stopped = true;
            return;
        }
        const IRadiantTarget::Merge M = Target->CollisionMerge();
        const std::vector<Polygon> Polygons = CoplanarPolygons(Rest, Stats.TrianglesDropped, M.Distance, M.MinDot);
        // Past the target's budget of planes (the prisms' sides with the clip map's brushes'), the
        // polygons as patches instead, a triangle each (a fan of the polygon), which take no plane:
        // cod2map ends at MAX_MAP_PLANES (131072) for Ghosts' and AW's collision. A patch of one
        // triangle cannot roof anything as a grid of walls did.
        size_t Planes = 0;
        for (const auto& B : Scene.Collision.Brushes) Planes += B.Sides.size();
        for (const Polygon& G : Polygons) Planes += G.Loop.size() + 2;
        if ((double)Planes > Target->CollisionPlaneBudget())
        {
            std::vector<std::array<const V3*, 3>> Fan;
            for (const Polygon& G : Polygons)
            {
                for (size_t J = 1; J + 1 < G.Loop.size(); J++)
                {
                    const std::array<const V3*, 3> C = { &G.Loop[0], &G.Loop[J], &G.Loop[J + 1] };
                    if (Sliver(*C[0], *C[1], *C[2], Target->MinPatchTriangleArea()))
                    {
                        Stats.TrianglesDropped++;
                        continue;
                    }
                    Fan.push_back(C);
                }
                Stats.Triangles += G.Triangles;
            }
            const auto V = [&](const V3& P) { return "\tv " + Num(P[0]) + " " + Num(P[1]) + " " + Num(P[2]) + " t 0 0 0 0\n"; };
            for (const auto& C : Fan)
            {
                // faces as the game's: the grid's triangle faces against (row 1 - row 0) x
                // (column 1 - column 0), the fourth corner repeating the second
                Out += "// brush " + std::to_string(Number++) + "\n {\n  mesh\n  {\n   clip_full\n   lightmap_gray\n   2 2 16 8\n   (\n" + V(*C[0]) +
                       V(*C[1]) + "   )\n   (\n" + V(*C[2]) + V(*C[1]) + "   )\n  }\n }\n";
                Stats.CollisionPatches++;
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
            if (WriteBrush(Out, B, Number, nullptr, nullptr, Tools, "clip_full", Stats, { Tex.Drawn, ForGdt, 0, &Stats.PrismTextures, &Hidden }))
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
    if (!Scene.Collision.Triangles.empty())
    {
        Begin(nullptr, "func_group");
        BrushNumber = 0;
        Prisms(Scene.Collision.Triangles, BrushNumber);
        if (Stopped) return false;
        Out += "}\n";
    }

    // the other entities; a brush entity's brushes moved to where it stands. Named: the models
    // they and the placements name, written as files below
    std::set<std::string> Named;
    // past the target's budget of brush sides (the world's with the brush models'), the brush
    // models of the most sides keep their drawn triangles only, largest first (IW's mp_flip: three
    // animated buildings of 19380 brushes, one a triangle each)
    std::set<size_t> Stripped;
    {
        size_t Sides = 0;
        for (const Brush& B : Scene.Collision.Brushes) Sides += B.Sides.size();
        std::vector<std::pair<size_t, size_t>> Models;   // sides, brush model
        std::set<size_t> Seen;
        for (const Entity& E : Ents)
        {
            const auto Model = E.find("model");
            if (&E == World || Model == E.end() || Model->second.empty() || Model->second[0] != '*') continue;
            const size_t N = (size_t)std::strtoul(Model->second.c_str() + 1, nullptr, 10);
            if (N >= Scene.Collision.Models.size() || !Seen.insert(N).second) continue;
            size_t Own = 0;
            for (const Brush& B : Scene.Collision.Models[N]) Own += B.Sides.size();
            Sides += Own;
            Models.push_back({ Own, N });
        }
        std::sort(Models.rbegin(), Models.rend());
        for (const auto& [Own, N] : Models)
        {
            if ((double)Sides <= Target->BrushSideBudget()) break;
            Stripped.insert(N);
            Sides -= Own;
            Stats.BrushesOverBudget += Scene.Collision.Models[N].size();
        }
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
            if ((N >= From.size() || From[N].empty() || (Kind == '*' && Stripped.count(N))) && !HasDrawn)
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
            const Texturing Own{ Kind == '*' ? Tex.Drawn : nullptr, ForGdt, (uint32_t)N, &Stats.EntityTextures, &Hidden };
            if (N < From.size() && !(Kind == '*' && Stripped.count(N)))
                for (const Brush& B : From[N])
                    if (WriteBrush(Out, B, Number, Origin, Axis, Tools, Trigger ? "trigger" : "", Stats, Own))
                    {
                        Number++;
                        if (Kind == '?') Stats.Brushes--, Stats.TriggerBrushes++;   // not the clip map's
                    }
            if (Kind == '*' && N > 0 && Patched.insert((uint32_t)N).second) WritePatches(Out, Scene, Drawn, (uint32_t)N, ForGdt, Stats, Number, Ov, Cover);
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
    // entity holds, as patches; else they go to the world's models below
    if (Patches)
    {
        if (!Stage("the world's patches")) return false;
        std::string Group;
        size_t Number = 0;
        WritePatches(Group, Scene, Drawn, 0, ForGdt, Stats, Number, Ov, Cover);
        for (const uint32_t O : Owners)
            if (!Patched.count(O)) WritePatches(Group, Scene, Drawn, O, ForGdt, Stats, Number, Ov, Cover);
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
    for (const auto* List : { &Scene.StaticInstances, &Scene.DynamicInstances, &Scene.SplinedInstances })
        for (const Instance& I : *List)
            if (I.Model.empty()) continue;
            else if (!Within(I.Origin)) Stats.BeyondWorld++;
            else if (!Scene.Models.count(I.Model) && !Target->PlacesUnwrittenModels()) Stats.ModelsLeftOut++;
            else if (I.Scale < Target->MinModelScale()) Stats.ScaledToNothing++;
            else Place(I);

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
    // a piece larger than the target's models take as its parts (ModelParts)
    Jobs.clear();
    for (const auto& [Name, Surfaces] : Pieces)
    {
        std::vector<const Surface*> All;
        for (const Surface& S : Surfaces) All.push_back(&S);
        const auto Of = ModelParts(All, Target->MaxModelSurfaces(), Target->MaxMaterialVertices(), Cuts);
        for (size_t P = 0; P < Of.size(); P++)
        {
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
        Out += Quote("classname", "misc_model") + Quote("model", Name) + Quote("origin", "0 0 0") + Quote("angles", "0 0 0");
        for (const auto& [K, V] : Target->WorldModelKeys()) Out += Quote(K, V);
        Out += "}\n";
    }
    if (!Oversize.empty())
    {
        Begin(nullptr, "func_group");
        size_t Number = 0;
        for (const auto& [S, K] : Oversize)
            WriteTrianglePatch(Out, *S, K, MaterialName(S->Material, ForGdt), Scene.LayerWeights(S->Material), Stats, Number);
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
    if (Hard.BrushSides && Stats.Faces > Hard.BrushSides)
        Over.push_back(std::to_string(Stats.Faces) + " brush sides (cod2map stopped on MAX_MAP_PLANES_COMPILE with " +
                       std::to_string(Hard.BrushSides) + ")");
    Stats.LimitsOver = Over.size();
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
        if (!ModToolsTree::WriteKept(Path, Out))
        {
            Error = "cannot write " + Path.string() + " (nor set the one there aside)";
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
        return Ok;
    }
    fs::create_directories(GdtFile.parent_path(), Ec);
    if (InTree) Ok = ModToolsTree::WriteKept(GdtFile, Gdt::Document(Entries, "\n")) && Ok;
    else std::ofstream(GdtFile, std::ios::binary) << Gdt::Document(Entries, "\n");
    return Ok;
}

std::unique_ptr<ISceneWriter> RadiantWriter(bool Materials, const std::string& Tree, bool Patches, const std::string& Game)
{
    return std::make_unique<RadiantSceneWriter>(Materials, Tree, Patches, Game);
}
}
