#include "stdafx.h"

#include "Games/IW/IwEffects.h"

#include "CoDAssetType.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "Games/IW/IWZone.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace IwEffects
{
namespace
{
    using Json = nlohmann::ordered_json;
    using ExportNaming::At;

#pragma pack(push, 1)
    struct FxIntRange { int32_t Base, Amplitude; };
    struct FxFloatRange { float Base, Amplitude; };
    // FxElemDef up to its counts: the same in Ghosts, Advanced Warfare and Modern Warfare Remastered
    struct ElemHead
    {
        uint32_t Flags, Flags2;
        FxIntRange Spawn;
        FxFloatRange SpawnRange, FadeInRange, FadeOutRange;
        float SpawnFrustumCullRadius;
        FxIntRange SpawnDelayMsec, LifeSpanMsec;
        FxFloatRange SpawnOrigin[3], SpawnOffsetRadius, SpawnOffsetHeight, SpawnAngles[3], AngularVelocity[3], InitialRotation;
        FxFloatRange Gravity, ReflectionFactor;
        uint8_t AtlasBehavior, AtlasIndex, AtlasFps, AtlasLoopCount, AtlasColIndexBits, AtlasRowIndexBits;
        int16_t AtlasEntryCount;
        uint8_t ElemType, LitType, VisualCount, VelIntervalCount, VisStateIntervalCount;
    };
#pragma pack(pop)
    // where the tables (iw6 f1402937f0, s1 f246960, h1 f36caa0) read the element: the type at
    // 0xb4 and the visual count at 0xb6 (the visuals' switch), the velocity samples (0x60 each)
    // at 0xc0 by the count at 0xb7, the visual samples at 0xc8 by 0xb8, the visuals at 0xd0, the
    // three effect names at 0xf0, the extended definition at 0x118
    static_assert(offsetof(ElemHead, ElemType) == 0xb4 && offsetof(ElemHead, VisualCount) == 0xb6 &&
                  offsetof(ElemHead, VelIntervalCount) == 0xb7 && offsetof(ElemHead, VisStateIntervalCount) == 0xb8, "IW element");
    constexpr int64_t VelSamplesAt = 0xc0, VisSamplesAt = 0xc8, VisualsAt = 0xd0, BoundsAt = 0xd8, EffectsAt = 0xf0;
    constexpr int64_t EmitDistAt = 0x108, ExtendedAt = 0x118, SortOrderAt = 0x120;

    Json Range(const FxFloatRange& R) { return Json::array({ R.Base, R.Amplitude }); }
    Json Range(const FxIntRange& R) { return Json::array({ R.Base, R.Amplitude }); }
    Json Ranges(const FxFloatRange* R) { return Json::array({ Range(R[0]), Range(R[1]), Range(R[2]) }); }
    Json Floats(const float* V, int N)
    {
        Json Out = Json::array();
        for (int I = 0; I < N; I++) Out.push_back(V[I]);
        return Out;
    }

    class Reader
    {
    public:
        explicit Reader(const Treyarch::EffectZone& Z) : Z(Z), Img(*Z.Image) {}

        // the zone offset of Size bytes at an address, -1 when they are not all in the zone
        int64_t Fits(uint64_t Address, int64_t Size) const
        {
            const int64_t O = Z.Off(Address);
            return O >= 0 && O + Size <= (int64_t)Img.size() ? O : -1;
        }
        uint64_t Pointer(int64_t Pos) const { return At<uint64_t>(Img, Pos); }
        // the asset an address points at, by the name its first field points to (a name-only
        // header's ",name" as name)
        std::string Asset(uint64_t Address) const { return Z.Asset(Address); }
        // an effect, a sound or a vector field the zone holds by name
        std::string Name(uint64_t Address) const { return Address ? Z.String(Address) : std::string(); }

        // a material visual with its techset, images and blending when the zone's reader gives
        // them (the Treyarch writer's fields); defined in another zone: pulled from there
        Json Material(uint64_t Address) const
        {
            const std::string N = Asset(Address);
            if (N.empty()) return Json();
            Json V{ { "material", N } };
            if (!Z.Material) return V;
            // a name-only header (",name", no techset; AW's and MWR's images named by a book):
            // the material kind's file of the zone defining it, techset with it, gives it whole
            if (const int64_t O = Z.Off(Address); O >= 0 && !Z.AssetName && Z.String(Pointer(O)).starts_with(","))
            {
                if (Z.NeedMaterial) Z.NeedMaterial(N);
                return V;
            }
            Z.AddMaterial(V, N, Address);
            return V;
        }

        const Treyarch::EffectZone& Z;
        const std::vector<uint8_t>& Img;
    };

    template <class T>
    Json Bits(uint32_t V, const T& Table)
    {
        Json Out = Json::array();
        for (const auto& B : Table)
            if (V & B.Mask) Out.push_back(B.Name);
        return Out;
    }

    // What an element draws or plays (the tables' visuals switch): one item is the slot itself,
    // more an array of 8-byte slots; a decal is always an array of mark visuals, each its materials
    Json Visuals(const Reader& R, const Layout& L, const std::string& Type, uint8_t Count, uint64_t Slot)
    {
        Json Out = Json::array();
        if (Type == "omniLight") return Out;
        if (Type == "decal")
        {
            const int64_t Array = R.Fits(Slot, Count * L.DecalStride);
            for (int I = 0; Array >= 0 && I < Count; I++)
            {
                Json Mark = Json::array();
                for (int K = 0; K < L.DecalMaterials; K++)
                    if (Json M = R.Material(R.Pointer(Array + I * L.DecalStride + 8 * K)); !M.is_null()) Mark.push_back(std::move(M));
                // the first of each mark drawn, the others (BO3's way) listed with it
                if (!Mark.empty())
                {
                    Json V = Mark[0];
                    if (Mark.size() > 1) V["marks"] = Mark;
                    Out.push_back(std::move(V));
                }
            }
            return Out;
        }
        const auto One = [&](uint64_t A) -> Json {
            if (A == 0) return Json();
            if (Type == "model") return Json{ { "model", R.Asset(A) } };
            if (Type == "runner") return Json{ { "effect", R.Name(A) } };
            if (Type == "sound") return Json{ { "sound", R.Name(A) } };
            if (Type == "vectorField") return Json{ { "vectorField", R.Name(A) } };
            // WWII's types the s2 table reads a string for (15, 19) or nothing (20), none seen
            // in a zone yet but 20 (rain and wakes, no visual)
            if (Type == "15" || Type == "19") return Json{ { "name", R.Name(A) } };
            if (Type == "20") return Json();
            if (Type == "spotLight") return Json{ { "lightDef", R.Asset(A) } };
            if (Type == "particleSimAnimation")
            {
                // FxParticleSimAnimation: its name, then its material
                Json V{ { "particleSimAnimation", R.Asset(A) } };
                if (const int64_t O = R.Fits(A, 0x10); O >= 0)
                    if (Json M = R.Material(R.Pointer(O + 8)); !M.is_null()) V.update(M);
                return V;
            }
            return R.Material(A);
        };
        if (Count == 1)
        {
            if (Json V = One(Slot); !V.is_null()) Out.push_back(std::move(V));
            return Out;
        }
        const int64_t Array = R.Fits(Slot, 8 * (int64_t)Count);
        for (int I = 0; Array >= 0 && I < Count; I++)
            if (Json V = One(R.Pointer(Array + 8 * I)); !V.is_null()) Out.push_back(std::move(V));
        return Out;
    }

    // velocity samples (0x60 each: local velocity and total delta, then world)
    Json VelocitySamples(const Reader& R, const Layout& L, uint64_t At, int Count)
    {
        Json Out = Json::array();
        const int64_t O = R.Fits(At, L.VelSample * Count);
        for (int I = 0; O >= 0 && I < Count; I++)
        {
            float V[24];
            std::memcpy(V, R.Img.data() + O + L.VelSample * I + L.VelFirst, sizeof(V));
            const auto Part = [&](int K) {
                return Json{ { "velocity", { { "base", Floats(V + K, 3) }, { "amplitude", Floats(V + K + 3, 3) } } },
                             { "totalDelta", { { "base", Floats(V + K + 6, 3) }, { "amplitude", Floats(V + K + 9, 3) } } } };
            };
            Out.push_back(Json{ { "local", Part(0) }, { "world", Part(12) } });
        }
        return Out;
    }

    // visual state samples: base then amplitude, each a colour (four floats, written 0..255 as
    // the others' bytes), then Ghosts' unlitHDRScale or AW/MWR's emissiveScale[3], rotation delta
    // and total, size (2), scale, and AW/MWR's pivot (2)
    Json VisualSamples(const Reader& R, const Layout& L, uint64_t At, int Count)
    {
        Json Out = Json::array();
        const int64_t O = R.Fits(At, 2 * L.VisState * Count);
        for (int I = 0; O >= 0 && I < Count; I++)
        {
            const auto Part = [&](int64_t P) {
                float F[16] = {};
                std::memcpy(F, R.Img.data() + P, std::min<size_t>(sizeof(F), (size_t)L.VisState));
                Json J{ { "color", Json::array({ F[0] * 255.0f, F[1] * 255.0f, F[2] * 255.0f, F[3] * 255.0f }) } };
                const float* S = F + 4;
                if (L.EmissiveRgb)
                {
                    J["emissiveScale"] = Floats(S, 3);
                    S += 3;
                }
                else J["unlitHDRScale"] = *S++;
                J["rotationDelta"] = S[0];
                J["rotationTotal"] = S[1];
                J["size"] = Floats(S + 2, 2);
                J["scale"] = S[4];
                if (L.EmissiveRgb) J["pivot"] = Floats(S + 5, 2);
                if (L.VisState > 0x38) J["unknown"] = S[7];   // WWII's fifteenth float
                return J;
            };
            const int64_t P = O + 2 * L.VisState * I;
            Out.push_back(Json{ { "base", Part(P) }, { "amplitude", Part(P + L.VisState) } });
        }
        return Out;
    }

    // the extended definition at 0x118 (the tables' switch on the type): a trail's, a spark
    // fountain's, a spot light's; the others' are not written
    void Extended(const Reader& R, const Layout& L, const std::string& Type, uint64_t A, Json& E)
    {
        if (A == 0) return;
        if (Type == "trail")
        {
            const int64_t O = R.Fits(A, L.TrailVertCount + 0x1c);
            if (O < 0) return;
            Json T{ { "scrollTimeMsec", At<int32_t>(R.Img, O) }, { "repeatDist", At<int32_t>(R.Img, O + 4) },
                    { "invSplitDist", At<float>(R.Img, O + 8) }, { "invSplitArcDist", At<float>(R.Img, O + 0xc) },
                    { "invSplitTime", At<float>(R.Img, O + 0x10) } };
            if (L.TrailVertCount > 0x14)
            {
                T["headFadingFactor"] = At<float>(R.Img, O + 0x14);
                T["tailFadingFactor"] = At<float>(R.Img, O + 0x18);
            }
            const int32_t VertCount = At<int32_t>(R.Img, O + L.TrailVertCount), IndCount = At<int32_t>(R.Img, O + L.TrailVertCount + 0xc);
            T["vertCount"] = VertCount;
            T["indCount"] = IndCount;
            // the ribbon's profile and its index pairs, each vertex as pos[2], normal[2], texCoord:
            // Ghosts' FxTrailVertex as is (0x14), AW's and after's of 0x20 pos[2], normal[2], then
            // two texture coordinates (the first 0 on every one seen, the second around the
            // profile, as Ghosts' one) and a unit vector
            const int Stride = L.TrailVertCount > 0x14 ? 0x20 : 0x14;
            if (VertCount > 0 && VertCount <= 256)
                if (const int64_t V = R.Fits(R.Pointer(O + L.TrailVertCount + 4), Stride * (int64_t)VertCount); V >= 0)
                {
                    Json Verts = Json::array();
                    for (int K = 0; K < VertCount; K++)
                    {
                        float F[6] = {};
                        std::memcpy(F, R.Img.data() + V + Stride * K, Stride > 0x14 ? 24 : 20);
                        if (Stride > 0x14) F[4] = F[5];
                        Verts.push_back(Floats(F, 5));
                    }
                    T["verts"] = std::move(Verts);
                }
            if (IndCount > 0 && IndCount <= 1024)
                if (const int64_t I = R.Fits(R.Pointer(O + L.TrailVertCount + 0x14), 2 * (int64_t)IndCount); I >= 0)
                {
                    Json Inds = Json::array();
                    for (int K = 0; K < IndCount; K++) Inds.push_back(At<uint16_t>(R.Img, I + 2 * K));
                    T["inds"] = std::move(Inds);
                }
            E["trailDef"] = std::move(T);
        }
        else if (Type == "sparkFountain")
        {
            const int64_t O = R.Fits(A, 0x34);
            if (O < 0) return;
            static const char* const Names[] = { "gravity", "bounceFrac", "bounceRand", "sparkSpacing", "sparkLength", "sparkCount",
                                                 "loopTime", "velMin", "velMax", "velConeFrac", "restSpeed", "boostTime", "boostFactor" };
            Json S;
            for (int I = 0; I < 13; I++)
                S[Names[I]] = I == 5 ? Json(At<int32_t>(R.Img, O + 4 * I)) : Json(At<float>(R.Img, O + 4 * I));
            E["sparkFountainDef"] = std::move(S);
        }
        else if (Type == "spotLight")
        {
            // Ghosts: fovInnerFraction, startRadius, endRadius, brightness, maxLength, exponent;
            // AW/MWR: halfFovOuter, halfFovInner, radius, brightness, maxLength, exponent, nearClip
            const bool Aw = L.EmissiveRgb;
            const int64_t O = R.Fits(A, Aw ? 0x1c : 0x18);
            if (O < 0) return;
            static const char* const Ghosts[] = { "fovInnerFraction", "startRadius", "endRadius", "brightness", "maxLength" };
            static const char* const Later[] = { "halfFovOuter", "halfFovInner", "radius", "brightness", "maxLength" };
            Json S;
            for (int I = 0; I < 5; I++) S[(Aw ? Later : Ghosts)[I]] = At<float>(R.Img, O + 4 * I);
            S["exponent"] = At<int32_t>(R.Img, O + 0x14);
            if (Aw) S["nearClip"] = At<float>(R.Img, O + 0x18);
            E["spotLightDef"] = std::move(S);
        }
    }

    Json Element(const Reader& R, const Layout& L, int64_t O)
    {
        // WWII's 8 more bytes before the atlas (Layout::Inserted) move the rest
        ElemHead H;
        constexpr size_t Before = offsetof(ElemHead, AtlasBehavior);
        std::memcpy(&H, R.Img.data() + O, Before);
        std::memcpy(reinterpret_cast<uint8_t*>(&H) + Before, R.Img.data() + O + Before + L.Inserted, sizeof(H) - Before);
        const int64_t S = L.Inserted;
        Json J;
        const std::string Type = H.ElemType < L.Types.size() ? L.Types[H.ElemType] : std::to_string(H.ElemType);
        J["type"] = Type;
        J["flags"] = Bits(H.Flags, L.Flags);
        J["flagsRaw"] = H.Flags;
        J["flags2"] = H.Flags2;
        static const char* const Offsets[] = { "none", "sphere", "cylinder", "?" };
        J["spawnOffset"] = Offsets[(H.Flags >> 4) & 3];
        static const char* const Runs[] = { "world", "spawn", "effect", "offset", "camera", "?", "?", "?" };
        J["runRelativeTo"] = Runs[(H.Flags & L.RunMask) >> 6 & 7];
        J["litType"] = H.LitType;
        J["spawn"] = Range(H.Spawn);
        J["spawnRange"] = Range(H.SpawnRange);
        J["fadeInRange"] = Range(H.FadeInRange);
        J["fadeOutRange"] = Range(H.FadeOutRange);
        J["spawnFrustumCullRadius"] = H.SpawnFrustumCullRadius;
        J["spawnDelayMsec"] = Range(H.SpawnDelayMsec);
        J["lifeSpanMsec"] = Range(H.LifeSpanMsec);
        J["spawnOrigin"] = Ranges(H.SpawnOrigin);
        J["spawnOffsetRadius"] = Range(H.SpawnOffsetRadius);
        J["spawnOffsetHeight"] = Range(H.SpawnOffsetHeight);
        J["spawnAngles"] = Ranges(H.SpawnAngles);
        J["angularVelocity"] = Ranges(H.AngularVelocity);
        J["initialRotation"] = Range(H.InitialRotation);
        J["gravity"] = Range(H.Gravity);
        J["elasticity"] = Range(H.ReflectionFactor);
        if (L.Inserted) J["unknownRange"] = Range(At<FxFloatRange>(R.Img, O + Before));   // WWII: 0.5 on every element seen
        J["atlas"] = Json{ { "behavior", H.AtlasBehavior }, { "index", H.AtlasIndex }, { "fps", H.AtlasFps },
                           { "loopCount", H.AtlasLoopCount }, { "colIndexBits", H.AtlasColIndexBits },
                           { "rowIndexBits", H.AtlasRowIndexBits }, { "entryCount", H.AtlasEntryCount } };
        float B[6];
        std::memcpy(B, R.Img.data() + O + S + BoundsAt, sizeof(B));
        J["collision"] = Json{ { "midPoint", Floats(B, 3) }, { "halfSize", Floats(B + 3, 3) } };
        J["fxOnImpact"] = R.Name(R.Pointer(O + S + EffectsAt));
        J["fxOnDeath"] = R.Name(R.Pointer(O + S + EffectsAt + 8));
        J["fxEmitted"] = R.Name(R.Pointer(O + S + EffectsAt + 0x10));
        J["emitDist"] = Range(At<FxFloatRange>(R.Img, O + S + EmitDistAt));
        J["emitDistVariance"] = Range(At<FxFloatRange>(R.Img, O + S + EmitDistAt + 8));
        J["sortOrder"] = At<uint8_t>(R.Img, O + S + SortOrderAt);
        J["lightingFrac"] = At<uint8_t>(R.Img, O + S + SortOrderAt + 1);
        J["velocitySamples"] = VelocitySamples(R, L, R.Pointer(O + S + VelSamplesAt), H.VelIntervalCount + 1);
        J["visualSamples"] = VisualSamples(R, L, R.Pointer(O + S + VisSamplesAt), H.VisStateIntervalCount + 1);
        J["visuals"] = Visuals(R, L, Type, H.VisualCount, R.Pointer(O + S + VisualsAt));
        Extended(R, L, Type, R.Pointer(O + S + ExtendedAt), J);
        return J;
    }

    // the flags (zonetool's FxElemDefFlags; the masked ones - spawn offset, run, cloud shape - apart)
    std::vector<Layout::Bit> CommonFlags(uint32_t Collision, uint32_t CastShadow, uint32_t NoThermal, uint32_t Surface,
                                         uint32_t DynamicLight, uint32_t Volumetric)
    {
        std::vector<Layout::Bit> F = { { 0x2, "spawnRelative" }, { 0x4, "spawnFrustumCull" }, { 0x8, "runnerUsesRandRot" },
                                       { 0x200, "dieOnTouch" }, { 0x400, "drawPastFog" }, { 0x800, "drawWithViewModel" },
                                       { 0x1000, "blocksSight" }, { 0x2000, "drawInThermalViewOnly" },
                                       { 0x4000, "trailOrientByVelocity" }, { 0x8000, "emitOrientByElem" },
                                       { 0x10000, "useOcclusionQuery" }, { NoThermal, "noDrawInThermalView" },
                                       { Surface, "spawnImpactFxWithSurfaceName" }, { DynamicLight, "receiveDynamicLight" },
                                       { Volumetric, "volumetricTrail" }, { Collision, "useCollision" },
                                       { 0x1000000, "hasVelocityGraphLocal" }, { 0x2000000, "hasVelocityGraphWorld" },
                                       { 0x4000000, "hasGravity" }, { 0x8000000, "modelUsesPhysics" },
                                       { 0x10000000, "nonUniformScale" } };
        if (CastShadow) F.push_back({ CastShadow, "castShadow" });
        return F;
    }
}

const Layout& Ghosts()
{
    static const Layout L = [] {
        Layout L;
        L.Game = "ghosts";
        L.ElemSize = 0x138, L.VisState = 0x28, L.EmissiveRgb = false;
        L.DecalStride = 0x18, L.DecalMaterials = 3, L.TrailVertCount = 0x14;
        L.Types = { "billboardSprite", "orientedSprite", "tail", "trail", "flare", "particleSimAnimation", "apexTurbulence",
                    "nvidiaHair", "cloud", "sparkCloud", "sparkFountain", "model", "omniLight", "spotLight", "sound", "decal",
                    "runner", "vectorField" };
        L.Flags = CommonFlags(0x200000, 0, 0x20000, 0x40000, 0x80000, 0x100000);
        // IW6's own: the vector fields at 0x400000, the HDR scalar off at 0x800000
        L.Flags.push_back({ 0x400000, "useVectorFields" });
        L.Flags.push_back({ 0x800000, "noSurfaceHdrScalar" });
        return L;
    }();
    return L;
}

const Layout& ModernWarfareRemastered()
{
    static const Layout L = [] {
        Layout L;
        L.Game = "mwr";
        L.ElemSize = 0x140, L.VisState = 0x38, L.EmissiveRgb = true;
        L.DecalStride = 0x18, L.DecalMaterials = 3, L.TrailVertCount = 0x1c;
        L.Types = { "billboardSprite", "orientedSprite", "rotatedSprite", "tail", "line", "trail", "flare", "particleSimAnimation",
                    "cloud", "sparkCloud", "sparkFountain", "model", "omniLight", "spotLight", "sound", "decal", "runner",
                    "vectorField" };
        L.Flags = CommonFlags(0x400000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000);
        L.Flags.push_back({ 0x800000, "useVectorFields" });
        return L;
    }();
    return L;
}

const Layout& WorldWar2()
{
    static const Layout L = [] {
        Layout L = ModernWarfareRemastered();
        L.Game = "ww2";
        // the s2 tables (f4ac960, f4acc20): the elements at 0x40 of a 0x48 effect, an element of
        // 0x168 whose type is at 0xbc (8 bytes more before the atlas: a range, 0.5 on every element
        // of mp_carentan_s2), the velocity samples 0x94 (a float, 1.0, then the local and world
        // frames and a third), the visual states 0x3c (AW's and one more float, last); the types
        // as MWR's to the spot light, then sound 14 (aliases), decal 16 (the table's mark records),
        // runner 17 (effect names), vector field 18 (vf_ names), 15, 19 and 20 unnamed
        L.Elements = 0x40, L.ElemSize = 0x168, L.Inserted = 8, L.VelSample = 0x94, L.VelFirst = 4, L.VisState = 0x3c;
        L.Types = { "billboardSprite", "orientedSprite", "rotatedSprite", "tail", "line", "trail", "flare", "particleSimAnimation",
                    "cloud", "sparkCloud", "sparkFountain", "model", "omniLight", "spotLight", "sound", "15", "decal", "runner",
                    "vectorField", "19", "20" };
        return L;
    }();
    return L;
}

const Layout& AdvancedWarfare()
{
    static const Layout L = [] {
        Layout L = ModernWarfareRemastered();
        L.Game = "aw";
        L.DecalStride = 0x10, L.DecalMaterials = 2;   // the s1 table's mark visuals: two materials
        return L;
    }();
    return L;
}

bool ReadEffect(const Treyarch::EffectZone& Z, const Layout& L, int64_t Pos, const std::string& Name, std::string& Out)
{
    const Reader R(Z);
    const auto& Img = *Z.Image;
    // FxEffectDef (0x40): flags 8, totalSize 0xc, msecLoopingLife 0x10, the counts 0x14..0x1c,
    // elemMaxRadius 0x20, occlusion query 0x24..0x34, the elements at 0x38
    uint32_t Counts[3];
    for (int I = 0; I < 3; I++) Counts[I] = At<uint32_t>(Img, Pos + 0x14 + 4 * I);
    Json J;
    J["name"] = Name;
    J["game"] = L.Game;
    J["flags"] = At<uint32_t>(Img, Pos + 8);
    J["loopingLifeMsec"] = At<int32_t>(Img, Pos + 0x10);
    J["elemMaxRadius"] = At<float>(Img, Pos + 0x20);
    J["counts"] = Json{ { "looping", Counts[0] }, { "oneShot", Counts[1] }, { "emission", Counts[2] } };
    const uint64_t Total = (uint64_t)Counts[0] + Counts[1] + Counts[2];
    if (Total > 0x10000) return false;
    const uint64_t Elements = R.Pointer(Pos + L.Elements);
    const int64_t First = Total ? R.Fits(Elements, (int64_t)Total * L.ElemSize) : 0;
    if (First < 0) return false;
    Json List = Json::array();
    static const char* const Groups[] = { "looping", "oneShot", "emission" };
    int64_t Index = 0;
    for (int G = 0; G < 3; G++)
        for (uint32_t I = 0; I < Counts[G]; I++, Index++)
        {
            Json E{ { "group", Groups[G] }, { "index", I } };
            E.update(Element(R, L, First + Index * L.ElemSize));
            List.push_back(std::move(E));
        }
    J["elements"] = std::move(List);
    Out = J.dump(1, '\t', false, Json::error_handler_t::replace) + "\n";
    return true;
}
}

void IWZone::ExportsIwEffects(const IwEffects::Layout& L, std::function<XMaterial_t(uint64_t Address)> Read, int64_t TechsetAt)
{
    using namespace DiskGame;
    // a material through the game's reader, its techset's name added (the techniqueSet's first
    // field); NeedImage told of the images it names that another zone defines (",name"): the
    // image kind exports the images this zone holds, not those
    const auto Material = [this, Read, TechsetAt](std::function<void(const std::string&)> NeedImage) {
        return Treyarch::MaterialReader([this, Read, TechsetAt, NeedImage](uint64_t A) {
            XMaterial_t M = Read(A);
            if (const int64_t O = Off(A); O >= 0)
                if (const int64_t Set = Off(PointerAt(O + TechsetAt)); Set >= 0) M.TechsetName = StringAt(PointerAt(Set));
            if (NeedImage)
                for (const auto& I : M.Images)
                {
                    if (I.ImageName.size() > 1 && I.ImageName[0] == ',' && I.ImageName[1] != '$') NeedImage(I.ImageName.substr(1));
                    // a name-only material's, named by the common zones' book (AW): the image too
                    else if (I.ImagePtr == ExportWriters::ImageElsewhere && !I.ImageName.empty() && I.ImageName[0] != '$')
                        NeedImage(I.ImageName);
                }
            return M;
        }, [this](uint64_t A) { return Off(A); });
    };
    // the install index lists the zone's materials (an effect's, pulled from where they are)
    ExportsMaterials("material", Treyarch::MaterialKind(Material(nullptr)));
    Exports(Kind::Fx, [this, &L, Material](const Context& C, Totals& T) {
        auto Z = EffectView();
        // with the images: each material's, for the preview to find among them; a material or
        // an image of another zone pulled from there
        Z.ReadMaterials(C, Material([&C](const std::string& N) { C.Needs(Kind::Image, N); }));
        ExportEffects(C, T, "fx", [&](int64_t Pos, const std::string& N, std::string& J) { return IwEffects::ReadEffect(Z, L, Pos, N, J); });
    });
}
