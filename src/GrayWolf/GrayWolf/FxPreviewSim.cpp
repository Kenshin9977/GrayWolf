#include "stdafx.h"

#include "FxPreviewSim.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>

namespace FxPreview
{
namespace
{
    constexpr float Pi = 3.14159265358979f;
    constexpr float GravityUnits = 800.0f;   // units/s^2 the gravity range is a fraction of

    // the randoms a particle draws, by what they decide
    enum Key : uint32_t
    {
        KLife = 1, KDelay, KCount, KOrigin0, KOrigin1, KOrigin2, KOffsetDir0, KOffsetDir1, KOffsetRadius, KOffsetYaw,
        KOffsetHeight, KVel0, KVel1, KVel2, KAngle0, KAngle1, KAngle2, KAngVel0, KAngVel1, KAngVel2, KGravity,
        KColor, KSize0, KSize1, KRotation, KInitialRotation, KScale, KAtlas, KMaterial, KModel, KChild, KEmit,
        KChildSeed, KSparkSpeed, KSparkDir0, KSparkDir1, KBounce,
    };

    uint64_t Mix(uint64_t X)
    {
        X += 0x9e3779b97f4a7c15ull;
        X = (X ^ (X >> 30)) * 0xbf58476d1ce4e5b9ull;
        X = (X ^ (X >> 27)) * 0x94d049bb133111ebull;
        return X ^ (X >> 31);
    }

    struct Rng
    {
        uint64_t Base;
        Rng(uint32_t Seed, uint32_t Elem, int64_t Seq) : Base(Mix(Mix(((uint64_t)Seed << 32) ^ Elem) ^ (uint64_t)Seq)) {}
        float operator()(uint32_t K) const { return (float)((Mix(Base ^ ((uint64_t)K * 0x632be59bd9b4e019ull)) >> 40) * (1.0 / 16777216.0)); }
        uint32_t Bits(uint32_t K) const { return (uint32_t)Mix(Base ^ ((uint64_t)K * 0x632be59bd9b4e019ull)); }
    };

    using Vec = std::array<float, 3>;
    Vec Add(const Vec& A, const Vec& B) { return { A[0] + B[0], A[1] + B[1], A[2] + B[2] }; }
    Vec Sub(const Vec& A, const Vec& B) { return { A[0] - B[0], A[1] - B[1], A[2] - B[2] }; }
    Vec Mul(const Vec& A, float S) { return { A[0] * S, A[1] * S, A[2] * S }; }
    float Dot(const Vec& A, const Vec& B) { return A[0] * B[0] + A[1] * B[1] + A[2] * B[2]; }
    Vec Cross(const Vec& A, const Vec& B) { return { A[1] * B[2] - A[2] * B[1], A[2] * B[0] - A[0] * B[2], A[0] * B[1] - A[1] * B[0] }; }
    float Length(const Vec& A) { return std::sqrt(Dot(A, A)); }
    Vec Normalized(const Vec& A, const Vec& Fallback)
    {
        const float L = Length(A);
        return L > 1e-6f ? Mul(A, 1.0f / L) : Fallback;
    }

    // a frame: origin and axes (rows: forward, left, up)
    struct Orient
    {
        Vec O{};
        Vec A[3] = { Vec{ 1, 0, 0 }, Vec{ 0, 1, 0 }, Vec{ 0, 0, 1 } };
        Vec ToWorld(const Vec& L) const { return Add(O, Dir(L)); }
        Vec Dir(const Vec& L) const { return Add(Add(Mul(A[0], L[0]), Mul(A[1], L[1])), Mul(A[2], L[2])); }
    };

    // the CoD angles (pitch, yaw, roll) as axes in a frame
    void AnglesToAxis(const float* Ang, const Orient& F, Vec Out[3])
    {
        const float Cy = std::cos(Ang[1]), Sy = std::sin(Ang[1]), Cp = std::cos(Ang[0]), Sp = std::sin(Ang[0]);
        const float Cr = std::cos(Ang[2]), Sr = std::sin(Ang[2]);
        Out[0] = F.Dir({ Cp * Cy, Cp * Sy, -Sp });
        Out[1] = F.Dir({ Sr * Sp * Cy - Cr * Sy, Sr * Sp * Sy + Cr * Cy, Sr * Cp });
        Out[2] = F.Dir({ Cr * Sp * Cy + Sr * Sy, Cr * Sp * Sy - Sr * Cy, Cr * Cp });
    }

    // where a particle is, in the world, Age msec into a Life msec life
    struct Placement
    {
        Orient Frame;     // the frame its velocity graphs run in
        Vec Local{};      // its spawn origin in that frame
        Vec Offset{};     // the spawn offset, in the world (none when the frame is the offset's)
    };

    Vec PositionAt(const Element& E, const Rng& R, const Placement& P, float Life, float Age)
    {
        Age = std::clamp(Age, 0.0f, Life);
        Vec Local = P.Local, World{};
        const size_t Count = E.Velocity.size();
        if (Count >= 2 && Life > 0)
        {
            const int N = (int)Count - 1;
            const float Point = std::clamp(Age / Life, 0.0f, 1.0f) * N;
            const int I = std::min(N - 1, (int)std::floor(Point));
            const float S = Point - I, W1 = 0.5f * S * S, W0 = S - W1;
            const float Rnd[3] = { R(KVel0), R(KVel1), R(KVel2) };
            const auto Step = [&](int Frame, Vec& Out) {
                const VelocitySample& A = E.Velocity[I];
                const VelocitySample& B = E.Velocity[I + 1];
                for (int K = 0; K < 3; K++)
                    Out[K] += Life * (A.TotalDelta[Frame][K].At(Rnd[K]) + A.Velocity[Frame][K].At(Rnd[K]) * W0 +
                                      B.Velocity[Frame][K].At(Rnd[K]) * W1);
            };
            if (E.UseLocalVelocity) Step(0, Local);
            if (E.UseWorldVelocity) Step(1, World);
        }
        Vec Pos = Add(Add(P.Frame.ToWorld(Local), P.Offset), World);
        const float G = E.Gravity.At(R(KGravity)), Sec = Age / 1000.0f;
        Pos[2] -= 0.5f * G * GravityUnits * Sec * Sec;
        return Pos;
    }

    // the visual graphs at Age: colour, size, rotation, scale
    void VisualAt(const Element& E, const Rng& R, float Life, float Age, Particle& P)
    {
        if (E.Visual.empty()) return;
        const int N = std::max(1, (int)E.Visual.size() - 1);
        const float Norm = Life > 0 ? std::clamp(Age / Life, 0.0f, 1.0f) : 0.0f;
        const float Point = Norm * N;
        const int I = std::min(N - 1, (int)std::floor(Point));
        const float S = Point - I;
        const VisualSample& A = E.Visual[std::min<size_t>(I, E.Visual.size() - 1)];
        const VisualSample& B = E.Visual[std::min<size_t>(I + 1, E.Visual.size() - 1)];
        const float Rc = R(KColor);
        for (int K = 0; K < 4; K++)
        {
            const float From = A.ColorBase[K] * (1 - Rc) + A.ColorAmp[K] * Rc, To = B.ColorBase[K] * (1 - Rc) + B.ColorAmp[K] * Rc;
            P.Color[K] = From + (To - From) * S;
        }
        const float R0 = R(KSize0), R1 = E.NonUniformScale ? R(KSize1) : R0;
        P.Size[0] = A.Size[0].At(R0) + (B.Size[0].At(R0) - A.Size[0].At(R0)) * S;
        const int Second = E.NonUniformScale ? 1 : 0;
        P.Size[1] = A.Size[Second].At(R1) + (B.Size[Second].At(R1) - A.Size[Second].At(R1)) * S;
        const float Rr = R(KRotation), W1 = 0.5f * S * S, W0 = S - W1;
        P.Rotation = E.InitialRotation.At(R(KInitialRotation)) +
                     Life * (A.RotationTotal.At(Rr) + A.RotationDelta.At(Rr) * W0 + B.RotationDelta.At(Rr) * W1);
        const float Rs = R(KScale);
        P.Scale = A.Scale.At(Rs) + (B.Scale.At(Rs) - A.Scale.At(Rs)) * S;
        const auto Lerp = [&](const Range& X, const Range& Y) { return X.At(Rs) + (Y.At(Rs) - X.At(Rs)) * S; };
        P.LightIntensity = Lerp(A.LightIntensity, B.LightIntensity);
        P.LightRadius = Lerp(A.LightRadius, B.LightRadius);
        P.LightFov = Lerp(A.LightFov, B.LightFov);
        P.Intensity = Lerp(A.Intensity, B.Intensity);
        // a light's def where the graphs give nothing (the Infinity Ward games' spot lights)
        if (P.LightRadius <= 0 && E.Light.Reach > 0) P.LightRadius = E.Light.Reach;
        if (P.LightFov <= 0 && E.Light.HalfFov > 0) P.LightFov = 2 * E.Light.HalfFov;
        if (P.LightIntensity <= 0 && E.Light.Brightness > 0) P.LightIntensity = E.Light.Brightness;
    }

    void AtlasAt(const Element& E, const Rng& R, int64_t Seq, float Life, float Age, float* Uv)
    {
        const int Count = E.AtlasCols * E.AtlasRows;
        if (Count <= 1)
        {
            Uv[0] = Uv[1] = 0;
            Uv[2] = Uv[3] = 1;
            return;
        }
        // any grid (Infinite Warfare's materials give their own), not only the power of two Treyarch's bits give
        int Index = E.AtlasStart == 1 ? (int)(R(KAtlas) * Count) : E.AtlasStart == 2 ? (int)(Seq % Count) : E.AtlasIndex;
        if (E.AtlasOverLife) Index += (int)(Count * (Life > 0 ? std::clamp(Age / Life, 0.0f, 1.0f) : 0.0f));
        else if (E.AtlasFps) Index += (int)(E.AtlasFps * Age / 1000.0f);
        if (E.AtlasLoopLimited && Index >= Count * std::max(1, E.AtlasLoops)) Index = Count - 1;
        Index = ((Index % Count) + Count) % Count;
        Uv[2] = 1.0f / E.AtlasCols;
        Uv[3] = 1.0f / E.AtlasRows;
        Uv[0] = (Index % E.AtlasCols) * Uv[2];
        Uv[1] = (float)(Index / E.AtlasCols) * Uv[3];   // the row: an integer
    }

    float MaxOf(const Range& R) { return R.Base + std::max(0.0f, R.Amp); }
    // numbers a preview can draw: finite and within a hundred million units (a map is under a million)
    bool Finite(const float* V, int N)
    {
        for (int K = 0; K < N; K++)
            if (!std::isfinite(V[K]) || std::fabs(V[K]) > 1e8f) return false;
        return true;
    }
    float RangeInt(const Range& R, float Rnd)   // base + an integer in [0, amplitude]
    {
        return R.Base + std::floor((std::max(0.0f, R.Amp) + 1) * Rnd);
    }

    // A spark fountain's particle Age msec into its Life: its sparks (SparkFountain), each a tail
    // along its velocity, launched where the particle spawned, their cone around the way it
    // goes there (its velocity graphs' direction, not their speed: the def gives the sparks'),
    // falling, bouncing on the ground at Ground; a spark at rest is not drawn. False when the
    // particle cap was reached.
    bool Sparks(const Element& E, const Rng& R, uint32_t Seed, uint32_t Elem, int64_t Seq, const Placement& P, float Life, float Age,
                float Ground, size_t MaxParticles, Frame& Out)
    {
        const SparkFountain& F = *E.Fountain;
        if (F.Count <= 0) return true;
        const Vec From = PositionAt(E, R, P, Life, 0);
        const float Dt = std::min(1.0f, Life);
        const Vec Base = Mul(Sub(PositionAt(E, R, P, Life, Dt), From), 1.0f / Dt);   // units/msec
        const Vec Axis = Normalized(Base, P.Frame.A[0]);
        const Vec Other = std::fabs(Axis[2]) < 0.9f ? Vec{ 0, 0, 1 } : Vec{ 1, 0, 0 };
        const Vec U = Normalized(Cross(Other, Axis), Vec{ 0, 1, 0 }), W = Cross(Axis, U);
        // launched again every loop, each flight at most a loop and within the particle's life
        const float Loop = F.LoopTime >= 1.0f ? F.LoopTime : Life;
        const int64_t Cycle = (int64_t)std::floor(Age / Loop);
        const float T = Age - (float)Cycle * Loop, Flight = std::min(Loop, Life);
        if (T >= Flight || !std::isfinite(T)) return true;
        const float G = F.Gravity, Rest = std::max(0.0f, F.RestSpeed) / 1000.0f;
        // the boost: BoostFactor times faster at launch, back to its speed over BoostTime, linearly
        // (how far ahead it puts the spark, in msec of its own speed)
        float Boost = 0;
        if (F.BoostFactor > 1 && F.BoostTime > 0)
            Boost = (F.BoostFactor - 1) * (T < F.BoostTime ? T - T * T / (2 * F.BoostTime) : F.BoostTime * 0.5f);
        for (int K = 0; K < F.Count; K++)
        {
            if (Out.Particles.size() >= MaxParticles) return false;
            const Rng S(Seed, Elem ^ 0x5f0000u, Seq * 1000003 + Cycle * 4099 + K);
            // its direction within the cone: a uniform point of the sphere's cap ConeFrac of it
            const float C = 1 - 2 * F.ConeFrac * S(KSparkDir0), Sn = std::sqrt(std::max(0.0f, 1 - C * C)), A = 2 * Pi * S(KSparkDir1);
            const float Speed = F.VelMin + (F.VelMax - F.VelMin) * S(KSparkSpeed);
            Vec V = Mul(Add(Add(Mul(Axis, C), Mul(U, Sn * std::cos(A))), Mul(W, Sn * std::sin(A))), Speed);
            Vec O = Add(From, Mul(V, Boost));
            float Tl = T;
            bool Resting = false;
            // its flights from bounce to bounce: z = O.z + V.z t + G t^2 / 2 down to the ground
            for (int Bounce = 0; Bounce < 8; Bounce++)
            {
                const float H = O[2] - Ground;
                float Hit = -1;
                if (H >= 0 && G < 0) Hit = (-V[2] - std::sqrt(V[2] * V[2] - 2 * G * H)) / G;
                else if (H >= 0 && G == 0 && V[2] < 0) Hit = H / -V[2];
                if (!(Hit >= 0) || Hit > Tl) break;
                const float Keep = std::max(0.0f, F.BounceFrac + F.BounceRand * S(KBounce + Bounce));
                O = { O[0] + V[0] * Hit, O[1] + V[1] * Hit, Ground };
                V = { V[0] * Keep, V[1] * Keep, -(V[2] + G * Hit) * Keep };
                Tl -= Hit;
                if (Length(V) <= Rest || V[2] < 1e-4f)
                {
                    Resting = true;
                    break;
                }
            }
            if (Resting) continue;
            Vec Pos = Add(O, Mul(V, Tl));
            Pos[2] += 0.5f * G * Tl * Tl;
            Pos[2] = std::max(Pos[2], std::min(Ground, O[2]));
            const Vec Vel = { V[0], V[1], V[2] + G * Tl };
            Particle Q;
            Q.El = &E;
            Q.Type = ElemType::Tail;
            Q.Seed = S.Bits(KChildSeed + 100);
            if (!E.Materials.empty()) Q.Mat = &E.Materials[std::min<size_t>(E.Materials.size() - 1, (size_t)(S(KMaterial) * E.Materials.size()))];
            const Vec Dir = Normalized(Vel, Axis);
            for (int X = 0; X < 3; X++)
            {
                Q.Pos[X] = Pos[X];
                Q.Dir[X] = Dir[X];
                for (int L = 0; L < 3; L++) Q.Axis[X][L] = P.Frame.A[X][L];
            }
            VisualAt(E, S, Flight, T, Q);
            AtlasAt(E, S, K, Flight, T, Q.Uv);
            // a streak SparkLength long, as long as its path so far at first
            const float Streak = F.Length > 0 ? F.Length : 2 * Q.Size[1];
            Q.Size[1] = 0.5f * std::min(Streak, Length(Sub(Pos, From)));
            if (!Finite(Q.Pos, 3) || !Finite(Q.Size, 2) || !Finite(Q.Dir, 3)) continue;
            Out.Particles.push_back(Q);
        }
        return true;
    }

    // A trail's texture along it: once over it, or with its profile's def every RepeatDist units
    // of it and scrolled with the time (wrapped: not for an atlas frame)
    void TrailTexture(const Element& E, double Now, TrailStrip& S)
    {
        S.Def = E.Trail ? &*E.Trail : nullptr;
        const size_t N = S.Points.size();
        for (size_t K = 0; K < N; K++) S.Points[K].V = N > 1 ? (float)K / (N - 1) : 0.0f;
        if (!S.Def || S.Uv[2] < 1 || S.Uv[3] < 1) return;
        const float Repeat = S.Def->RepeatDist;
        const float Scroll = S.Def->ScrollTimeMsec ? (float)std::fmod(Now / S.Def->ScrollTimeMsec, 1.0) : 0.0f;
        if (Repeat <= 0 && Scroll == 0) return;
        float D = 0;
        for (size_t K = 0; K < N; K++)
        {
            if (K)
            {
                const auto& A = S.Points[K - 1].Pos;
                const auto& B = S.Points[K].Pos;
                D += Length(Vec{ B[0] - A[0], B[1] - A[1], B[2] - A[2] });
            }
            S.Points[K].V = (Repeat > 0 ? std::fmod(D / Repeat, 1024.0f) : S.Points[K].V) + Scroll;
        }
        S.Wrap = true;
    }
}

Simulator::Simulator(std::shared_ptr<Library> L, std::shared_ptr<const Effect> Root) : Lib(std::move(L)), RootFx(std::move(Root)) {}

const Effect* Simulator::Child(const std::string& Name)
{
    if (Name.empty() || !Lib) return nullptr;
    auto Fx = Lib->ByName(Name);
    if (!Fx) return nullptr;
    if (std::find(Keep.begin(), Keep.end(), Fx) == Keep.end()) Keep.push_back(Fx);
    return Fx.get();
}

double Simulator::Span(const Effect* Fx, int Depth)
{
    if (!Fx || Depth > Opt.MaxDepth) return 0;
    if (const auto It = Spans.find(Fx); It != Spans.end()) return It->second;
    Spans[Fx] = 0;   // an effect that starts itself stops here
    double Most = 0;
    for (const auto& E : Fx->Elements)
    {
        // a sound's or a decal's life draws nothing here
        if (E.Type == ElemType::Sound || E.Type == ElemType::Decal || E.Type == ElemType::Other || E.UnderwaterOnly) continue;
        double D = MaxOf(E.DelayMsec) + MaxOf(E.LifeMsec);
        if (E.Group == SpawnGroup::Looping)
        {
            const double Loop = Fx->LoopingLifeMsec > 0 && Fx->LoopingLifeMsec < INT_MAX ? Fx->LoopingLifeMsec : Opt.ChildLoopMsec;
            D += Loop;
        }
        double Children = 0;
        for (const auto& N : E.Effects) Children = std::max(Children, Span(Child(N), Depth + 1));
        for (const std::string* N : { &E.OnDeath, &E.Emitted, &E.Attached }) Children = std::max(Children, Span(Child(*N), Depth + 1));
        Most = std::max(Most, D + Children);
    }
    Spans[Fx] = Most;
    return Most;
}

float Simulator::GroundZ() const
{
    const Effect& Fx = *RootFx;
    const bool Standing = Fx.BoundsKnown && Fx.BoundsCenter[2] >= Fx.BoundsHalf[2] * 0.5f;
    return Opt.ForwardUp || Standing ? 0.0f : -Opt.GroundBelow;
}

double Simulator::DurationMsec()
{
    const Effect& Fx = *RootFx;
    double D = Span(&Fx, 0);
    if (Fx.Loops())
    {
        double Life = 0;
        for (const auto& E : Fx.Elements) Life = std::max(Life, (double)MaxOf(E.LifeMsec));
        if (Fx.LoopingLifeMsec > 0 && Fx.LoopingLifeMsec < INT_MAX) D = std::max(D, Fx.LoopingLifeMsec + Life);
        else D = std::max(1000.0, Life * 2);
    }
    return std::max(D, 100.0);
}

void Simulator::Evaluate(double Msec, Frame& Out)
{
    if (Cached && CachedMsec == Msec)
    {
        Out = Last;
        return;
    }
    Out = Frame{};
    Instance Root;
    Root.Fx = RootFx.get();
    Root.Seed = Opt.Seed;
    if (Opt.ForwardUp)
    {
        // forward (X) up, left kept: X->Z, Z->-X
        Root.Axis[0][0] = 0, Root.Axis[0][1] = 0, Root.Axis[0][2] = 1;
        Root.Axis[2][0] = -1, Root.Axis[2][1] = 0, Root.Axis[2][2] = 0;
    }
    if (RootFx->LoopingLifeMsec > 0 && RootFx->LoopingLifeMsec < INT_MAX) Root.LoopStopMsec = RootFx->LoopingLifeMsec;
    Run(Root, Msec, Out);
    Last = Out;
    CachedMsec = Msec;
    Cached = true;
}

void Simulator::Run(const Instance& I, double Now, Frame& Out)
{
    const double T = Now - I.StartMsec;
    if (T < 0 || !I.Fx) return;
    // effects started by effects: at most MaxEffects a frame (an emitter every half unit of
    // every debris' path started thousands)
    if (Out.Effects >= Opt.MaxEffects)
    {
        Out.Capped = true;
        return;
    }
    const float Ground = GroundZ();
    Out.Effects++;
    Orient Fx;
    Fx.O = { I.Origin[0], I.Origin[1], I.Origin[2] };
    for (int K = 0; K < 3; K++) Fx.A[K] = { I.Axis[K][0], I.Axis[K][1], I.Axis[K][2] };

    for (uint32_t Ei = 0; Ei < (uint32_t)I.Fx->Elements.size(); Ei++)
    {
        const Element& E = I.Fx->Elements[Ei];
        if (E.Type == ElemType::Sound || E.Type == ElemType::Other || E.UnderwaterOnly) continue;   // a type the preview does not know (a vfx id the layout does not name): nothing drawn
        const float MaxLife = std::max(1.0f, MaxOf(E.LifeMsec)), MaxDelay = std::max(0.0f, MaxOf(E.DelayMsec));
        // what this element's particles start, and how long that may still draw
        const Effect* Runner = nullptr;
        const Effect* OnDeath = nullptr;
        const Effect* Emitted = nullptr;
        const bool Deeper = I.Depth < Opt.MaxDepth;
        double ChildSpan = 0;
        if (Deeper)
        {
            if ((E.Type == ElemType::Runner || !E.Effects.empty()) && !E.Effects.empty()) Runner = Child(E.Effects[0]);
            if (!Runner && !E.Attached.empty()) Runner = Child(E.Attached);
            OnDeath = Child(E.OnDeath);
            Emitted = Child(E.Emitted);
            for (const Effect* C : { Runner, OnDeath, Emitted }) ChildSpan = std::max(ChildSpan, Span(C, I.Depth + 1));
        }
        const double Window = MaxLife + MaxDelay + ChildSpan;

        // the spawns that may matter now: (sequence, spawn time)
        struct Spawn { int64_t Seq; double At; };
        std::vector<Spawn> Spawns;
        if (E.Group == SpawnGroup::Looping)
        {
            const Rng R0(I.Seed, Ei, -1);
            const double Delay = E.DelayMsec.At(R0(KDelay));
            const double Interval = std::max(1, E.IntervalMsec);
            const double Stop = std::min(I.LoopStopMsec, T);
            int64_t First = (int64_t)std::floor((T - Window - Delay) / Interval);
            int64_t Last = (int64_t)std::floor((Stop - Delay) / Interval);
            First = std::max<int64_t>(0, First);
            if (E.LoopCount > 0) Last = std::min<int64_t>(Last, E.LoopCount - 1);
            First = std::max(First, Last - 20000);
            for (int64_t K = First; K <= Last; K++)
            {
                const Rng Rk(I.Seed, Ei, K * 64);
                const int Count = std::clamp((int)RangeInt(E.PerInterval, Rk(KCount)), 0, 64);
                for (int J = 0; J < Count; J++) Spawns.push_back({ K * 64 + J, Delay + K * Interval });
            }
        }
        else
        {
            const Rng R0(I.Seed, Ei, -1);
            const int Count = std::clamp((int)RangeInt(E.OneShotCount, R0(KCount)), 0, 4096);
            for (int J = 0; J < Count; J++)
            {
                const Rng Rj(I.Seed, Ei, J);
                Spawns.push_back({ J, E.DelayMsec.At(Rj(KDelay)) });
            }
        }

        TrailStrip Ribbon;                 // a trail element's particles joined
        // or each one's own path, made only when the ribbon has no length: its particles' spawns
        struct Pending { int64_t Seq; Placement P; float Life; double Age; };
        std::vector<Pending> Paths;
        // a trail spawning every msec (smoke tendrils: 1500 alive) joins at most MaxTrailPoints
        // of its particles, every Nth: its ribbon's shape, a hundredth of the work
        const size_t Every = E.Type == ElemType::Trail ? std::max<size_t>(1, (Spawns.size() + Opt.MaxTrailPoints - 1) / Opt.MaxTrailPoints) : 1;
        for (size_t Si = 0; Si < Spawns.size(); Si++)
        {
            const Spawn& S = Spawns[Si];
            if (Every > 1 && Si % Every != 0 && Si + 1 != Spawns.size()) continue;
            if (Out.Particles.size() + Out.Trails.size() + Paths.size() >= Opt.MaxParticles)
            {
                Out.Capped = true;
                return;
            }
            const double Age = T - S.At;
            if (Age < 0) continue;
            const Rng R(I.Seed, Ei, S.Seq);
            const float Life = std::max(1.0f, RangeInt(E.LifeMsec, R(KLife)));
            if (Age > Life + ChildSpan) continue;

            // its frame: the effect's, or for "offset" the spawn offset's direction
            Placement P;
            P.Frame = Fx;
            for (int K = 0; K < 3; K++) P.Local[K] = E.Origin[K].At(R(KOrigin0 + K));
            if (E.Shaped && E.Offset != SpawnOffset::None)
            {
                // the vfx shapes: a point of the shape, its axes kept by their signs, turned into the effect's frame
                Vec S;
                const float Rad = E.OffsetRadius.At(R(KOffsetRadius));
                if (E.Offset == SpawnOffset::Sphere)
                {
                    const float Z = 2 * R(KOffsetDir0) - 1, A = 2 * Pi * R(KOffsetDir1), Rr = std::sqrt(std::max(0.0f, 1 - Z * Z));
                    S = Mul(Vec{ Rr * std::cos(A), Rr * std::sin(A), Z }, Rad);
                }
                else
                {
                    const float Yaw = 2 * Pi * R(KOffsetYaw);
                    S = { Rad * std::cos(Yaw), Rad * std::sin(Yaw), E.OffsetHeight.At(R(KOffsetHeight)) };
                }
                for (int K = 0; K < 3; K++)
                {
                    const int Keep = (E.AxisFlags >> (2 * K)) & 3;
                    S[K] = Keep == 0 ? 0.0f : Keep == 1 ? std::fabs(S[K]) : Keep == 2 ? -std::fabs(S[K]) : S[K];
                }
                // v + 2 w (q x v) + 2 q x (q x v)
                const Vec Q{ E.ShapeQuat[0], E.ShapeQuat[1], E.ShapeQuat[2] };
                const Vec C1 = Cross(Q, S);
                S = Add(Add(S, Mul(C1, 2 * E.ShapeQuat[3])), Mul(Cross(Q, C1), 2));
                P.Offset = Fx.Dir(S);
            }
            else if (E.Offset == SpawnOffset::Sphere)
            {
                const float Z = 2 * R(KOffsetDir0) - 1, A = 2 * Pi * R(KOffsetDir1), Rr = std::sqrt(std::max(0.0f, 1 - Z * Z));
                P.Offset = Mul(Vec{ Rr * std::cos(A), Rr * std::sin(A), Z }, E.OffsetRadius.At(R(KOffsetRadius)));
            }
            else if (E.Offset == SpawnOffset::Cylinder)
            {
                const float Yaw = 2 * Pi * R(KOffsetYaw), Rad = E.OffsetRadius.At(R(KOffsetRadius));
                P.Offset = Add(Add(Mul(Fx.A[1], Rad * std::cos(Yaw)), Mul(Fx.A[2], Rad * std::sin(Yaw))),
                               Mul(Fx.A[0], E.OffsetHeight.At(R(KOffsetHeight))));
            }
            if (E.Relative == RunRelative::Offset && E.Offset != SpawnOffset::None)
            {
                P.Frame.O = Add(Fx.ToWorld(P.Local), P.Offset);
                P.Frame.A[0] = Normalized(P.Offset, Fx.A[0]);
                const Vec Other = std::fabs(P.Frame.A[0][2]) < 0.9f ? Vec{ 0, 0, 1 } : Vec{ 1, 0, 0 };
                P.Frame.A[1] = Normalized(Cross(Other, P.Frame.A[0]), Fx.A[1]);
                P.Frame.A[2] = Cross(P.Frame.A[0], P.Frame.A[1]);
                P.Local = {};
                P.Offset = {};
            }
            else if (E.Relative == RunRelative::World)
            {
                P.Frame.A[0] = { 1, 0, 0 }, P.Frame.A[1] = { 0, 1, 0 }, P.Frame.A[2] = { 0, 0, 1 };
            }
            const auto Axes = [&](float At, Vec Out3[3]) {
                float Ang[3];
                for (int K = 0; K < 3; K++)
                    Ang[K] = E.Angles[K].At(R(KAngle0 + K)) + At * E.AngularVelocity[K].At(R(KAngVel0 + K));
                AnglesToAxis(Ang, P.Frame, Out3);
            };

            // the effects it starts
            if (Runner && S.At <= T)
            {
                Instance C;
                C.Fx = Runner;
                C.StartMsec = I.StartMsec + S.At;
                C.LoopStopMsec = Life;
                const Vec O = PositionAt(E, R, P, Life, 0);
                Vec Ax[3];
                Axes(0, Ax);
                for (int K = 0; K < 3; K++)
                {
                    C.Origin[K] = O[K];
                    for (int L = 0; L < 3; L++) C.Axis[K][L] = Ax[K][L];
                }
                C.Seed = R.Bits(KChildSeed);
                C.Depth = I.Depth + 1;
                Run(C, Now, Out);
            }
            if (OnDeath && Age >= Life)
            {
                Instance C;
                C.Fx = OnDeath;
                C.StartMsec = I.StartMsec + S.At + Life;
                C.LoopStopMsec = OnDeath->LoopingLifeMsec > 0 && OnDeath->LoopingLifeMsec < INT_MAX ? OnDeath->LoopingLifeMsec : Opt.ChildLoopMsec;
                const Vec O = PositionAt(E, R, P, Life, Life);
                for (int K = 0; K < 3; K++)
                {
                    C.Origin[K] = O[K];
                    for (int L = 0; L < 3; L++) C.Axis[K][L] = I.Axis[K][L];
                }
                C.Seed = R.Bits(KChildSeed + 1);
                C.Depth = I.Depth + 1;
                Run(C, Now, Out);
            }
            if (Emitted)
            {
                // one every emitDist units along its path so far
                const float Every = std::max(0.5f, E.EmitDist.At(R(KEmit)));
                const int Steps = 32;
                const float Until = (float)std::min<double>(Age, Life);
                Vec Prev = PositionAt(E, R, P, Life, 0);
                float Walked = 0, Next = Every;
                int Emits = 0;
                for (int K = 1; K <= Steps && Emits < 64; K++)
                {
                    const float At = Until * K / Steps;
                    const Vec Pos = PositionAt(E, R, P, Life, At);
                    const float Seg = Length(Sub(Pos, Prev));
                    while (Seg > 0 && Walked + Seg >= Next && Emits < 64)
                    {
                        const float F = (Next - Walked) / Seg;
                        const Vec O = Add(Prev, Mul(Sub(Pos, Prev), F));
                        Instance C;
                        C.Fx = Emitted;
                        C.StartMsec = I.StartMsec + S.At + Until * (K - 1 + F) / Steps;
                        C.LoopStopMsec = Opt.ChildLoopMsec;
                        for (int X = 0; X < 3; X++)
                        {
                            C.Origin[X] = O[X];
                            for (int L = 0; L < 3; L++) C.Axis[X][L] = I.Axis[X][L];
                        }
                        C.Seed = R.Bits(KChildSeed + 2 + Emits);
                        C.Depth = I.Depth + 1;
                        if (Now - C.StartMsec < Span(Emitted, I.Depth + 1)) Run(C, Now, Out);
                        Next += Every + E.EmitDistVariance.At(R.Bits(KEmit + Emits) * (1.0f / 4294967296.0f));
                        Emits++;
                    }
                    Walked += Seg;
                    Prev = Pos;
                }
            }

            // the particle itself, while it lives
            if (Age >= Life || E.Type == ElemType::Runner) continue;
            if (E.Fountain)
            {
                if (!Sparks(E, R, I.Seed, Ei, S.Seq, P, Life, (float)Age, Ground, Opt.MaxParticles, Out))
                {
                    Out.Capped = true;
                    return;
                }
                continue;
            }
            Particle Pt;
            Pt.El = &E;
            Pt.Type = E.Type;
            Pt.Seed = R.Bits(KChildSeed + 100);
            if (!E.Materials.empty()) Pt.Mat = &E.Materials[std::min<size_t>(E.Materials.size() - 1, (size_t)(R(KMaterial) * E.Materials.size()))];
            if (!E.Models.empty()) Pt.Model = &E.Models[std::min<size_t>(E.Models.size() - 1, (size_t)(R(KModel) * E.Models.size()))];
            Vec Pos = PositionAt(E, R, P, Life, (float)Age);
            if (E.Collide && Pos[2] < Ground)
            {
                if (E.DieOnTouch) continue;
                Pos[2] = Ground;
            }
            const Vec Before = PositionAt(E, R, P, Life, (float)std::max(0.0, Age - 4.0));
            Vec Dir = Normalized(Sub(Pos, Before), Vec{});
            if (Length(Dir) == 0)
            {
                const Vec After = PositionAt(E, R, P, Life, (float)std::min<double>(Life, Age + 4.0));
                Dir = Normalized(Sub(After, Pos), P.Frame.A[0]);
            }
            for (int K = 0; K < 3; K++)
            {
                Pt.Pos[K] = Pos[K];
                Pt.Dir[K] = Dir[K];
            }
            Vec Ax[3];
            Axes((float)Age, Ax);
            for (int K = 0; K < 3; K++)
                for (int L = 0; L < 3; L++) Pt.Axis[K][L] = Ax[K][L];
            VisualAt(E, R, Life, (float)Age, Pt);
            AtlasAt(E, R, S.Seq, Life, (float)Age, Pt.Uv);
            // an effect whose numbers overflow (an odd file): nothing drawn for it, nothing sorted on a NaN
            if (!Finite(Pt.Pos, 3) || !Finite(Pt.Size, 2) || !Finite(&Pt.Scale, 1) || !std::isfinite(Pt.Rotation)) continue;
            if (E.Type == ElemType::Trail)
            {
                // a point of the element's ribbon (its particles in spawn order, as the games
                // join them), and its own path over the last part of its life for when the
                // ribbon has no length (an effect that does not move)
                TrailStrip::Point Tp{};
                std::copy(Pt.Pos, Pt.Pos + 3, Tp.Pos);
                Tp.Width = Pt.Size[0];
                std::copy(Pt.Color, Pt.Color + 4, Tp.Color);
                if (Ribbon.Points.empty())
                {
                    Ribbon.Mat = Pt.Mat;
                    std::copy(Pt.Uv, Pt.Uv + 4, Ribbon.Uv);
                }
                Ribbon.Points.push_back(Tp);
                Paths.push_back({ S.Seq, P, Life, Age });
                continue;
            }
            Out.Particles.push_back(Pt);
        }
        if (E.Type == ElemType::Trail)
        {
            float Length_ = 0;
            for (size_t K = 1; K < Ribbon.Points.size(); K++)
            {
                const auto& A = Ribbon.Points[K - 1].Pos;
                const auto& B = Ribbon.Points[K].Pos;
                Length_ += Length(Vec{ B[0] - A[0], B[1] - A[1], B[2] - A[2] });
            }
            if (Ribbon.Points.size() >= 2 && Length_ > 1.0f)
            {
                TrailTexture(E, T, Ribbon);
                Out.Trails.push_back(std::move(Ribbon));
            }
            else
                for (const Pending& Pe : Paths)
                {
                    const Rng R(I.Seed, Ei, Pe.Seq);
                    TrailStrip Own;
                    AtlasAt(E, R, Pe.Seq, Pe.Life, (float)Pe.Age, Own.Uv);
                    if (!E.Materials.empty())
                        Own.Mat = &E.Materials[std::min<size_t>(E.Materials.size() - 1, (size_t)(R(KMaterial) * E.Materials.size()))];
                    const float Span_ = E.TrailLength > 0 ? E.TrailLength : Pe.Life * 0.5f;
                    const float From = (float)std::max(0.0, Pe.Age - Span_);
                    const int Points = 16;
                    for (int K = 0; K <= Points; K++)
                    {
                        const float At = From + ((float)Pe.Age - From) * K / Points;
                        Particle Q;
                        VisualAt(E, R, Pe.Life, At, Q);
                        const Vec Q3 = PositionAt(E, R, Pe.P, Pe.Life, At);
                        TrailStrip::Point Op{};
                        for (int X = 0; X < 3; X++) Op.Pos[X] = Q3[X];
                        Op.Width = Q.Size[0];
                        std::copy(Q.Color, Q.Color + 4, Op.Color);
                        Op.V = (float)K / Points;
                        if (Finite(Op.Pos, 3) && Finite(&Op.Width, 1)) Own.Points.push_back(Op);
                    }
                    TrailTexture(E, T, Own);
                    Out.Trails.push_back(std::move(Own));
                }
        }
    }
}
}
