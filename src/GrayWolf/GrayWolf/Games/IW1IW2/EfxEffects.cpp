#include "stdafx.h"

#include "EfxEffects.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace IW12::Efx
{
namespace
{
    using Json = nlohmann::ordered_json;

    constexpr float Pi = 3.14159265358979f;
    constexpr int CurveIntervals = 16;      // a curve's graphs sampled at 17 points over the life
    constexpr float WorldGravity = 800.0f;  // the preview's gravity unit

    std::string Lower(std::string S)
    {
        for (char& C : S) C = (char)std::tolower((unsigned char)C);
        return S;
    }
    std::string Trim(const std::string& S)
    {
        const size_t B = S.find_first_not_of(" \t\r"), E = S.find_last_not_of(" \t\r");
        return B == std::string::npos ? std::string() : S.substr(B, E - B + 1);
    }
    std::vector<std::string> Words(const std::string& S)
    {
        std::istringstream In(S);
        std::vector<std::string> Out;
        for (std::string W; In >> W;) Out.push_back(W);
        return Out;
    }
    std::vector<float> Numbers(const std::string& S)
    {
        std::vector<float> Out;
        for (const auto& W : Words(S))
        {
            char* End = nullptr;
            const float F = std::strtof(W.c_str(), &End);
            if (End == W.c_str()) break;
            Out.push_back(std::isfinite(F) ? F : 0.0f);
        }
        return Out;
    }

    // A graph block: a curve (CoD2: keys of t and Dims values) times its scale, or start and end
    // (each Dims values, or 2 x Dims: a range)
    struct Graph
    {
        std::vector<float> Curve;
        bool HasCurve = false;
        std::vector<float> Scale, Start, End;
    };
    struct Element
    {
        std::string Type;
        std::vector<std::pair<std::string, std::string>> Values;   // in file order
        std::map<std::string, Graph> Graphs;
        std::map<std::string, std::vector<std::string>> Lists;
        const std::string* Value(const char* Key) const
        {
            for (const auto& [K, V] : Values)
                if (K == Key) return &V;
            return nullptr;
        }
        const Graph* GraphOf(const std::string& Key) const
        {
            const auto It = Graphs.find(Key);
            return It == Graphs.end() ? nullptr : &It->second;
        }
    };

    // the file's lines, trimmed, empty ones dropped
    struct Lines
    {
        std::vector<std::string> L;
        size_t At = 0;
        bool Done() const { return At >= L.size(); }
        const std::string& Peek() const { return L[At]; }
    };

    // a graph's body after its "{", to its "}" (a block within skipped)
    bool ParseGraph(Lines& In, Graph& G)
    {
        int Depth = 1;
        while (!In.Done())
        {
            const std::string Line = In.L[In.At++];
            if (Line == "{") { Depth++; continue; }
            if (Line == "}")
            {
                if (--Depth == 0) return true;
                continue;
            }
            if (Depth > 1) continue;
            const auto W = Words(Line);
            const std::string Key = W.empty() ? std::string() : W[0];
            const std::string Rest = Trim(Line.substr(Key.size()));
            if (Key == "curve" && !In.Done() && In.Peek() == "[")
            {
                In.At++;
                G.HasCurve = true;
                while (!In.Done() && In.Peek() != "]")
                    for (float F : Numbers(In.L[In.At++])) G.Curve.push_back(F);
                if (!In.Done()) In.At++;
            }
            else if (Key == "scale") G.Scale = Numbers(Rest);
            else if (Key == "start") G.Start = Numbers(Rest);
            else if (Key == "end") G.End = Numbers(Rest);
        }
        return false;
    }

    bool Parse(const std::string& Text, std::vector<Element>& Out, std::string& Error)
    {
        Lines In;
        std::istringstream S(Text);
        for (std::string Line; std::getline(S, Line);)
            if (std::string T = Trim(Line); !T.empty())
            {
                // a brace line can carry a stray byte after it (panzerschreck_flash's "}\xf6")
                if (T[0] == '{' || T[0] == '}' || T[0] == '[' || T[0] == ']') T.resize(1);
                In.L.push_back(std::move(T));
            }
        while (!In.Done())
        {
            Element E;
            E.Type = In.L[In.At++];
            if (In.Done() || In.Peek() != "{")
            {
                Error = "element " + E.Type + " without its block";
                return false;
            }
            In.At++;
            bool Closed = false;
            while (!In.Done())
            {
                const std::string Line = In.L[In.At++];
                if (Line == "}")
                {
                    Closed = true;
                    break;
                }
                const auto W = Words(Line);
                const std::string Key = W.empty() ? std::string() : W[0];
                const std::string Rest = Trim(Line.substr(Key.size()));
                if (!In.Done() && In.Peek() == "{")
                {
                    In.At++;
                    if (!ParseGraph(In, E.Graphs[Key]))
                    {
                        Error = "graph " + Key + " not closed";
                        return false;
                    }
                }
                else if (!In.Done() && In.Peek() == "[")
                {
                    In.At++;
                    auto& List = E.Lists[Key];
                    while (!In.Done() && In.Peek() != "]") List.push_back(In.L[In.At++]);
                    if (In.Done())
                    {
                        Error = "list " + Key + " not closed";
                        return false;
                    }
                    In.At++;
                }
                else E.Values.emplace_back(Key, Rest);
            }
            if (!Closed)
            {
                Error = "element " + E.Type + " not closed";
                return false;
            }
            Out.push_back(std::move(E));
        }
        return true;
    }

    struct Span
    {
        float Lo = 0, Hi = 0;
    };
    // a value line of Dims numbers or 2 x Dims (min then max), Default when missing
    std::vector<Span> SpansOf(const std::vector<float>& N, size_t Dims, float Default)
    {
        std::vector<Span> Out(Dims, Span{ Default, Default });
        if (N.size() >= 2 * Dims)
            for (size_t K = 0; K < Dims; K++) Out[K] = { N[K], N[Dims + K] };
        else if (N.size() >= Dims)
            for (size_t K = 0; K < Dims; K++) Out[K] = { N[K], N[K] };
        return Out;
    }
    std::vector<Span> ValueSpans(const Element& E, const char* Key, size_t Dims, float Default)
    {
        const std::string* V = E.Value(Key);
        return SpansOf(V ? Numbers(*V) : std::vector<float>(), Dims, Default);
    }

    // a curve's keys: t then Dims values each, while t keeps within 0..1 and does not go back (a
    // scalar curve ends with a pair that is not a key: 2.755e-040 2.572e-038)
    std::vector<std::vector<float>> Keys(const std::vector<float>& C, size_t Dims)
    {
        std::vector<std::vector<float>> Out;
        for (size_t I = 0; I + Dims + 1 <= C.size(); I += Dims + 1)
        {
            const float T = C[I];
            if (T < -1e-4f || T > 1.0001f || (!Out.empty() && T < Out.back()[0] - 1e-5f)) break;
            if (Out.empty() && T > 1e-4f) break;
            // lines are segments (t0 v0 t1 v1): each inner key twice
            std::vector<float> K(C.begin() + I, C.begin() + I + Dims + 1);
            if (Out.empty() || K != Out.back()) Out.push_back(std::move(K));
        }
        return Out;
    }
    float CurveAt(const std::vector<std::vector<float>>& K, size_t Dim, float T)
    {
        if (K.empty()) return 0;
        if (T <= K.front()[0]) return K.front()[1 + Dim];
        for (size_t I = 1; I < K.size(); I++)
            if (T <= K[I][0])
            {
                const float T0 = K[I - 1][0], T1 = K[I][0];
                const float S = T1 - T0 > 1e-6f ? (T - T0) / (T1 - T0) : 1.0f;
                return K[I - 1][1 + Dim] + (K[I][1 + Dim] - K[I - 1][1 + Dim]) * S;
            }
        return K.back()[1 + Dim];
    }

    // A graph and its random twin (sizeRand...) at T: per value its span. The twin is drawn with the
    // graph's scale; Twin null: the graph alone.
    std::vector<Span> GraphAt(const Graph* G, const Graph* Twin, size_t Dims, float T, float Default)
    {
        std::vector<Span> Out(Dims, Span{ Default, Default });
        if (G == nullptr) return Out;
        if (G->HasCurve)
        {
            const float S0 = G->Scale.empty() ? 1.0f : G->Scale[0], S1 = G->Scale.size() > 1 ? G->Scale[1] : S0;
            const auto Of = [&](const Graph& C, size_t D) {
                const auto K = Keys(C.Curve, Dims);
                return K.empty() ? Default : CurveAt(K, D, T);
            };
            for (size_t D = 0; D < Dims; D++)
            {
                std::vector<float> V{ Of(*G, D) * S0, Of(*G, D) * S1 };
                if (Twin != nullptr && Twin->HasCurve) V.push_back(Of(*Twin, D) * S0), V.push_back(Of(*Twin, D) * S1);
                Out[D] = { *std::min_element(V.begin(), V.end()), *std::max_element(V.begin(), V.end()) };
            }
            return Out;
        }
        if (G->Start.empty() && G->End.empty()) return Out;
        // no start: the default (grenade_flash's alpha { end 0 } fades from 1); no end: the start
        const auto A = SpansOf(G->Start, Dims, Default);
        const auto B = SpansOf(G->End.empty() ? G->Start : G->End, Dims, Default);
        for (size_t D = 0; D < Dims; D++)
            Out[D] = { A[D].Lo + (B[D].Lo - A[D].Lo) * T, A[D].Hi + (B[D].Hi - A[D].Hi) * T };
        return Out;
    }

    Json RangeJson(const Span& S) { return Json::array({ S.Lo, S.Hi - S.Lo }); }
    Json RangeJson(float Lo, float Hi) { return Json::array({ Lo, Hi - Lo }); }

    const char* TypeOf(const std::string& Word, bool Model)
    {
        if (Model) return "model";
        static const std::pair<const char*, const char*> Types[] = {
            { "Particle", "billboardSprite" }, { "OrientedParticle", "orientedSprite" }, { "Tail", "tail" }, { "Line", "line" },
            { "Cloud", "cloud" }, { "Decal", "decal" }, { "Light", "omniLight" }, { "Sound", "sound" }, { "FxRunner", "runner" },
            { "Emitter", "emitter" }, { "Cylinder", "cylinder" }, { "CameraShake", "cameraShake" } };
        for (const auto& [W, T] : Types)
            if (Word == W) return T;
        return nullptr;
    }

    Json ElementJson(const Element& Src, size_t Index, const MaterialFields& Material)
    {
        // a graph written as one line (CoD1's "length 20 40"): constant
        Element E = Src;
        for (const char* Key : { "rgb", "alpha", "size", "size2", "length" })
            if (const std::string* V = E.Value(Key); V && !E.GraphOf(Key)) E.Graphs[Key].Start = Numbers(*V);
        std::set<std::string> Flags;
        for (const char* Key : { "flags", "spawnFlags" })
            if (const std::string* V = E.Value(Key))
                for (const auto& W : Words(*V)) Flags.insert(W);
        const auto Has = [&](const char* F) { return Flags.count(F) != 0; };
        const bool Model = Has("useModel") && E.Lists.count("models");
        const char* Type = TypeOf(E.Type, Model);
        const std::string TypeName = Type ? Type : Lower(E.Type);
        const bool Long = TypeName == "tail" || TypeName == "line";
        const bool Cylinder = TypeName == "cylinder";

        Json J;
        J["group"] = "oneShot";
        J["index"] = Index;
        J["type"] = TypeName;
        J["efxType"] = E.Type;
        if (const std::string* N = E.Value("name")) J["name"] = *N;

        // the flags the preview reads, then the file's own words
        Json Out = Json::array();
        const std::string* NonUniform = E.Value("nonUniformScale");
        if (Long || Cylinder || (NonUniform && std::atoi(NonUniform->c_str()) != 0)) Out.push_back("nonUniformScale");
        if (Has("usePhysics")) Out.push_back("useCollision");
        if (Has("impactKills")) Out.push_back("dieOnTouch");

        const bool Sphere = Has("orgOnSphere"), OnCylinder = Has("orgOnCylinder");
        J["spawnOffset"] = Sphere ? "sphere" : OnCylinder ? "cylinder" : "none";
        // axisFromSphere: moves along the offset's direction; relative: with the effect
        J["runRelativeTo"] = Has("axisFromSphere") && (Sphere || OnCylinder) ? "offset" : Has("relative") ? "effect" : "spawn";

        const auto Count = ValueSpans(E, "count", 1, 1)[0];
        const auto Life = ValueSpans(E, "life", 1, 1000)[0];
        J["spawn"] = RangeJson(Count);
        J["spawnDelayMsec"] = RangeJson(ValueSpans(E, "delay", 1, 0)[0]);
        J["lifeSpanMsec"] = RangeJson(Life);
        Json Origin = Json::array();
        for (const auto& S : ValueSpans(E, "origin", 3, 0)) Origin.push_back(RangeJson(S));
        J["spawnOrigin"] = std::move(Origin);
        J["spawnOffsetRadius"] = RangeJson(ValueSpans(E, "radius", 1, 0)[0]);
        J["spawnOffsetHeight"] = RangeJson(ValueSpans(E, "height", 1, 0)[0]);
        // angles in degrees (min x y z, max x y z); their change per second
        Json Angles = Json::array(), AngVel = Json::array();
        for (const auto& S : ValueSpans(E, "angle", 3, 0)) Angles.push_back(RangeJson(S.Lo * Pi / 180, S.Hi * Pi / 180));
        for (const auto& S : ValueSpans(E, "angleDelta", 3, 0))
            AngVel.push_back(RangeJson(S.Lo * Pi / 180000, S.Hi * Pi / 180000));
        J["spawnAngles"] = std::move(Angles);
        J["angularVelocity"] = std::move(AngVel);
        const auto Rot = ValueSpans(E, "rotation", 1, 0)[0];
        J["initialRotation"] = RangeJson(Rot.Lo * Pi / 180, Rot.Hi * Pi / 180);
        const auto G = ValueSpans(E, "gravity", 1, 0)[0];
        J["gravity"] = RangeJson(-G.Lo / WorldGravity, -G.Hi / WorldGravity);

        // straight graphs (start/end, two-key curves; the velocity from a constant acceleration) are
        // exact with one interval; a curve of more keys sampled
        int Intervals = 1;
        for (const auto& [Key, Gr] : E.Graphs)
            if (Gr.HasCurve && Keys(Gr.Curve, Key == "rgb" ? 3 : 1).size() > 2) Intervals = CurveIntervals;

        // velocity: CoD2's curves (velocityX..., velocity2X...: the second set world-relative with
        // absoluteVel2), CoD1's constant velocity and acceleration (world with absoluteVel,
        // absoluteAccel); units per second, the acceleration over the mean life
        const float MeanLife = std::max(1.0f, (Life.Lo + Life.Hi) / 2);
        struct Vel
        {
            Span V[2][3];   // [local, world][axis]
        };
        std::vector<Vel> Vels(Intervals + 1);
        bool Moves[2] = { false, false };
        const auto AddSpan = [&](Span& To, const Span& S, int Frame) {
            To.Lo += S.Lo, To.Hi += S.Hi;
            if (S.Lo != 0 || S.Hi != 0) Moves[Frame] = true;
        };
        const auto Velocity0 = ValueSpans(E, "velocity", 3, 0), Accel = ValueSpans(E, "acceleration", 3, 0);
        for (int I = 0; I <= Intervals; I++)
        {
            const float T = (float)I / Intervals, Sec = T * MeanLife / 1000;
            const char* Axes[] = { "X", "Y", "Z" };
            for (int K = 0; K < 3; K++)
            {
                for (int Set = 0; Set < 2; Set++)
                {
                    const std::string Key = std::string(Set == 0 ? "velocity" : "velocity2") + Axes[K];
                    const bool Random = Has(Set == 0 ? "useRandomVelocity" : "useRandomVelocity2");
                    const Graph* Gr = E.GraphOf(Key);
                    if (!Gr) continue;
                    const int Frame = Has(Set == 0 ? "absoluteVel" : "absoluteVel2") ? 1 : 0;
                    AddSpan(Vels[I].V[Frame][K], GraphAt(Gr, Random ? E.GraphOf(Key + "Rand") : nullptr, 1, T, 0)[0], Frame);
                }
                AddSpan(Vels[I].V[Has("absoluteVel") ? 1 : 0][K], Velocity0[K], Has("absoluteVel") ? 1 : 0);
                AddSpan(Vels[I].V[Has("absoluteAccel") ? 1 : 0][K], Span{ Accel[K].Lo * Sec, Accel[K].Hi * Sec },
                        Has("absoluteAccel") ? 1 : 0);
            }
        }
        if (Moves[0]) Out.push_back("hasVelocityGraphLocal");
        if (Moves[1]) Out.push_back("hasVelocityGraphWorld");
        // the preview's samples: per interval, the velocity over the life in msec (units per msec per
        // interval) and the distance covered before it (trapezoids)
        const float PerSample = 1.0f / (1000.0f * Intervals);
        Json VelSamples = Json::array();
        float Total[2][3][2] = {};
        for (int I = 0; I <= Intervals; I++)
        {
            Json S;
            for (int F = 0; F < 2; F++)
            {
                Json Base = Json::array(), Amp = Json::array(), TB = Json::array(), TA = Json::array();
                for (int K = 0; K < 3; K++)
                {
                    const Span& V = Vels[I].V[F][K];
                    Base.push_back(V.Lo * PerSample);
                    Amp.push_back((V.Hi - V.Lo) * PerSample);
                    TB.push_back(Total[F][K][0]);
                    TA.push_back(Total[F][K][1]);
                    if (I < Intervals)
                    {
                        const Span& N = Vels[I + 1].V[F][K];
                        Total[F][K][0] += 0.5f * (V.Lo + N.Lo) * PerSample;
                        Total[F][K][1] += 0.5f * ((V.Hi - V.Lo) + (N.Hi - N.Lo)) * PerSample;
                    }
                }
                S[F == 0 ? "local" : "world"] = Json{ { "velocity", { { "base", Base }, { "amplitude", Amp } } },
                                                      { "totalDelta", { { "base", TB }, { "amplitude", TA } } } };
            }
            VelSamples.push_back(std::move(S));
        }

        // visual samples: colour (rgb, alpha and their random twins), size (a tail's and a line's
        // second size its length, a sprite's size2), rotation per second (radians)
        const auto Twin = [&](const char* Key, const char* Flag) { return Has(Flag) ? E.GraphOf(std::string(Key) + "Rand") : nullptr; };
        const Graph* Size2 = Long ? E.GraphOf("length") : E.GraphOf("size2") ? E.GraphOf("size2") : E.GraphOf("size");
        const Graph* Size2Twin = Long ? Twin("length", "useRandomLength") : E.GraphOf("size2") ? Twin("size2", "useRandomSize") : Twin("size", "useRandomSize");
        const std::string* RotDeltaValue = E.Value("rotationDelta");
        const Span RotDeltaConst = SpansOf(RotDeltaValue ? Numbers(*RotDeltaValue) : std::vector<float>(), 1, 0)[0];
        Json VisSamples = Json::array();
        float RotTotal[2] = {};
        Span PrevRot{};
        for (int I = 0; I <= Intervals; I++)
        {
            const float T = (float)I / Intervals;
            const auto Rgb = GraphAt(E.GraphOf("rgb"), nullptr, 3, T, 1);
            const auto RgbB = Has("useRandomColors") && E.GraphOf("rgbRand") ? GraphAt(E.GraphOf("rgbRand"), nullptr, 3, T, 1) : Rgb;
            const auto A = GraphAt(E.GraphOf("alpha"), nullptr, 1, T, 1)[0];
            const auto AB = Has("useRandomAlpha") && E.GraphOf("alphaRand") ? GraphAt(E.GraphOf("alphaRand"), nullptr, 1, T, 1)[0] : A;
            const auto S0 = GraphAt(E.GraphOf("size"), Twin("size", "useRandomSize"), 1, T, 1)[0];
            const auto S1 = GraphAt(Size2, Size2Twin, 1, T, 1)[0];
            // a cylinder's height: its length graph, as its scale
            const auto Height = Cylinder ? GraphAt(E.GraphOf("length"), Twin("length", "useRandomLength"), 1, T, 0)[0] : Span{ 1, 1 };
            Span Rot = RotDeltaValue ? RotDeltaConst : GraphAt(E.GraphOf("rotationDelta"), Twin("rotationDelta", "useRandomRotationDelta"), 1, T, 0)[0];
            Rot.Lo *= PerSample, Rot.Hi *= PerSample;
            if (I > 0)
            {
                RotTotal[0] += 0.5f * (PrevRot.Lo + Rot.Lo);
                RotTotal[1] += 0.5f * ((PrevRot.Hi - PrevRot.Lo) + (Rot.Hi - Rot.Lo));
            }
            PrevRot = Rot;
            const auto Byte = [](float V) { return (int)std::lround(std::clamp(V, 0.0f, 1.0f) * 255); };
            // a span's two ends: base then amplitude, colours each a colour
            const auto Part = [&](bool High) {
                const auto Pick = [&](const Span& S) { return High ? S.Hi : S.Lo; };
                Json C = Json::array({ Byte(Pick(High ? RgbB[0] : Rgb[0])), Byte(Pick(High ? RgbB[1] : Rgb[1])),
                                       Byte(Pick(High ? RgbB[2] : Rgb[2])), Byte(Pick(High ? AB : A)) });
                const auto Amp = [&](const Span& S) { return High ? S.Hi - S.Lo : S.Lo; };
                return Json{ { "color", C }, { "rotationDelta", Amp(Rot) }, { "rotationTotal", High ? RotTotal[1] : RotTotal[0] },
                             { "size", Json::array({ Amp(S0), Amp(S1) }) }, { "scale", Amp(Height) } };
            };
            VisSamples.push_back(Json{ { "base", Part(false) }, { "amplitude", Part(true) } });
        }

        // what it draws, plays or starts
        Json Visuals = Json::array();
        const auto ListOf = [&](const char* Key) {
            const auto It = E.Lists.find(Key);
            return It == E.Lists.end() ? std::vector<std::string>() : It->second;
        };
        if (TypeName == "runner")
            for (const auto& N : ListOf("playfx")) Visuals.push_back(Json{ { "effect", EffectName(N) } });
        else if (TypeName == "sound")
            for (const auto& N : ListOf("sounds")) Visuals.push_back(Json{ { "sound", N } });
        else if (Model)
            for (auto N : ListOf("models"))
            {
                N = Lower(N);
                if (N.rfind("xmodel/", 0) == 0) N.erase(0, 7);
                Visuals.push_back(Json{ { "model", N } });
            }
        else
            for (const auto& N : ListOf("shaders"))
            {
                Json V{ { "material", N } };
                if (Material) Material(N, V);
                Visuals.push_back(std::move(V));
            }
        // an atlas (CoD2: its material's grid), played by the sequence fields: start fixed (at
        // sequenceFixedFrameValue), random or by particle (0, 1, 2), over the life (play rate 1)
        // or at sequenceFixedFpsValue, looping or sequenceLoopTimes times (loop mode 1)
        int Rows = 1, Cols = 1;
        for (const auto& V : Visuals)
            if (const auto At = V.find("atlas"); At != V.end() && At->is_array() && At->size() == 2)
            {
                Rows = (*At)[0].get<int>(), Cols = (*At)[1].get<int>();
                break;
            }
        const auto IntOf = [&](const char* Key, int Default) {
            const std::string* V = E.Value(Key);
            return V ? (int)std::lround(std::strtod(V->c_str(), nullptr)) : Default;
        };
        const auto Bits = [](int N) {
            int B = 0;
            while ((1 << B) < N && B < 8) B++;
            return B;
        };
        const int Behavior = std::clamp(IntOf("sequenceStartFrameMode", 0), 0, 2) | (IntOf("sequencePlayRateMode", 0) == 1 ? 4 : 0) |
                             (IntOf("sequenceLoopMode", 0) == 1 ? 8 : 0);
        J["atlas"] = Json{ { "behavior", Behavior }, { "index", IntOf("sequenceFixedFrameValue", 0) },
                           { "fps", IntOf("sequenceFixedFpsValue", 0) }, { "loopCount", IntOf("sequenceLoopTimes", 0) },
                           { "colIndexBits", Bits(Cols) }, { "rowIndexBits", Bits(Rows) }, { "entryCount", Rows * Cols } };

        // the effects it starts: emitted along its path (an emitter: density per 100 units), on
        // its death, on impact
        const auto First = [&](const char* Key) {
            const auto L = ListOf(Key);
            return L.empty() ? std::string() : EffectName(L[0]);
        };
        J["fxEmitted"] = Has("emitFx") || TypeName == "emitter" ? First("emitfx") : std::string();
        J["fxOnDeath"] = Has("deathFx") ? First("deathfx") : std::string();
        J["fxOnImpact"] = Has("impactFx") ? First("impactfx") : std::string();
        const float Density = ValueSpans(E, "density", 1, 0)[0].Lo;
        J["emitDist"] = RangeJson(Density > 0 ? 100 / Density : 25.0f, Density > 0 ? 100 / Density : 25.0f);
        J["emitDistVariance"] = RangeJson(0, 0);
        for (const auto& F : Flags) Out.push_back(F);
        J["flags"] = std::move(Out);
        J["velocitySamples"] = std::move(VelSamples);
        J["visualSamples"] = std::move(VisSamples);
        J["visuals"] = std::move(Visuals);
        // the file's lines as written
        Json Fields = Json::object();
        for (const auto& [K, V] : E.Values) Fields[K] = V;
        J["efx"] = std::move(Fields);
        return J;
    }
}

std::string EffectName(const std::string& Ref)
{
    std::string N = Lower(Trim(Ref));
    for (char& C : N)
        if (C == '\\') C = '/';
    while (!N.empty() && N[0] == '/') N.erase(0, 1);
    if (N.rfind("fx/", 0) == 0) N.erase(0, 3);
    if (N.size() > 4 && N.compare(N.size() - 4, 4, ".efx") == 0) N.erase(N.size() - 4);
    return N;
}

bool ToJson(const std::string& Text, const std::string& Name, const std::string& Game, const MaterialFields& Material, std::string& Out,
            std::string& Error)
{
    std::vector<Element> Elements;
    if (!Parse(Text, Elements, Error)) return false;
    Json J;
    J["name"] = Name;
    J["game"] = Game;
    J["format"] = "efx";
    J["flags"] = 0;
    J["loopingLifeMsec"] = 0;
    Json List = Json::array();
    int Longest = 0;
    for (size_t I = 0; I < Elements.size(); I++)
    {
        List.push_back(ElementJson(Elements[I], I, Material));
        const auto& E = Elements[I];
        Longest = std::max(Longest, (int)(ValueSpans(E, "delay", 1, 0)[0].Hi + ValueSpans(E, "life", 1, 1000)[0].Hi));
    }
    J["nonLoopingLifeMsec"] = Longest;
    J["counts"] = Json{ { "looping", 0 }, { "oneShot", Elements.size() }, { "emission", 0 } };
    J["notes"] = "Read from the game's .efx text. Units the file leaves open, assumed: velocity in units per second, "
                 "rotation and angle changes in radians and degrees per second, gravity in units/s^2 (negative pulls "
                 "down) written as a fraction of 800, an emitter's density as effects per 100 units travelled.";
    J["elements"] = std::move(List);
    Out = J.dump(1, '\t', false, Json::error_handler_t::replace) + "\n";
    return true;
}
}
