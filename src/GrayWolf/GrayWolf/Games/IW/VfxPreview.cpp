#include "stdafx.h"

// The preview adapter of the vfx core: an emitter's first state as one FxPreview element, its
// modules by canonical name. Times in seconds become msec; spawn: burst mode (emitter flag 2) one
// burst, or bursts at the spawn rate over the emitter's life; otherwise particles at the spawn
// rate (the EmissionGraph's mean when there is one) over the emitter's life, for ever when it has
// none, as many as particleCountMax allows. Graphs (keys linear, times their scale; two sets of
// curves picked between at random under flag 0x10) are resampled on Samples intervals; a graph
// overrides the init value, a velocity graph adds to it under flag 0x400 (GRAPH_ADDITIVE).
// Skipped: disabled modules (flag 1), vector fields, attractors, particle sims, ray casts,
// states after the first.

#include "Vfx.h"

#include "FxPreviewModel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace Vfx
{
namespace
{
    using Json = nlohmann::json;
    using FxPreview::Range;

    constexpr int Samples = 16;
    // module flags; Linear: the graph is the straight line from its begin to its end value (SizeGraph
    // and VelocityGraph, the exe's 0xd20c00 under flag 0x1000)
    constexpr uint32_t Disabled = 1, RandomBetween = 0x10, WorldSpace = 0x80, Additive = 0x400, Linear = 0x1000;
    constexpr uint32_t BurstMode = 2, InfiniteLife = 8;

    uint32_t FlagsOf(const Json& M)
    {
        const auto It = M.find("flags");
        if (It == M.end() || !It->is_string()) return 0;
        return (uint32_t)std::strtoull(It->get<std::string>().c_str(), nullptr, 16);
    }
    float Num(const Json& J, float Default = 0) { return J.is_number() ? J.get<float>() : Default; }
    std::string Str(const Json& J, const char* Key)
    {
        const auto It = J.is_object() ? J.find(Key) : J.end();
        return It != J.end() && It->is_string() ? It->get<std::string>() : std::string();
    }
    float At(const Json& J, const char* Key, int I, float Default = 0)
    {
        const auto It = J.find(Key);
        return It != J.end() && It->is_array() && (int)It->size() > I ? Num((*It)[I], Default) : Default;
    }
    Range MinMax(float Min, float Max) { return { Min, Max - Min }; }

    struct Curve
    {
        float Scale = 1;
        std::vector<std::pair<float, float>> Keys;
        float operator()(float T) const
        {
            if (Keys.empty()) return 0;
            if (T <= Keys.front().first) return Keys.front().second * Scale;
            for (size_t I = 1; I < Keys.size(); I++)
                if (T <= Keys[I].first)
                {
                    const auto& [T0, V0] = Keys[I - 1];
                    const auto& [T1, V1] = Keys[I];
                    const float F = T1 > T0 ? (T - T0) / (T1 - T0) : 1.0f;
                    return (V0 + (V1 - V0) * F) * Scale;
                }
            return Keys.back().second * Scale;
        }
        float Mean() const
        {
            float S = 0;
            for (int I = 0; I <= Samples; I++) S += (*this)((float)I / Samples);
            return S / (Samples + 1);
        }
    };
    std::vector<Curve> CurvesOf(const Json& M)
    {
        std::vector<Curve> Out;
        const auto It = M.find("curves");
        if (It == M.end() || !It->is_array()) return Out;
        for (const auto& C : *It)
        {
            Curve K;
            K.Scale = Num(C.value("scale", Json()), 1);
            if (const auto Keys = C.find("keys"); Keys != C.end() && Keys->is_array())
                for (const auto& P : *Keys)
                    if (P.is_array() && P.size() >= 2) K.Keys.emplace_back(Num(P[0]), Num(P[1]));
            Out.push_back(std::move(K));
        }
        return Out;
    }
    // a graph's channel C at T: set A's, and as the amplitude set B's minus A's when the module
    // picks between them (each set Per curves)
    Range Graph(const std::vector<Curve>& Cs, uint32_t Flags, int Per, int C, float T)
    {
        if (C >= (int)Cs.size()) return {};
        const float A = Cs[C](T);
        if (!(Flags & RandomBetween) || C + Per >= (int)Cs.size()) return { A, 0 };
        return { A, Cs[C + Per](T) - A };
    }

    FxPreview::ElemType TypeOf(const std::string& T)
    {
        using E = FxPreview::ElemType;
        static const std::pair<const char*, E> Types[] = {
            { "billboardSprite", E::Billboard }, { "orientedSprite", E::Oriented }, { "tail", E::Tail }, { "geoTrail", E::Trail },
            { "cloud", E::Cloud }, { "sparkCloud", E::Cloud }, { "model", E::Model }, { "lightOmni", E::Light },
            { "lightSpot", E::SpotLight }, { "decal", E::Decal }, { "flare", E::LensFlare }, { "beam", E::Beam },
            { "runner", E::Runner }, { "vfxLight", E::Light },
        };
        for (const auto& [N, Ty] : Types)
            if (T == N) return Ty;
        return E::Other;
    }

    // the CoD angles (pitch, yaw, roll) of a quaternion's rotation (x, y, z, w)
    void QuatAngles(float X, float Y, float Z, float W, float* Ang)
    {
        const float N = std::sqrt(X * X + Y * Y + Z * Z + W * W);
        if (N < 1e-6f) return;
        X /= N, Y /= N, Z /= N, W /= N;
        const float Fx = 1 - 2 * (Y * Y + Z * Z), Fy = 2 * (X * Y + W * Z), Fz = 2 * (X * Z - W * Y);
        const float Lz = 2 * (Y * Z + W * X), Uz = 1 - 2 * (X * X + Y * Y);
        Ang[0] = -std::asin(std::clamp(Fz, -1.0f, 1.0f));
        Ang[1] = std::atan2(Fy, Fx);
        Ang[2] = std::atan2(Lz, Uz);
    }

    struct State
    {
        const Json& S;
        // the first enabled module of that name in a group
        const Json* Find(const char* Group, const char* Name) const
        {
            const auto G = S.find(Group);
            if (G == S.end() || !G->is_array()) return nullptr;
            for (const auto& M : *G)
                if (Str(M, "module") == Name && !(FlagsOf(M) & Disabled)) return &M;
            return nullptr;
        }
    };

    void Materials(const Json* M, FxPreview::Element& El, int& Rows, int& Cols)
    {
        if (!M) return;
        const auto A = M->find("assets");
        if (A == M->end() || !A->is_array()) return;
        for (const auto& V : *A)
        {
            const Json* Mat = &V;
            // a decal: its first material
            if (const auto D = V.find("decal"); D != V.end() && D->is_array() && !D->empty()) Mat = &(*D)[0];
            const std::string N = Str(*Mat, "material");
            if (N.empty()) continue;
            FxPreview::Material Out;
            Out.Name = N;
            FxPreview::ReadMaterial(*Mat, Out);
            if (Rows <= 1 && Cols <= 1)
            {
                Rows = std::max(1, (int)At(*Mat, "atlas", 0, 1));
                Cols = std::max(1, (int)At(*Mat, "atlas", 1, 1));
            }
            El.Materials.push_back(std::move(Out));
        }
    }
    std::vector<std::string> Names(const Json* M, const char* Key)
    {
        std::vector<std::string> Out;
        if (!M) return Out;
        if (const auto A = M->find("assets"); A != M->end() && A->is_array())
            for (const auto& V : *A)
                if (std::string N = Str(V, Key); !N.empty()) Out.push_back(N);
        return Out;
    }

    bool Element(const Json& E, const Json& Sj, FxPreview::Element& El)
    {
        using namespace FxPreview;
        const State St{ Sj };
        const auto Ty = Sj.find("elementType");
        El.TypeName = Ty == Sj.end() ? std::string() : Ty->is_string() ? Ty->get<std::string>() : Ty->dump();
        El.Type = TypeOf(El.TypeName);
        const uint32_t Ef = FlagsOf(E);

        // ---- timing (seconds -> msec)
        El.LifeMsec = MinMax(At(E, "particleLife", 0) * 1000, At(E, "particleLife", 1) * 1000);
        const float EmitterLife = At(E, "emitterLife", 1) * 1000;
        if (Ef & InfiniteLife) El.LifeMsec = { std::max(EmitterLife, 5000.0f), 0 };
        El.LifeMsec.Base = std::max(1.0f, El.LifeMsec.Base);
        const float EmitterDelay = At(E, "emitterDelay", 0) * 1000;
        El.DelayMsec = MinMax(At(E, "particleDelay", 0) * 1000 + EmitterDelay, At(E, "particleDelay", 1) * 1000 + EmitterDelay);
        float Rate = 0.5f * (At(E, "spawnRate", 0) + At(E, "spawnRate", 1));
        if (const Json* G = St.Find("update", "EmissionGraph"))
            if (const auto Cs = CurvesOf(*G); !Cs.empty()) Rate = Cs[0].Mean();
        const Range Burst = MinMax(At(E, "burstCount", 0, 1), At(E, "burstCount", 1, 1));
        const float CountMax = Num(E.value("particleCountMax", Json()), 0);
        if ((Ef & BurstMode) && EmitterLife <= 0)
        {
            El.Group = SpawnGroup::OneShot;
            El.OneShotCount = Burst;
        }
        else
        {
            El.Group = SpawnGroup::Looping;
            float Interval = Rate > 0 ? 1000.0f / Rate : 1000.0f;
            // no more alive at once than the emitter keeps
            const float LifeMax = El.LifeMsec.Base + std::max(0.0f, El.LifeMsec.Amp);
            const float PerSpawn = (Ef & BurstMode) ? Burst.Base + 0.5f * Burst.Amp : 1.0f;
            if (CountMax > 0 && EmitterLife <= 0) Interval = std::max(Interval, LifeMax * PerSpawn / CountMax);
            El.IntervalMsec = std::max(1, (int)std::lround(Interval));
            El.PerInterval = (Ef & BurstMode) ? Burst : Range{ 1, 0 };
            El.LoopCount = EmitterLife > 0 ? std::max(1, (int)std::lround(EmitterLife / Interval)) : 0;
        }

        // ---- where it spawns. The shapes as IW7's init functions place a point (sphere 0xd17420,
        // cylinder 0xd16970 -> 0xd18230): a radius between its min and max (the max alone under
        // spawnFlags), a cylinder's height from -halfHeight to +halfHeight along its z, its radius
        // in x, y; each axis kept by axisFlags (0xd18420: per axis bit 1 positive, 2 negative, none
        // zero), turned by directionQuat under hasRotation, then moved by offset.
        const auto Shape = [&](const Json& M) {
            El.Shaped = true;
            El.AxisFlags = (uint8_t)(M.contains("axisFlags") ? (int)Num(M.value("axisFlags", Json()), 63) : 63);
            const float Lo = At(M, "radius", 0), Hi = At(M, "radius", 1);
            El.OffsetRadius = Num(M.value("spawnFlags", Json())) != 0 ? Range{ Hi, 0 } : MinMax(Lo, Hi);
            if (M.value("hasRotation", Json()) == true)
            {
                float Q[4], N = 0;
                for (int K = 0; K < 4; K++) Q[K] = At(M, "directionQuat", K, K == 3 ? 1.0f : 0.0f), N += Q[K] * Q[K];
                if (N > 1e-8f)
                    for (int K = 0; K < 4; K++) El.ShapeQuat[K] = Q[K] / std::sqrt(N);
            }
            for (int K = 0; K < 3; K++) El.Origin[K] = { At(M, "offset", K), 0 };
        };
        if (const Json* M = St.Find("init", "InitSpawnShapeSphere"))
        {
            El.Offset = SpawnOffset::Sphere;
            Shape(*M);
        }
        else if (const Json* M = St.Find("init", "InitSpawnShapeCylinder"))
        {
            El.Offset = SpawnOffset::Cylinder;
            Shape(*M);
            const float H = Num(M->value("halfHeight", Json()));
            El.OffsetHeight = { -H, 2 * H };
        }
        else if (const Json* M = St.Find("init", "InitSpawnShapeBox"))
        {
            for (int K = 0; K < 3; K++) El.Origin[K] = MinMax(At(*M, "dimensionsMin", K), At(*M, "dimensionsMax", K));
        }
        else if (const Json* M = St.Find("init", "InitSpawnShapeEllipsoid"))
        {
            El.Offset = SpawnOffset::Sphere;
            float Lo = 0, Hi = 0;
            for (int K = 0; K < 3; K++) Lo += At(*M, "radiusMin", K) / 3, Hi += At(*M, "radiusMax", K) / 3;
            El.OffsetRadius = MinMax(Lo, Hi);
            for (int K = 0; K < 3; K++) El.Origin[K] = { At(*M, "offset", K), 0 };
        }
        // velocity frame: local, world, or away from the effect's origin
        bool InitWorld = false;
        if (const Json* M = St.Find("init", "InitRelativeVelocity"))
        {
            const int T = (int)Num(M->value("velocityType", Json()));
            InitWorld = T == 1 || T == 4;
            if (T == 2 || T == 5) El.Relative = RunRelative::Offset;
        }
        if (const Json* M = St.Find("init", "InitOrientedSprite"))
        {
            // the quaternion turns the sprite's normal from its z, as directionQuat turns a spawn
            // shape's z (MW2019's water ripples: both the same quarter turn about y, the disc and its
            // ring lying on the water when the effect's forward is the surface's normal); the
            // preview's quad faces its x: the quaternion applied after a turn taking x to z
            const float Qx = At(*M, "orientationQuat", 0), Qy = At(*M, "orientationQuat", 1), Qz = At(*M, "orientationQuat", 2),
                        Qw = At(*M, "orientationQuat", 3, 1);
            const float S = 0.70710678f;   // the turn: -90 degrees about y, (0, -S, 0, S)
            float Ang[3] = {};
            QuatAngles(S * (Qx + Qz), S * (Qy - Qw), S * (Qz - Qx), S * (Qw + Qy), Ang);
            for (int K = 0; K < 3; K++) El.Angles[K] = { Ang[K], 0 };
        }
        if (const Json* M = St.Find("init", "InitRotation3D"))
        {
            // rotations about x (roll), y (pitch), z (yaw); rates per second -> per msec
            static const int Axis[3] = { 2, 0, 1 };
            for (int K = 0; K < 3; K++)
            {
                El.Angles[Axis[K]] = MinMax(El.Angles[Axis[K]].Base + At(*M, "angleMin", K), El.Angles[Axis[K]].Base + At(*M, "angleMax", K));
                El.AngularVelocity[Axis[K]] = MinMax(At(*M, "rateMin", K) / 1000, At(*M, "rateMax", K) / 1000);
            }
        }

        // ---- what it draws
        int Rows = 1, Cols = 1;
        Materials(St.Find("init", "InitMaterial"), El, Rows, Cols);
        Materials(St.Find("init", "InitDecal"), El, Rows, Cols);
        El.Models = Names(St.Find("init", "InitModel"), "model");
        El.Effects = Names(St.Find("init", "InitRunner"), "effect");
        for (const auto& [Test, Out] : { std::pair<const char*, std::string*>{ "TestDeath", &El.OnDeath }, { "TestImpact", &El.OnImpact },
                                         { "TestBirth", &El.Attached } })
            if (const auto N = Names(St.Find("test", Test), "effect"); !N.empty()) *Out = N[0];
        if (const Json* M = St.Find("init", "InitAtlas"))
        {
            // the grid: the material's, here or once the zone defining it is read (Library)
            El.AtlasRows = Rows, El.AtlasCols = Cols;
            El.AtlasFromMaterial = true;
            const int Start = (int)Num(M->value("startFrame", Json())), Loops = (int)Num(M->value("loopCount", Json()), -1);
            El.AtlasStart = Start < 0 ? 1 : 0;
            El.AtlasIndex = std::max(0, Start);
            El.AtlasFps = std::max(0, (int)std::lround(Num(M->value("playRate", Json()))));
            El.AtlasLoops = std::max(0, Loops);
            El.AtlasLoopLimited = Loops > 0;
        }
        if (const Json* M = St.Find("update", "Gravity"))
            if ((int)Num(E.value("gravityOptions", Json())) != 2) {
                El.Gravity = MinMax(At(*M, "gravity", 0), At(*M, "gravity", 1));
                // MWIII's is in g (386.09 in/s2), the earlier games' in 800 in/s2: the same effects
                // hold 0.5 there, 1.036 here
                if (M->contains("gravityG"))
                    El.Gravity = MinMax(At(*M, "gravityG", 0) * (386.09f / 800.0f), At(*M, "gravityG", 1) * (386.09f / 800.0f));
            }

        // ---- graphs over the life, resampled
        const Json* Attr = St.Find("init", "InitAttributes");
        const Json* ColorG = St.Find("update", "ColorGraph");
        const Json* ColorL = St.Find("update", "ColorLerp");
        const Json* SizeG = St.Find("update", "SizeGraph");
        const Json* SizeL = St.Find("update", "SizeLerp");
        const Json* Intensity = St.Find("update", "IntensityGraph");
        const Json* VelG = St.Find("update", "VelocityGraph");
        const Json* Force = St.Find("update", "Force");
        const Json* Rot = St.Find("init", "InitRotation");
        const Json* RotG = St.Find("update", "RotationGraph");
        const auto Cc = ColorG ? CurvesOf(*ColorG) : std::vector<Curve>{};
        const auto Sc = SizeG ? CurvesOf(*SizeG) : std::vector<Curve>{};
        const auto Ic = Intensity ? CurvesOf(*Intensity) : std::vector<Curve>{};
        const auto Vc = VelG ? CurvesOf(*VelG) : std::vector<Curve>{};
        const auto Rc = RotG ? CurvesOf(*RotG) : std::vector<Curve>{};
        const uint32_t Cf = ColorG ? FlagsOf(*ColorG) : 0, Sf = SizeG ? FlagsOf(*SizeG) : 0, If = Intensity ? FlagsOf(*Intensity) : 0;
        const uint32_t Vf = VelG ? FlagsOf(*VelG) : 0, Rf = RotG ? FlagsOf(*RotG) : 0, Ff = Force ? FlagsOf(*Force) : 0;
        const bool VelWorld = (Vf & WorldSpace) != 0, AttrWorld = InitWorld || (Attr && (FlagsOf(*Attr) & WorldSpace));
        const float MeanLife = (El.LifeMsec.Base + 0.5f * std::max(0.0f, El.LifeMsec.Amp)) / 1000.0f;
        El.InitialRotation = Rot ? MinMax(At(*Rot, "angle", 0), At(*Rot, "angle", 1)) : Range{};
        const Range RotRate = Rot ? MinMax(At(*Rot, "rate", 0), At(*Rot, "rate", 1)) : Range{};
        bool NonUniform = false;
        std::vector<Range> RateAt(Samples + 1);
        std::vector<std::array<std::array<Range, 3>, 2>> Vel(Samples + 1);   // [local, world][axis], units/s
        for (int I = 0; I <= Samples; I++)
        {
            const float T = (float)I / Samples;
            VisualSample V;
            for (int K = 0; K < 4; K++)
            {
                Range C{ 1, 0 };
                if (ColorG) C = Graph(Cc, Cf, 4, K, T);
                else if (ColorL) C = { At(*ColorL, "colorBegin", K, 1) + (At(*ColorL, "colorEnd", K, 1) - At(*ColorL, "colorBegin", K, 1)) * T, 0 };
                else if (Attr) C = MinMax(At(*Attr, "colorMin", K, 1), At(*Attr, "colorMax", K, 1));
                V.ColorBase[K] = std::clamp(C.Base, 0.0f, 1.0f);
                V.ColorAmp[K] = std::clamp(C.Base + C.Amp, 0.0f, 1.0f);
            }
            for (int K = 0; K < 2; K++)
            {
                Range S{ 1, 0 };
                if (SizeG && (Sf & Linear)) S = { At(*SizeG, "sizeBegin", K) + (At(*SizeG, "sizeEnd", K) - At(*SizeG, "sizeBegin", K)) * T, 0 };
                else if (SizeG) S = Graph(Sc, Sf, 3, K, T);
                else if (SizeL) S = { At(*SizeL, "sizeBegin", K) + (At(*SizeL, "sizeEnd", K) - At(*SizeL, "sizeBegin", K)) * T, 0 };
                else if (Attr) S = MinMax(At(*Attr, "sizeMin", K), At(*Attr, "sizeMax", K));
                // the games' sizes are widths: half extents here
                V.Size[K] = { S.Base * 0.5f, S.Amp * 0.5f };
            }
            NonUniform |= std::fabs(V.Size[0].Base - V.Size[1].Base) > 1e-3f || std::fabs(V.Size[0].Amp - V.Size[1].Amp) > 1e-3f;
            // a model's or a cloud's scale, a light's radius: the size's first channel
            V.Scale = { V.Size[0].Base * 2, V.Size[0].Amp * 2 };
            if (El.Type == ElemType::Light || El.Type == ElemType::SpotLight)
            {
                V.LightRadius = V.Scale;
                V.LightIntensity = Intensity ? Graph(Ic, If, 1, 0, T) : Range{ 1, 0 };
            }
            // on a sprite, a tail or a flare (MW2019, MWIII: smoke 2, a fireball 3 fading to 1): its
            // colour's brightness
            else if (Intensity && !Ic.empty())
                V.Intensity = Graph(Ic, If, 1, 0, T);
            El.Visual.push_back(V);
            RateAt[I] = RotRate;
            if (RotG && RotG->value("useRotationRate", Json()) == true) RateAt[I] = Graph(Rc, Rf, 1, 0, T);
            // velocity: the init range, a graph over or onto it, a constant force's gain
            for (int K = 0; K < 3; K++)
            {
                const Range Init = Attr ? MinMax(At(*Attr, "velocityMin", K), At(*Attr, "velocityMax", K)) : Range{};
                Vel[I][AttrWorld ? 1 : 0][K] = Init;
                if (VelG)
                {
                    const Range G = (Vf & Linear) ? Range{ At(*VelG, "velocityBegin", K) + (At(*VelG, "velocityEnd", K) - At(*VelG, "velocityBegin", K)) * T, 0 }
                                                  : Graph(Vc, Vf, 3, K, T);
                    Range& Into = Vel[I][VelWorld ? 1 : 0][K];
                    if (Vf & Additive) Into = { Into.Base + G.Base, Into.Amp + G.Amp };
                    else
                    {
                        Vel[I][AttrWorld ? 1 : 0][K] = {};
                        Into = G;
                    }
                }
                if (Force)
                {
                    const Range A = MinMax(At(*Force, "forceMin", K), At(*Force, "forceMax", K));
                    Range& Into = Vel[I][(Ff & WorldSpace) ? 1 : 0][K];
                    Into.Base += A.Base * T * MeanLife;
                    Into.Amp += A.Amp * T * MeanLife;
                }
            }
        }
        El.NonUniformScale = NonUniform;
        // the sim's rotation and velocity samples: per msec over one sample's share of the life,
        // and what was covered before (the quadratic the sim integrates)
        Range Total{};
        for (int I = 0; I <= Samples; I++)
        {
            VisualSample& V = El.Visual[I];
            V.RotationDelta = { RateAt[I].Base / 1000 / Samples, RateAt[I].Amp / 1000 / Samples };
            V.RotationTotal = Total;
            if (I < Samples)
            {
                const Range Next{ RateAt[I + 1].Base / 1000 / Samples, RateAt[I + 1].Amp / 1000 / Samples };
                Total = { Total.Base + 0.5f * (V.RotationDelta.Base + Next.Base), Total.Amp + 0.5f * (V.RotationDelta.Amp + Next.Amp) };
            }
        }
        bool Moves[2] = {};
        El.Velocity.resize(Samples + 1);
        for (int F = 0; F < 2; F++)
            for (int K = 0; K < 3; K++)
            {
                Range Sum{};
                for (int I = 0; I <= Samples; I++)
                {
                    const Range& V = Vel[I][F][K];
                    Moves[F] |= V.Base != 0 || V.Amp != 0;
                    VelocitySample& S = El.Velocity[I];
                    S.Velocity[F][K] = { V.Base / 1000 / Samples, V.Amp / 1000 / Samples };
                    S.TotalDelta[F][K] = Sum;
                    if (I < Samples)
                    {
                        const Range& N = Vel[I + 1][F][K];
                        Sum = { Sum.Base + 0.5f * (V.Base + N.Base) / 1000 / Samples, Sum.Amp + 0.5f * (V.Amp + N.Amp) / 1000 / Samples };
                    }
                }
            }
        El.UseLocalVelocity = Moves[0];
        El.UseWorldVelocity = Moves[1];
        return true;
    }
}

bool ReadPreview(const nlohmann::json& J, FxPreview::Effect& Fx, std::string& Error)
{
    const auto Es = J.find("emitters");
    if (Es == J.end() || !Es->is_array())
    {
        Error = "not a vfx effect (no emitters)";
        return false;
    }
    Fx.Name = Str(J, "name");
    Fx.Game = J.contains("game") ? Str(J, "game") : "vfx";
    Fx.Timed = true;
    Fx.LoopingLifeMsec = 0;
    for (const auto& E : *Es)
    {
        if (FlagsOf(E) & Disabled) continue;
        const auto Ss = E.find("states");
        if (Ss == E.end() || !Ss->is_array() || Ss->empty()) continue;
        // an emitter whose fields are not what the writer writes: left out, the others drawn
        FxPreview::Element El;
        try
        {
            if (Element(E, (*Ss)[0], El)) Fx.Elements.push_back(std::move(El));
        }
        catch (const std::exception&) {}
    }
    return true;
}
}
