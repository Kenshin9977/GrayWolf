#include "stdafx.h"

#include "FxPreviewModel.h"

#include "Games/IW/Vfx.h"
#include "Strings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <climits>
#include <filesystem>
#include <fstream>
#include <functional>

namespace fs = std::filesystem;

namespace FxPreview
{
namespace
{
    using Json = nlohmann::json;

    std::string Lower(std::string S)
    {
        for (char& C : S) C = (char)std::tolower((unsigned char)C);
        return S;
    }
    bool Has(const std::string& S, const char* Part) { return S.find(Part) != std::string::npos; }

    // paths as UTF-8 text
    fs::path PathOf(const std::string& S)
    {
        try { return fs::path(Strings::ToUnicodeString(S)); }
        catch (const std::exception&) { return fs::path(S); }
    }
    std::string Text(const fs::path& P) { return Strings::ToNormalString(P.wstring()); }

    // a field, null when it is missing (a const json's [] on a missing key is undefined) or the
    // value is not an object / array: the writers' JSON read whatever it lacks
    const Json& Get(const Json& J, const char* Key)
    {
        static const Json None;
        if (!J.is_object()) return None;
        const auto It = J.find(Key);
        return It == J.end() ? None : *It;
    }
    const Json& Get(const Json& J, int I)
    {
        static const Json None;
        return J.is_array() && I >= 0 && I < (int)J.size() ? J[(size_t)I] : None;
    }
    // a number, 0 when it is something else
    float Num(const Json& J) { return J.is_number() ? J.get<float>() : 0.0f; }
    int Int(const Json& J) { return J.is_number() ? (int)std::clamp(J.get<double>(), (double)INT_MIN, (double)INT_MAX) : 0; }

    Range RangeOf(const Json& J)
    {
        Range R;
        if (J.is_array() && J.size() >= 2 && J[0].is_number() && J[1].is_number())
        {
            R.Base = Num(J[0]);
            R.Amp = Num(J[1]);
        }
        return R;
    }
    std::string Str(const Json& J, const char* Key)
    {
        const auto It = J.find(Key);
        return It != J.end() && It->is_string() ? It->get<std::string>() : std::string();
    }
    bool HasFlag(const Json& E, const char* Name)
    {
        const auto It = E.find("flags");
        if (It == E.end() || !It->is_array()) return false;
        for (const auto& F : *It)
            if (F.is_string() && F.get<std::string>() == Name) return true;
        return false;
    }

    ElemType TypeOf(const std::string& T)
    {
        static const std::pair<const char*, ElemType> Types[] = {
            { "billboardSprite", ElemType::Billboard }, { "orientedSprite", ElemType::Oriented },
            { "rotatedSprite", ElemType::Rotated }, { "tail", ElemType::Tail }, { "line", ElemType::Line },
            { "trail", ElemType::Trail }, { "cloud", ElemType::Cloud }, { "model", ElemType::Model },
            { "omniLight", ElemType::Light }, { "dynamicLight", ElemType::Light }, { "spotLight", ElemType::SpotLight },
            { "sound", ElemType::Sound }, { "dynamicSound", ElemType::Sound }, { "decal", ElemType::Decal },
            { "lensFlare", ElemType::LensFlare }, { "beamSource", ElemType::Beam },
            { "runner", ElemType::Runner }, { "cylinder", ElemType::Cylinder },
            // the Infinity Ward games' (IwEffects.h, and World at War's, MW2's, MW3's): a flare is their
            // lens flare, a spark cloud a cloud, a spark fountain's sparks stretched along their velocity
            { "flare", ElemType::LensFlare }, { "sparkCloud", ElemType::Cloud }, { "sparkFountain", ElemType::Tail },
        };
        for (const auto& [N, Ty] : Types)
            if (T == N) return Ty;
        return ElemType::Other;
    }

    // How a material blends, from its techset when the export names it, else from its name's
    // suffixes (the games' naming: _add additive, _em emissive, _lit lit)
    void Classify(Material& M)
    {
        const std::string T = Lower(M.Techset);
        std::string N = Lower(M.Name);
        if (const size_t Slash = N.find_last_of("/\\"); Slash != std::string::npos) N = N.substr(Slash + 1);
        const auto Token = [&](const char* Tok) {
            const std::string S = std::string("_") + Tok;
            for (size_t P = N.find(S); P != std::string::npos; P = N.find(S, P + 1))
            {
                const size_t E = P + S.size();
                if (E == N.size() || N[E] == '_' || std::isdigit((unsigned char)N[E])) return true;
            }
            return false;
        };
        M.Lit = Has(T, "lit") ? true : Token("lit");
        M.Distortion = Has(T, "distort") || Has(N, "distort");
        M.LightSprite = Has(T, "spritelight");
        // the game's own blend factors first (GFXS_BLEND_*: 1 zero, 2 one, 5 src alpha, 6 inv src alpha)
        if (M.BlendSrc >= 0)
        {
            const int S = M.BlendSrc, D = M.BlendDst;
            if (S == 0 || (S == 2 && D == 1)) M.Mode = Blend::Opaque;
            else if (D == 2) M.Mode = Blend::Additive;
            else if (S == 2 && D == 6) M.Mode = Blend::Premultiplied;
            else M.Mode = Blend::Alpha;
            return;
        }
        if (!T.empty())
        {
            // MW2019 and MWIII's effect techsets are "effect_bad" (blend add) on opaque-alpha
            // textures: drawn additive
            if (Has(T, "add") || Has(T, "screen") || Has(T, "_bad_")) M.Mode = Blend::Additive;
            else if (Has(T, "emissive")) M.Mode = Blend::Premultiplied;
            else if (Has(T, "replace") || Has(T, "opaque")) M.Mode = Blend::Opaque;
            else M.Mode = Blend::Alpha;
            if (Has(T, "emissive") && !Has(T, "lit_emissive")) M.Lit = false;
            return;
        }
        // an emissive map alone (Black Ops 6/7's fire, sparks): added
        if (M.Emissive)
        {
            M.Mode = Blend::Additive;
            return;
        }
        if (Token("add") || Token("sadd") || Token("screen")) M.Mode = Blend::Additive;
        else if (Token("em")) M.Mode = Blend::Premultiplied;
        else M.Mode = Blend::Alpha;
    }

    // a material's images from its slots: the colour map first (diffuse, then emissive, then any
    // image not a known other kind); an opacity, transmittance or ramp image to its own field
    void SelectImages(Material& M)
    {
        M.Images.clear();
        M.AlphaMask.clear();
        M.Opacity.clear(), M.Ramp.clear(), M.OpacityInverted = M.Emissive = false;
        // Black Ops 4's mask (semantic 0x34614347: *_alpha, *_mask, $white_reveal beside the
        // colour map of every effect material that has two images)
        constexpr uint32_t Bo4Mask = 0x34614347;
        for (const auto& S : M.Slots)
            if (S.Semantic == Bo4Mask && !S.Name.empty() && S.Name[0] != '$') M.AlphaMask = S.Name;
        for (int Pass = 0; Pass < 3; Pass++)
            for (const auto& S : M.Slots)
            {
                const std::string& U = S.Usage;
                if (S.Name.empty() || S.Name[0] == '$' || S.Name == M.AlphaMask) continue;
                if (Pass == 0 && (U == "opacity" || U == "transmittance"))
                    M.Opacity = S.Name, M.OpacityInverted = U == "transmittance";
                else if (Pass == 0 && U == "ramp")
                    M.Ramp = S.Name;
                if (U == "opacity" || U == "transmittance" || U == "ramp" || U == "normal" || U == "specular") continue;
                if (Pass == (U == "diffuse" ? 0 : U == "emissive" ? 1 : 2))
                {
                    if (Pass == 1 && M.Images.empty()) M.Emissive = true;
                    M.Images.push_back(S.Name);
                }
            }
    }

    // ---- the Treyarch adapter: Black Ops, Black Ops II, Black Ops III (and Cold War's types and
    // visuals, without timings)
    // a material's techset, images and blend factors as the fx kind's visuals and the material
    // kind's files write them
    void ReadMaterialFields(const Json& V, Material& M)
    {
        M.Techset = Str(V, "techset");
        if (const auto B = V.find("blend"); B != V.end() && B->is_array() && B->size() == 2)
        {
            M.BlendSrc = (int)Num((*B)[0]);
            M.BlendDst = (int)Num((*B)[1]);
        }
        // Black Ops III's emissive effect techsets (ei/effect_lit_emissive_blend...): hdrScale, the
        // float at byte 184 of technique 6's static pixel cbuffer (its pixel shader's $Globals);
        // their "_i<N>" names say the same (gfx_spark_blink_anim_em_i12000: 12000)
        M.HdrScale = 0;
        if (Has(Lower(M.Techset), "effect_lit_emissive"))
            if (const auto C = V.find("constants"); C != V.end() && C->is_array() && C->size() > 6 && (*C)[6].is_array() &&
                                                    (*C)[6].size() > 46 && (*C)[6][46].is_number())
                M.HdrScale = Num((*C)[6][46]);
        if (const auto A = V.find("atlas"); A != V.end() && A->is_array() && A->size() == 2 && (*A)[0].is_number() && (*A)[1].is_number())
        {
            M.AtlasRows = (int)Num((*A)[0]);
            M.AtlasCols = (int)Num((*A)[1]);
        }
        M.Frames.clear();
        M.FrameRate = 0;
        if (const auto Fr = V.find("frames"); Fr != V.end() && Fr->is_array() && Fr->size() <= 64)
        {
            for (const auto& F : *Fr)
                if (F.is_string() && !F.get<std::string>().empty()) M.Frames.push_back(F.get<std::string>());
            const float Rate = Num(Get(V, "frameRate"));
            M.FrameRate = std::isfinite(Rate) ? std::clamp(Rate, 0.0f, 1000.0f) : 0.0f;
            if (M.Frames.size() < 2 || M.FrameRate <= 0) M.Frames.clear(), M.FrameRate = 0;
        }
        M.Slots.clear();
        if (const auto Im = V.find("images"); Im != V.end() && Im->is_array())
            for (const auto& I : *Im)
                M.Slots.push_back({ Str(I, "usage"), (uint32_t)Int(Get(I, "semantic")), Str(I, "name") });
        SelectImages(M);
        Classify(M);
    }

    void ReadVisuals(const Json& E, Element& El)
    {
        const auto It = E.find("visuals");
        if (It == E.end() || !It->is_array()) return;
        for (const auto& V : *It)
        {
            if (!V.is_object()) continue;
            if (const std::string N = Str(V, "material"); !N.empty())
            {
                Material M;
                M.Name = N;
                ReadMaterialFields(V, M);
                El.Materials.push_back(std::move(M));
            }
            if (const std::string N = Str(V, "model"); !N.empty()) El.Models.push_back(N);
            if (const std::string N = Str(V, "effect"); !N.empty()) El.Effects.push_back(N);
        }
        if (const auto Fx = E.find("effects"); Fx != E.end() && Fx->is_array())
            for (const auto& N : *Fx)
                if (N.is_string() && !N.get<std::string>().empty()) El.Effects.push_back(N.get<std::string>());
    }

    VisualSample VisualOf(const Json& S)
    {
        VisualSample V;
        const auto Part = [&](const char* Key, float* Color, int K) {
            const auto It = S.find(Key);
            if (It == S.end()) return;
            const Json& P = *It;
            if (const auto C = P.find("color"); C != P.end() && C->is_array() && C->size() == 4)
                for (int I = 0; I < 4; I++) Color[I] = Num((*C)[I]) / 255.0f;
            const auto F = [&](const char* Name, Range& R) {
                const auto X = P.find(Name);
                if (X == P.end() || !X->is_number()) return;
                (K == 0 ? R.Base : R.Amp) = Num(*X);
            };
            F("rotationDelta", V.RotationDelta);
            F("rotationTotal", V.RotationTotal);
            F("scale", V.Scale);
            F("lightIntensity", V.LightIntensity);
            F("lightRadius", V.LightRadius);
            F("lightFov", V.LightFov);
            if (const auto Sz = P.find("size"); Sz != P.end() && Sz->is_array() && Sz->size() == 2)
                for (int I = 0; I < 2; I++) (K == 0 ? V.Size[I].Base : V.Size[I].Amp) = Num((*Sz)[I]);
        };
        Part("base", V.ColorBase, 0);
        Part("amplitude", V.ColorAmp, 1);
        return V;
    }

    VelocitySample VelocityOf(const Json& S)
    {
        VelocitySample V;
        const char* Frames[] = { "local", "world" };
        for (int F = 0; F < 2; F++)
        {
            const auto It = S.find(Frames[F]);
            if (It == S.end()) continue;
            const auto Pair = [&](const char* Key, Range* Out) {
                const auto P = It->find(Key);
                if (P == It->end()) return;
                const Json& B = Get(*P, "base");
                const Json& A = Get(*P, "amplitude");
                for (int I = 0; I < 3 && B.is_array() && A.is_array() && I < (int)B.size() && I < (int)A.size(); I++)
                    Out[I] = Range{ Num(B[I]), Num(A[I]) };
            };
            Pair("velocity", V.Velocity[F]);
            Pair("totalDelta", V.TotalDelta[F]);
        }
        return V;
    }

    // the Infinity Ward games' extended defs (Games/IW/IwEffects.cpp, TreyarchEffects.cpp
    // IwExtended): a spark fountain's, a trail's profile, a spot light's
    void ReadExtended(const Json& E, Element& El)
    {
        // a number, or Else when missing; finite and within a hundred million (an odd file)
        const auto Field = [](const Json& J, const char* Key, float Else = 0.0f) {
            const Json& V = Get(J, Key);
            const float F = V.is_number() ? V.get<float>() : Else;
            return std::isfinite(F) ? std::clamp(F, -1e8f, 1e8f) : Else;
        };
        if (const Json& S = Get(E, "sparkFountainDef"); S.is_object())
        {
            SparkFountain F;
            F.Gravity = Field(S, "gravity");
            F.BounceFrac = Field(S, "bounceFrac");
            F.BounceRand = Field(S, "bounceRand");
            F.Spacing = Field(S, "sparkSpacing");
            F.Length = Field(S, "sparkLength");
            F.Count = std::clamp(Int(Get(S, "sparkCount")), 0, 1024);
            F.LoopTime = Field(S, "loopTime");
            F.VelMin = Field(S, "velMin");
            F.VelMax = Field(S, "velMax");
            F.ConeFrac = std::clamp(Field(S, "velConeFrac", 1.0f), 0.0f, 1.0f);
            F.RestSpeed = Field(S, "restSpeed");
            F.BoostTime = Field(S, "boostTime");
            F.BoostFactor = Field(S, "boostFactor");
            El.Fountain = F;
        }
        if (const Json& T = Get(E, "trailDef"); T.is_object())
        {
            TrailDef D;
            D.ScrollTimeMsec = Int(Get(T, "scrollTimeMsec"));
            D.RepeatDist = std::max(0.0f, Field(T, "repeatDist"));
            if (const Json& V = Get(T, "verts"); V.is_array())
                for (const auto& X : V)
                {
                    if (!X.is_array() || X.size() < 5) break;
                    TrailDef::Vert Q{};
                    for (int K = 0; K < 5; K++)
                    {
                        const float F = Num(X[K]);
                        (K < 2 ? Q.Pos[K] : K < 4 ? Q.Normal[K - 2] : Q.U) = std::isfinite(F) ? std::clamp(F, -16.0f, 16.0f) : 0.0f;
                    }
                    D.Verts.push_back(Q);
                }
            // pairs of vertices it has
            if (const Json& I = Get(T, "inds"); I.is_array())
                for (size_t K = 0; K + 1 < I.size(); K += 2)
                {
                    const int A = Int(I[K]), B = Int(I[K + 1]);
                    if (A < 0 || B < 0 || A >= (int)D.Verts.size() || B >= (int)D.Verts.size()) continue;
                    D.Inds.push_back((uint16_t)A);
                    D.Inds.push_back((uint16_t)B);
                }
            El.Trail = std::move(D);
        }
        if (const Json& S = Get(E, "spotLightDef"); S.is_object())
        {
            LightShape& L = El.Light;
            L.Brightness = std::max(0.0f, Field(S, "brightness"));
            const float Length = std::max(0.0f, Field(S, "maxLength"));
            if (Get(S, "halfFovOuter").is_number())
            {
                // AW, MWR: half its angle (in degrees past a right angle's radians) and its reach
                float Half = std::fabs(Field(S, "halfFovOuter"));
                L.HalfFov = std::min(Half > 1.6f ? Half * 3.14159265f / 180.0f : Half, 1.5f);
                L.Reach = std::max(0.0f, Field(S, "radius"));
                if (L.Reach <= 0) L.Reach = Length;
            }
            else
            {
                const float Start = std::max(0.0f, Field(S, "startRadius")), End = std::max(0.0f, Field(S, "endRadius"));
                L.StartRadius = Start;
                L.Reach = Length;
                if (Length > 0 && End > Start) L.HalfFov = std::min(std::atan((End - Start) / Length), 1.5f);
            }
        }
    }

    bool ReadTreyarch(const Json& J, Effect& Fx)
    {
        Fx.Name = Str(J, "name");
        Fx.LoopingLifeMsec = Int(Get(J, "loopingLifeMsec"));
        Fx.NonLoopingLifeMsec = Int(Get(J, "nonLoopingLifeMsec"));
        if (const auto B = J.find("boundingBoxDim"); B != J.end() && B->is_array() && B->size() >= 3)
        {
            for (int I = 0; I < 3; I++) Fx.BoundsHalf[I] = std::max(1.0f, std::fabs(Num((*B)[I])));
            Fx.BoundsKnown = true;
        }
        if (const auto C = J.find("boundingCenter"); C != J.end() && C->is_array() && C->size() >= 3)
            for (int I = 0; I < 3; I++) Fx.BoundsCenter[I] = Num((*C)[I]);
        else if (const auto S = J.find("boundingSphere"); S != J.end() && S->is_array() && S->size() >= 3)
            for (int I = 0; I < 3; I++) Fx.BoundsCenter[I] = Num((*S)[I]);

        const auto Els = J.find("elements");
        if (Els == J.end() || !Els->is_array()) return false;
        Fx.Timed = false;
        for (const auto& E : *Els)
        try
        {
            Element El;
            const std::string G = Str(E, "group");
            El.Group = G == "looping" ? SpawnGroup::Looping : G == "emission" ? SpawnGroup::Emission : SpawnGroup::OneShot;
            El.TypeName = Str(E, "type");
            El.Type = TypeOf(El.TypeName);
            ReadVisuals(E, El);
            if (!E.contains("spawn"))
            {
                // no timings (Cold War): each element one still particle, side by side along Y
                // (24 units apart, centred on the origin): what each draws, one next to the other
                El.Group = SpawnGroup::Looping;
                El.IntervalMsec = 1000;
                El.LifeMsec = { 1000, 0 };
                El.Visual.resize(2);
                for (auto& V : El.Visual) V.Size[0] = V.Size[1] = { 10, 0 };
                El.Origin[1] = { 24.0f * Fx.Elements.size() - 12.0f * (Els->size() - 1), 0 };
                El.Origin[2] = { 12, 0 };
                Fx.Elements.push_back(std::move(El));
                continue;
            }
            Fx.Timed = true;
            const Range Spawn = RangeOf(Get(E, "spawn"));
            if (El.Group == SpawnGroup::Looping)
            {
                El.IntervalMsec = std::max(1, (int)Spawn.Base);
                El.LoopCount = Spawn.Amp >= (float)INT_MAX || Spawn.Amp <= 0 ? 0 : (int)Spawn.Amp;
                if (E.contains("spawnLoopingSpawnCount")) El.PerInterval = RangeOf(Get(E, "spawnLoopingSpawnCount"));
                if (El.PerInterval.Base <= 0 && El.PerInterval.Amp <= 0) El.PerInterval = { 1, 0 };
            }
            else El.OneShotCount = Spawn;
            El.DelayMsec = RangeOf(Get(E, "spawnDelayMsec"));
            El.LifeMsec = RangeOf(Get(E, "lifeSpanMsec"));
            for (int I = 0; I < 3; I++)
            {
                El.Origin[I] = RangeOf(Get(Get(E, "spawnOrigin"), I));
                El.Angles[I] = RangeOf(Get(Get(E, "spawnAngles"), I));
                El.AngularVelocity[I] = RangeOf(Get(Get(E, "angularVelocity"), I));
            }
            const std::string Off = Str(E, "spawnOffset");
            El.Offset = Off == "sphere" ? SpawnOffset::Sphere : Off == "cylinder" ? SpawnOffset::Cylinder : SpawnOffset::None;
            El.OffsetRadius = RangeOf(Get(E, "spawnOffsetRadius"));
            El.OffsetHeight = RangeOf(Get(E, "spawnOffsetHeight"));
            const std::string Run = Str(E, "runRelativeTo");
            El.Relative = Run == "world" ? RunRelative::World : Run == "spawn" ? RunRelative::Spawn
                        : Run == "offset" ? RunRelative::Offset : RunRelative::Effect;
            El.InitialRotation = RangeOf(Get(E, "initialRotation"));
            El.Gravity = RangeOf(Get(E, "gravity"));
            El.NonUniformScale = HasFlag(E, "nonUniformScale");
            El.Collide = HasFlag(E, "useCollision");
            El.DieOnTouch = HasFlag(E, "dieOnTouch");
            El.LitType = Int(Get(E, "litType"));
            if (const auto X = E.find("extraFlags"); X != E.end() && X->is_array())
                for (const auto& F : *X) El.UnderwaterOnly |= F.is_string() && F.get<std::string>() == "underwaterOnly";
            // Black Ops and II say which graphs move the element; III moves by both
            if (!E.contains("extraFlags"))
            {
                El.UseLocalVelocity = HasFlag(E, "hasVelocityGraphLocal");
                El.UseWorldVelocity = HasFlag(E, "hasVelocityGraphWorld");
            }
            if (const auto A = E.find("atlas"); A != E.end() && A->is_object())
            {
                const int Behavior = Int(Get(*A, "behavior"));
                El.AtlasStart = Behavior & 3;
                El.AtlasOverLife = (Behavior & 4) != 0;
                El.AtlasLoopLimited = (Behavior & 8) != 0;
                El.AtlasIndex = Int(Get(*A, "index"));
                El.AtlasFps = Int(Get(*A, "fps"));
                El.AtlasLoops = Int(Get(*A, "loopCount"));
                El.AtlasCols = 1 << std::clamp(Int(Get(*A, "colIndexBits")), 0, 8);
                El.AtlasRows = 1 << std::clamp(Int(Get(*A, "rowIndexBits")), 0, 8);
            }
            // animated materials (CoD1's animMap): their frames side by side, played from each
            // particle's birth at their rate, looping, when the element's own atlas does not play
            // them (the game plays them on its clock: the phase differs); all its materials alike
            if (!El.Materials.empty() && El.AtlasCols * El.AtlasRows <= 1 &&
                std::all_of(El.Materials.begin(), El.Materials.end(), [&](const Material& M) {
                    return M.Frames.size() > 1 && M.Frames.size() == El.Materials[0].Frames.size() && M.FrameRate == El.Materials[0].FrameRate; }))
            {
                El.AtlasCols = (int)El.Materials[0].Frames.size();
                El.AtlasRows = 1;
                El.AtlasStart = 0, El.AtlasIndex = 0, El.AtlasOverLife = El.AtlasLoopLimited = false;
                El.AtlasFps = std::max(1, (int)std::lround(El.Materials[0].FrameRate));
            }
            if (const auto V = E.find("velocitySamples"); V != E.end() && V->is_array())
                for (const auto& S : *V) El.Velocity.push_back(VelocityOf(S));
            if (const auto V = E.find("visualSamples"); V != E.end() && V->is_array())
                for (const auto& S : *V) El.Visual.push_back(VisualOf(S));
            El.OnDeath = Str(E, "fxOnDeath");
            El.OnImpact = Str(E, "fxOnImpact");
            El.Emitted = Str(E, "fxEmitted");
            El.Attached = Str(E, "fxAttached");
            El.EmitDist = RangeOf(Get(E, "emitDist"));
            El.EmitDistVariance = RangeOf(Get(E, "emitDistVariance"));
            ReadExtended(E, El);
            Fx.Elements.push_back(std::move(El));
        }
        catch (const std::exception&) {}   // an element whose fields are not what the writer writes: left out
        return true;
    }
}

void ReadMaterial(const nlohmann::json& V, Material& M) { ReadMaterialFields(V, M); }

int Effect::OneShotMsec() const
{
    int Most = 0;
    for (const auto& E : Elements)
        if (E.Group != SpawnGroup::Looping)
            Most = std::max(Most, (int)(E.DelayMsec.Base + std::max(0.0f, E.DelayMsec.Amp) + E.LifeMsec.Base + std::max(0.0f, E.LifeMsec.Amp)));
    return Most;
}

bool Effect::Loops() const
{
    for (const auto& E : Elements)
        if (E.Group == SpawnGroup::Looping) return true;
    return false;
}

bool LoadEffect(const std::string& JsonPath, Effect& Out, std::string& Error)
{
    std::ifstream In(PathOf(JsonPath), std::ios::binary);
    if (!In)
    {
        Error = "cannot open " + JsonPath;
        return false;
    }
    Json J;
    try { J = Json::parse(In); }
    catch (const std::exception& Ex)
    {
        Error = std::string("not JSON: ") + Ex.what();
        return false;
    }
    Out = Effect{};
    Out.File = JsonPath;
    // the module system of Infinite Warfare and after (Games/IW/Vfx.h)
    if (Str(J, "format") == "vfx")
    {
        try { return Vfx::ReadPreview(J, Out, Error); }
        catch (const std::exception& Ex)
        {
            Error = std::string("unexpected JSON: ") + Ex.what();
            return false;
        }
    }
    if (!J.is_object() || !J.contains("elements"))
    {
        Error = "not an effect (no elements)";
        return false;
    }
    // the Treyarch layout, which the Infinity Ward games' writer (IwEffects.h) writes too, its
    // game named; another game's adapter would be picked here by the fields its writer sets
    // (Black Ops: a bounding sphere; II and III: a bounding centre; Cold War: no lifetimes)
    Out.Game = J.contains("game") ? Str(J, "game")
             : !J.contains("loopingLifeMsec") ? "coldwar" : J.contains("boundingSphere") ? "bo1" : "treyarch";
    try
    {
        if (!ReadTreyarch(J, Out))
        {
            Error = "elements not read";
            return false;
        }
        // Advanced Warfare and MWR keep an element's colour linear (smoke at 12/255, lit by the
        // light grid): shown in sRGB, both colours a particle picks between (base and amplitude,
        // VisualAt). A lit element (litType 2, 3) that blends is lit as a neutral sky and sun
        // would light it, 3 times its colour: a tint, grey smoke at 40/255 to 178/255, black
        // smoke at 1/255 to 10/255 (fire_lp_m_blacksmk); unlit, the grey smoke draws black
        if (Out.Game == "aw" || Out.Game == "mwr")
            for (auto& E : Out.Elements)
            {
                float Light = 1;   // the light it gets, linear
                const bool Blends = !E.Materials.empty() && std::none_of(E.Materials.begin(), E.Materials.end(), [](const Material& M) {
                    return M.Mode == Blend::Additive || M.Distortion || M.LightSprite; });
                if ((E.LitType == 2 || E.LitType == 3) && Blends) Light = 3;
                for (auto& V : E.Visual)
                    for (int I = 0; I < 3; I++)
                    {
                        const auto Srgb = [](float C) { C = std::clamp(C, 0.0f, 1.0f); return C <= 0.0031308f ? 12.92f * C : 1.055f * std::pow(C, 1 / 2.4f) - 0.055f; };
                        V.ColorBase[I] = Srgb(V.ColorBase[I] * Light);
                        V.ColorAmp[I] = Srgb(V.ColorAmp[I] * Light);
                    }
            }
    }
    catch (const std::exception& Ex)
    {
        Error = std::string("unexpected JSON: ") + Ex.what();
        return false;
    }
    return true;
}

// ---- Library

Library::Library(std::string Root) : RootDir(std::move(Root)) {}

std::shared_ptr<const Effect> Library::LoadRoot(const std::string& JsonPath, std::string& Error)
{
    auto Fx = std::make_shared<Effect>();
    if (!LoadEffect(JsonPath, *Fx, Error)) return nullptr;
    if (RootDir.empty())
    {
        // fx/<a>/<b>.json named "a/b": the root is as many folders up as the name has parts
        fs::path Dir = PathOf(JsonPath).parent_path();
        for (char C : Fx->Name)
            if (C == '/' || C == '\\') Dir = Dir.parent_path();
        RootDir = Text(Dir);
    }
    Complete(*Fx);
    Cache[Lower(Fx->Name)] = Fx;
    return Fx;
}

void Library::Complete(Effect& Fx) const
{
    // a material the effect's zone only named: as the zone that defines it wrote it
    if (Materials)
        for (auto& E : Fx.Elements)
            for (auto& M : E.Materials)
            {
                const Material* Def = Materials->FindMaterial(M.Name);
                if (!Def) continue;
                if (M.Images.empty())
                {
                    const std::string Name = M.Name;
                    M = *Def;
                    M.Name = Name;
                }
                else if (M.Techset.empty() && M.AtlasRows == 0 && Def->AtlasRows > 0)
                {
                    // a name-only material whose images were found elsewhere: its techset and atlas
                    // as the defining zone wrote them
                    auto Images = std::move(M.Images);
                    M = *Def;
                    if (M.Images.empty()) M.Images = std::move(Images);
                }
            }
    // an element whose grid is its material's
    for (auto& E : Fx.Elements)
        if (E.AtlasFromMaterial && E.AtlasCols * E.AtlasRows <= 1)
            for (const auto& M : E.Materials)
                if (M.AtlasRows * M.AtlasCols > 1)
                {
                    E.AtlasRows = M.AtlasRows;
                    E.AtlasCols = M.AtlasCols;
                    break;
                }
}

std::shared_ptr<const Effect> Library::ByName(const std::string& Name)
{
    if (Name.empty()) return nullptr;
    std::string Key = Lower(Name);
    std::replace(Key.begin(), Key.end(), '\\', '/');
    if (const auto It = Cache.find(Key); It != Cache.end()) return It->second;
    std::shared_ptr<Effect> Fx;
    std::vector<std::string> Dirs{ RootDir };
    Dirs.insert(Dirs.end(), Extra.begin(), Extra.end());
    for (const auto& D : Dirs)
    {
        if (D.empty()) continue;
        std::error_code Ec;
        const fs::path P = PathOf(D) / PathOf(Key + ".json");
        if (!fs::exists(P, Ec)) continue;
        auto Loaded = std::make_shared<Effect>();
        std::string Error;
        if (LoadEffect(Text(P), *Loaded, Error))
        {
            Complete(*Loaded);
            Fx = Loaded;
            break;
        }
    }
    Cache[Key] = Fx;
    return Fx;
}

// ---- TextureIndex

void TextureIndex::AddFolder(const std::string& Dir)
{
    if (Dir.empty() || std::find(Folders.begin(), Folders.end(), Dir) != Folders.end()) return;
    Folders.push_back(Dir);
    std::error_code Ec;
    const fs::path Root = PathOf(Dir);
    if (!fs::is_directory(Root, Ec)) return;
    // Walked with FindFirstFileEx (a game's export is tens of thousands of files; a name is only
    // converted when it is kept), in the order recursive_directory_iterator walks: a folder's
    // entries in turn, each subfolder entered when met.
    const auto IsExt = [](const wchar_t* Dot, const wchar_t* Ext) { return Dot && _wcsicmp(Dot, Ext) == 0; };
    std::function<void(const std::wstring&, int)> Walk = [&](const std::wstring& Folder, int Depth) {
        WIN32_FIND_DATAW Fd{};
        const HANDLE Find = FindFirstFileExW((Folder + L"\\*").c_str(), FindExInfoBasic, &Fd, FindExSearchNameMatch, nullptr,
                                             FIND_FIRST_EX_LARGE_FETCH);
        if (Find == INVALID_HANDLE_VALUE) return;
        do
        {
            const wchar_t* Name = Fd.cFileName;
            if (Fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (std::wcscmp(Name, L".") == 0 || std::wcscmp(Name, L"..") == 0 || (Fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    continue;
                // an export's anims and sounds hold nothing an effect draws: not walked
                if (Depth == 0 && (_wcsicmp(Name, L"anims") == 0 || _wcsicmp(Name, L"sounds") == 0)) continue;
                Walk(Folder + L"\\" + Name, Depth + 1);
                continue;
            }
            const wchar_t* Dot = std::wcsrchr(Name, L'.');
            if (!Dot) continue;
            const bool Dds = IsExt(Dot, L".dds");
            const bool Image = Dds || IsExt(Dot, L".png") || IsExt(Dot, L".tga") || IsExt(Dot, L".tiff") || IsExt(Dot, L".tif");
            const bool Model = !Image && (IsExt(Dot, L".semodel") || IsExt(Dot, L".cast") || IsExt(Dot, L".xmodel_export"));
            const bool JsonFile = !Image && !Model && IsExt(Dot, L".json");
            if (!Image && !Model && !JsonFile) continue;
            const fs::path File = fs::path(Folder) / Name;
            if (JsonFile)
            {
                // <game>/materials/...: the material kind's files, by the name each holds (not the
                // models' <model>.materials.json sidecars, a thousand of them in a game)
                if (Lower(Text(fs::path(Folder))).find("materials") == std::string::npos) continue;
                std::ifstream In(File, std::ios::binary);
                Json J;
                try { J = Json::parse(In); }
                catch (const std::exception&) { continue; }
                if (!J.is_object() || !J.contains("images") || !J.contains("name")) continue;
                Material M;
                M.Name = Str(J, "name");
                try { ReadMaterialFields(J, M); }
                catch (const std::exception&) { continue; }
                if (!M.Name.empty()) MaterialFiles.emplace(Lower(M.Name), std::move(M));
                continue;
            }
            std::string Stem = Lower(Strings::ToNormalString(std::wstring(Name, Dot)));
            if (Model)
            {
                // models/<name>/<name>_LOD0.<ext>: the model's first LOD, by its name
                const size_t Lod = Stem.rfind("_lod");
                if (Lod != std::string::npos && Lod + 5 == Stem.size() && std::isdigit((unsigned char)Stem.back()))
                {
                    if (Stem.back() != '0') continue;
                    Stem.erase(Lod);
                }
                if (!Stem.empty()) ModelFiles.emplace(Stem, Text(File));
                continue;
            }
            if (Stem.empty()) continue;
            // a DDS (mips, block compressed) before the others; the first copy found otherwise
            const auto Old = Files.find(Stem);
            const auto IsDdsPath = [](const std::string& P) { return P.size() >= 4 && Lower(P.substr(P.size() - 4)) == ".dds"; };
            if (Old == Files.end()) Files.emplace(std::move(Stem), Text(File));
            else if (Dds && !IsDdsPath(Old->second)) Old->second = Text(File);
        } while (FindNextFileW(Find, &Fd));
        FindClose(Find);
    };
    Walk(Root.wstring(), 0);
}

const Material* TextureIndex::FindMaterial(const std::string& Name) const
{
    std::string N = Lower(Name);
    if (!N.empty() && N[0] == ',') N.erase(0, 1);
    const auto It = MaterialFiles.find(N);
    return It == MaterialFiles.end() ? nullptr : &It->second;
}

std::string TextureIndex::FindModel(const std::string& ModelName) const
{
    std::string N = Lower(ModelName);
    if (const size_t Slash = N.find_last_of("/\\"); Slash != std::string::npos) N = N.substr(Slash + 1);
    const auto It = ModelFiles.find(N);
    return It == ModelFiles.end() ? std::string() : It->second;
}

std::string TextureIndex::FindImage(const std::string& ImageName) const
{
    std::string N = Lower(ImageName);
    if (!N.empty() && N[0] == ',') N.erase(0, 1);
    if (const size_t Slash = N.find_last_of("/\\"); Slash != std::string::npos) N = N.substr(Slash + 1);
    const auto It = Files.find(N);
    return It == Files.end() ? std::string() : It->second;
}

std::string TextureIndex::Find(const Material& M) const
{
    for (const auto& I : M.Images)
        if (std::string P = FindImage(I); !P.empty()) return P;
    // by name: gfx_fxt_x (Black Ops) or gfx_x (Black Ops III) -> fxt_x, then without its last
    // _suffix, again, as long as something is left (gfx_fire_flicker_base_1_anim_em -> fxt_fire_flicker_base_1_anim)
    std::string N = Lower(M.Name);
    if (const size_t Slash = N.find_last_of("/\\"); Slash != std::string::npos) N = N.substr(Slash + 1);
    std::vector<std::string> Bases{ N };
    if (N.rfind("gfx_fxt_", 0) == 0) Bases.push_back(N.substr(4));
    else if (N.rfind("gfx_", 0) == 0) Bases.push_back("fxt_" + N.substr(4));
    if (N.rfind("mc/", 0) == 0) Bases.push_back(N.substr(3));
    for (int Trim = 0; Trim < 4; Trim++)
    {
        for (const auto& B : Bases)
            if (std::string P = FindImage(B); !P.empty()) return P;
        bool Any = false;
        for (auto& B : Bases)
            if (const size_t U = B.find_last_of('_'); U != std::string::npos && U > 4)
            {
                B.erase(U);
                Any = true;
            }
        if (!Any) break;
    }
    return std::string();
}

std::vector<std::string> GameFolders(const std::string& JsonPath)
{
    // <out>/<game>/fx/...: the game folder is the one above the nearest "fx" - or above a farther
    // one beside the images or the materials (the Infinity Ward games name effects fx/...)
    std::vector<std::string> Out;
    std::error_code Ec;
    fs::path P = PathOf(JsonPath).parent_path(), Fx;
    for (; !P.empty() && P.has_parent_path() && P.parent_path() != P; P = P.parent_path())
        if (Lower(Text(P.filename())) == "fx")
        {
            const bool Beside = fs::is_directory(P.parent_path() / "images", Ec) || fs::is_directory(P.parent_path() / "materials", Ec);
            if (Fx.empty() || Beside) Fx = P;
            if (Beside) break;
        }
    if (Fx.empty()) return Out;
    const fs::path Game = Fx.parent_path();
    Out.push_back(Text(Game));
    // <out2>/<game> beside <out>: another export of the same game (its common zone)
    const fs::path Export = Game.parent_path(), Above = Export.parent_path();
    const fs::path Id = Game.filename();
    if (Above.empty() || Above == Export) return Out;
    int Seen = 0;
    for (auto It = fs::directory_iterator(Above, Ec); !Ec && It != fs::directory_iterator() && Seen < 256; It.increment(Ec), Seen++)
    {
        if (!It->is_directory(Ec) || It->path() == Export) continue;
        const fs::path Other = It->path() / Id;
        if (fs::is_directory(Other / "images", Ec) || fs::is_directory(Other / "fx", Ec)) Out.push_back(Text(Other));
    }
    return Out;
}
}
