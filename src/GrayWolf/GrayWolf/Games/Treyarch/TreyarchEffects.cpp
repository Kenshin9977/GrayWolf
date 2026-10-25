#include "stdafx.h"

#include "TreyarchEffects.h"

#include "CoDAssetType.h"
#include "DiskGame.h"
#include "ExportNaming.h"
#include "ZonekitZone.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <algorithm>
#include <cstring>
#include <iterator>

namespace Treyarch
{
namespace
{
    using Json = nlohmann::ordered_json;

    // ---- the structs, as Greyhound's CoDEffectTranslator read them from the games' memory
#pragma pack(push, 1)
    struct FxIntRange { int32_t Base, Amplitude; };
    struct FxFloatRange { float Base, Amplitude; };
    struct FxElemAtlas
    {
        uint8_t Behavior, Index, Fps, LoopCount, ColIndexBits, RowIndexBits;
        uint16_t EntryCountAndIndexRange;
    };

    // Black Ops 1 and 2 (0x124, 32-bit pointers); with 64-bit ones, what the Infinity Ward line's
    // elements are read into (BlackOpsOf)
    template <class Pointer>
    struct Bo1ElemOf
    {
        uint32_t Flags;
        FxIntRange Spawn;
        FxFloatRange SpawnRange, FadeInRange, FadeOutRange;
        float SpawnFrustumCullRadius;
        FxIntRange SpawnDelayMsec, LifeSpanMsec;
        FxFloatRange SpawnOrigin[3], SpawnOffsetRadius, SpawnOffsetHeight, SpawnAngles[3], AngularVelocity[3], InitialRotation;
        uint32_t RotationAxis;
        FxFloatRange Gravity, ReflectionFactor;
        FxElemAtlas Atlas;
        float WindInfluence;
        uint8_t ElemType, VisualCount, VelIntervalCount, VisStateIntervalCount;
        Pointer VelSamples, VisSamples, Visuals;
        float CollMins[3], CollMaxs[3];
        Pointer EffectOnImpact, EffectOnDeath, EffectEmitted;
        FxFloatRange EmitDist, EmitDistVariance;
        Pointer EffectAttached, TrailDef;
        uint8_t SortOrder, LightingFrac, Unused[2];
        uint16_t AlphaFadeTimeMsec, MaxWindStrength, SpawnIntervalAtMaxWind, LifespanAtMaxWind;
        FxFloatRange Billboard;   // trim, or for a cloud the density (an int range)
        Pointer SpawnSound;
        float BillboardPivot[2];
    };
    using Bo1Elem = Bo1ElemOf<uint32_t>;
    static_assert(sizeof(Bo1Elem) == 0x124, "BO1/BO2 FxElemDef is 0x124");

    // Call of Duty 4 (0xfc, 32-bit pointers; the order of OAT's IW3_Assets.h)
    struct Iw3Elem
    {
        uint32_t Flags;
        FxIntRange Spawn;
        FxFloatRange SpawnRange, FadeInRange, FadeOutRange;
        float SpawnFrustumCullRadius;
        FxIntRange SpawnDelayMsec, LifeSpanMsec;
        FxFloatRange SpawnOrigin[3], SpawnOffsetRadius, SpawnOffsetHeight, SpawnAngles[3], AngularVelocity[3], InitialRotation;
        FxFloatRange Gravity, ReflectionFactor;
        FxElemAtlas Atlas;
        uint8_t ElemType, VisualCount, VelIntervalCount, VisStateIntervalCount;
        uint32_t VelSamples, VisSamples, Visuals;
        float CollMins[3], CollMaxs[3];
        uint32_t EffectOnImpact, EffectOnDeath, EffectEmitted;
        FxFloatRange EmitDist, EmitDistVariance;
        uint32_t TrailDef;
        uint8_t SortOrder, LightingFrac, UseItemClip, Unused;
    };
    static_assert(sizeof(Iw3Elem) == 0xfc, "CoD4 FxElemDef is 0xfc");

    // World at War (0x100, 32-bit pointers; OAT's T4_Assets.h): CoD4's with Black Ops' wind
    // influence after the atlas
    struct T4Elem
    {
        uint32_t Flags;
        FxIntRange Spawn;
        FxFloatRange SpawnRange, FadeInRange, FadeOutRange;
        float SpawnFrustumCullRadius;
        FxIntRange SpawnDelayMsec, LifeSpanMsec;
        FxFloatRange SpawnOrigin[3], SpawnOffsetRadius, SpawnOffsetHeight, SpawnAngles[3], AngularVelocity[3], InitialRotation;
        FxFloatRange Gravity, ReflectionFactor;
        FxElemAtlas Atlas;
        float WindInfluence;
        uint8_t ElemType, VisualCount, VelIntervalCount, VisStateIntervalCount;
        uint32_t VelSamples, VisSamples, Visuals;
        float CollMins[3], CollMaxs[3];
        uint32_t EffectOnImpact, EffectOnDeath, EffectEmitted;
        FxFloatRange EmitDist, EmitDistVariance;
        uint32_t TrailDef;
        uint8_t SortOrder, LightingFrac, UseItemClip, Unused;
    };
    static_assert(sizeof(T4Elem) == 0x100, "World at War FxElemDef is 0x100");

    // Modern Warfare 2 and 3, the 64-bit rebuild (0x120; OAT's IW4/IW5_Assets.h, 8-byte pointers):
    // CoD4's with the collision box as centre and half size, an extended def (trail or spark
    // fountain) in the trail's place, and MW3's random seed where MW2 pads
    struct Iw4Elem
    {
        uint32_t Flags;
        FxIntRange Spawn;
        FxFloatRange SpawnRange, FadeInRange, FadeOutRange;
        float SpawnFrustumCullRadius;
        FxIntRange SpawnDelayMsec, LifeSpanMsec;
        FxFloatRange SpawnOrigin[3], SpawnOffsetRadius, SpawnOffsetHeight, SpawnAngles[3], AngularVelocity[3], InitialRotation;
        FxFloatRange Gravity, ReflectionFactor;
        FxElemAtlas Atlas;
        uint8_t ElemType, VisualCount, VelIntervalCount, VisStateIntervalCount;
        uint32_t Pad;
        uint64_t VelSamples, VisSamples, Visuals;
        float CollMid[3], CollHalf[3];
        uint64_t EffectOnImpact, EffectOnDeath, EffectEmitted;
        FxFloatRange EmitDist, EmitDistVariance;
        uint64_t Extended;
        uint8_t SortOrder, LightingFrac, UseItemClip, FadeInfo;
        int32_t RandomSeed;
    };
    static_assert(sizeof(Iw4Elem) == 0x120, "MW2/MW3 FxElemDef is 0x120");

    // Black Ops 3 (0x260)
    struct Bo3Elem
    {
        uint32_t Flags, ExtraFlags;
        FxIntRange Spawn, SpawnLoopingSpawnCount;
        FxFloatRange SpawnRange, FadeInRange, FadeOutRange;
        float SpawnFrustumCullRadius;
        FxIntRange SpawnDelayMsec, LifeSpanMsec;
        FxFloatRange SpawnOrigin[3], SpawnOffsetRadius, SpawnOffsetHeight;
        float SpawnOffsetCylindricalAxis;
        FxFloatRange SpawnAngles[3], AngularVelocity[3], InitialRotation;
        uint32_t RotationAxis;
        FxFloatRange Gravity, ReflectionFactor;
        FxElemAtlas Atlas;
        float WindInfluence;
        uint8_t ElemType, VisualCount, VelIntervalCount, ParentMovementStateIntervalCount, AttractorStateIntervalCount,
            VisStateIntervalCount;
        uint16_t Padding;
        uint64_t VelSamples, ParentMovementSamples, VisSamples, AttractorSamples;
        uint64_t Visuals;   // or an inline lens flare / beam source name (0x40)
        uint8_t VisualsRest[0x38];
        uint8_t Unused1[0x40];
        float CollMins[3], CollMaxs[3];
        float AttractorLocalPosition[3];
        uint8_t Unused2[4];
        uint64_t EffectOnImpact, EffectOnDeath, EffectEmitted;
        FxFloatRange EmitDist, EmitDistVariance, EmitDensity;
        float EmitSizeForDensity;
        uint32_t Pad1;
        uint64_t EffectAttached;
        FxFloatRange AttachmentDensity;
        float AttachmentSizeForDensity;
        uint32_t Pad2;
        uint64_t Lighting;   // lens flare, spot light or trail def
        uint8_t Displacement, LightingFrac, Unused[2];
        uint16_t AlphaFadeTimeMsec, MaxWindStrength, SpawnIntervalAtMaxWind, LifespanAtMaxWind;
        FxFloatRange Billboard;
        uint32_t Pad3;
        uint64_t SpawnSound, SpawnSoundHash, FollowSound, FollowSoundHash;
        float BillboardPivot[2];
        float AlphaDissolve, ZFeather;
        uint32_t FallOffBeginAngle, FallOffEndAngle;
        uint8_t Unmapped[0x20];
    };
    static_assert(sizeof(Bo3Elem) == 0x260, "BO3 FxElemDef is 0x260");

    // Black Ops 4 (0x280): Black Ops III's fields, the pointers first and the bytes last. The
    // zonekit table (bo4 f2e188e0) reads the visuals at 0 and 0x40 (0x40 each: an asset, or the
    // list of them), the trail, light or lens flare at 0x80, the samples at 0x88-0xa8 by the
    // counts at 0x26f-0x273, and dispatches on the type at 0x264 and the visual count at 0x26e.
    // The rest is placed by Black Ops III's values: 305 effects of mp_common are both games'
    // (their name hashes match), 2312 elements whose values sit at these offsets (each field
    // 99% or more of its non-zero values). Effects and sounds are name hashes (64-bit FNV-1a of
    // the name, 60 bits kept), not pointers.
    struct Bo4Elem
    {
        uint8_t Visuals[0x40];        // an asset, a hash (runner) or the list of them
        uint8_t Visuals2[0x40];
        uint64_t Lighting;            // trail def (type 5), light description (8), lens flare (11)
        uint64_t VelSamples, ParentMovementSamples, VisSamples, AttractorSamples;
        uint64_t GraphSamples;        // 0x30 per interval (count 0x272): not Black Ops III's, not read
        uint64_t SpawnSound, FollowSound;
        FxIntRange Billboard;         // a cloud's density
        uint64_t EffectOnImpact, Pad0, EffectOnDeath, Pad1, EffectEmitted, Pad2, EffectAttached;
        uint8_t Unmapped0[8];
        FxIntRange Spawn, SpawnLoopingSpawnCount;
        uint32_t Flags, ExtraFlags;
        FxIntRange SpawnDelayMsec, LifeSpanMsec;
        FxFloatRange SpawnRange, FadeInRange, FadeOutRange;
        FxFloatRange SpawnOrigin[3], SpawnOffsetRadius, SpawnOffsetHeight, SpawnAngles[3], AngularVelocity[3], InitialRotation;
        FxFloatRange Gravity, ReflectionFactor;
        FxFloatRange EmitDist, EmitDistVariance, EmitDensity, AttachmentDensity;
        float CollMins[3], CollMaxs[3];
        uint8_t Unmapped1[0x24];
        float BillboardPivot[2];
        float AlphaDissolve, ZFeather;
        uint32_t RotationAxis;
        uint32_t FallOffBeginAngle, FallOffEndAngle;
        uint32_t Unmapped2;
        float SpawnFrustumCullRadius;
        float EmitSizeForDensity, AttachmentSizeForDensity;
        uint8_t Unmapped3[0x1c];
        uint16_t AlphaFadeTimeMsec, MaxWindStrength, SpawnIntervalAtMaxWind, LifespanAtMaxWind;
        uint8_t ElemType;
        FxElemAtlas Atlas;
        uint8_t Unmapped4;
        uint8_t VisualCount, VelIntervalCount, ParentMovementStateIntervalCount, AttractorStateIntervalCount,
            GraphIntervalCount, VisStateIntervalCount;
        uint8_t Displacement, LightingFrac;
        uint8_t Unmapped5[10];
    };
    static_assert(sizeof(Bo4Elem) == 0x280, "BO4 FxElemDef is 0x280");
    static_assert(offsetof(Bo4Elem, Lighting) == 0x80 && offsetof(Bo4Elem, VelSamples) == 0x88 &&
                  offsetof(Bo4Elem, VisSamples) == 0x98 && offsetof(Bo4Elem, GraphSamples) == 0xa8 &&
                  offsetof(Bo4Elem, ElemType) == 0x264 && offsetof(Bo4Elem, VisualCount) == 0x26e &&
                  offsetof(Bo4Elem, VelIntervalCount) == 0x26f && offsetof(Bo4Elem, VisStateIntervalCount) == 0x273, "BO4 element (table)");
    static_assert(offsetof(Bo4Elem, SpawnSound) == 0xb0 && offsetof(Bo4Elem, EffectOnImpact) == 0xc8 &&
                  offsetof(Bo4Elem, EffectEmitted) == 0xe8 && offsetof(Bo4Elem, EffectAttached) == 0xf8 &&
                  offsetof(Bo4Elem, Spawn) == 0x108 && offsetof(Bo4Elem, Flags) == 0x118 && offsetof(Bo4Elem, LifeSpanMsec) == 0x128 &&
                  offsetof(Bo4Elem, SpawnOrigin) == 0x148 && offsetof(Bo4Elem, SpawnAngles) == 0x170 &&
                  offsetof(Bo4Elem, InitialRotation) == 0x1a0 && offsetof(Bo4Elem, EmitDist) == 0x1b8 &&
                  offsetof(Bo4Elem, CollMins) == 0x1d8 && offsetof(Bo4Elem, BillboardPivot) == 0x214 &&
                  offsetof(Bo4Elem, RotationAxis) == 0x224 && offsetof(Bo4Elem, SpawnFrustumCullRadius) == 0x234 &&
                  offsetof(Bo4Elem, AlphaFadeTimeMsec) == 0x25c && offsetof(Bo4Elem, Atlas) == 0x265 &&
                  offsetof(Bo4Elem, Displacement) == 0x274, "BO4 element (Black Ops III's values)");
#pragma pack(pop)
    // where the zonekit tables (t6 f18b4a0, t7 f13fb7d0) read the element's pointers
    // where the iw3 table (f79ca0) reads CoD4's: the samples at 0xb4/0xb8 by the counts at
    // 0xb2/0xb3, the visuals at 0xbc, the effect names at 0xd8, the trail at 0xf4
    static_assert(offsetof(Iw3Elem, VelIntervalCount) == 0xb2 && offsetof(Iw3Elem, VelSamples) == 0xb4 &&
                  offsetof(Iw3Elem, VisSamples) == 0xb8 && offsetof(Iw3Elem, Visuals) == 0xbc &&
                  offsetof(Iw3Elem, EffectOnImpact) == 0xd8 && offsetof(Iw3Elem, TrailDef) == 0xf4, "CoD4 element");
    // where the t4 table (f81b60) reads World at War's: the samples at 0xb8/0xbc by the counts at
    // 0xb6/0xb7, the visuals at 0xc0 (by the type at 0xb4, 10 a decal), the effect names at 0xdc,
    // the trail at 0xf8
    static_assert(offsetof(T4Elem, ElemType) == 0xb4 && offsetof(T4Elem, VelIntervalCount) == 0xb6 &&
                  offsetof(T4Elem, VelSamples) == 0xb8 && offsetof(T4Elem, VisSamples) == 0xbc &&
                  offsetof(T4Elem, Visuals) == 0xc0 && offsetof(T4Elem, EffectOnImpact) == 0xdc &&
                  offsetof(T4Elem, TrailDef) == 0xf8, "World at War element");
    // where the iw4 (fdcbd0) and iw5 (f90b60) tables read MW2's and MW3's: the samples at 0xb8/0xc0
    // by the counts at 0xb2/0xb3, the visuals at 0xc8 (by the type at 0xb0, 11 a decal), the
    // effect names at 0xe8, the extended def at 0x110
    static_assert(offsetof(Iw4Elem, ElemType) == 0xb0 && offsetof(Iw4Elem, VelIntervalCount) == 0xb2 &&
                  offsetof(Iw4Elem, VelSamples) == 0xb8 && offsetof(Iw4Elem, VisSamples) == 0xc0 &&
                  offsetof(Iw4Elem, Visuals) == 0xc8 && offsetof(Iw4Elem, EffectOnImpact) == 0xe8 &&
                  offsetof(Iw4Elem, Extended) == 0x110, "MW2/MW3 element");
    static_assert(offsetof(Bo1Elem, ElemType) == 0xb8 && offsetof(Bo1Elem, VelSamples) == 0xbc &&
                  offsetof(Bo1Elem, Visuals) == 0xc4 && offsetof(Bo1Elem, EffectOnImpact) == 0xe0 &&
                  offsetof(Bo1Elem, EffectAttached) == 0xfc && offsetof(Bo1Elem, SpawnSound) == 0x118, "BO1/BO2 element");
    static_assert(offsetof(Bo3Elem, ElemType) == 0xc8 && offsetof(Bo3Elem, VelSamples) == 0xd0 &&
                  offsetof(Bo3Elem, VisSamples) == 0xe0 && offsetof(Bo3Elem, Visuals) == 0xf0 &&
                  offsetof(Bo3Elem, EffectOnImpact) == 0x198 && offsetof(Bo3Elem, EffectAttached) == 0x1d0 &&
                  offsetof(Bo3Elem, Lighting) == 0x1e8 && offsetof(Bo3Elem, SpawnSound) == 0x208 &&
                  offsetof(Bo3Elem, FollowSound) == 0x218, "BO3 element");

    const char* const Bo1Types[] = { "billboardSprite", "orientedSprite", "rotatedSprite", "tail", "line", "trail", "cloud",
                                     "model", "omniLight", "spotLight", "sound", "decal", "runner" };
    const char* const Bo3Types[] = { "billboardSprite", "orientedSprite", "rotatedSprite", "tail", "line", "trail",
                                     "cloud", "model", "dynamicLight", "omniLight", "spotLight", "dynamicSound",
                                     "lensFlare", "decal", "runner", "beamSource", "beamTarget" };

    // the single-bit flags, by bit (the masked ones - spawn offset, run, orientation - apart)
    struct Bit { uint32_t Mask; const char* Name; };
    const Bit Bo1Flags[] = { { 0x2, "spawnRelative" }, { 0x4, "spawnFrustumCull" }, { 0x8, "runnerUsesRandRot" },
                             { 0x100, "useCollision" }, { 0x200, "dieOnTouch" }, { 0x400, "drawPastFog" },
                             { 0x800, "drawWithViewModel" }, { 0x1000, "blocksSight" }, { 0x2000, "useItemClip" },
                             { 0x80000, "useWorldUp" }, { 0x100000, "alignViewpoint" }, { 0x200000, "useBillboardPivot" },
                             { 0x400000, "useGaussianCloud" }, { 0x800000, "useRotationAxis" },
                             { 0x1000000, "hasVelocityGraphLocal" }, { 0x2000000, "hasVelocityGraphWorld" },
                             { 0x4000000, "hasGravity" }, { 0x8000000, "modelUsesPhysics" }, { 0x10000000, "nonUniformScale" },
                             { 0x20000000, "flameChunk" }, { 0x40000000, "hasReflection" }, { 0x80000000, "isMatureContent" } };
    const Bit Bo3Flags[] = { { 0x2, "spawnRelative" }, { 0x4, "spawnFrustumCull" }, { 0x8, "runnerUsesRandRot" },
                             { 0x200, "useCollision" }, { 0x400, "dieOnTouch" }, { 0x800, "drawPastFog" },
                             { 0x1000, "drawWithViewModel" }, { 0x2000, "blocksSight" }, { 0x4000, "useItemClip" },
                             { 0x8000, "useOcclusionQuery" }, { 0x10000, "inheritParentMovement" }, { 0x20000, "useThermal" },
                             { 0x40000, "attractorGraphEnable" }, { 0x80000, "useWorldUp" }, { 0x100000, "alignViewpoint" },
                             { 0x200000, "useBillboardPivot" }, { 0x400000, "useGaussianCloud" }, { 0x800000, "useRotationAxis" },
                             { 0x8000000, "modelUsesPhysics" }, { 0x10000000, "nonUniformScale" },
                             { 0x20000000, "noComputeSprites" }, { 0x40000000, "spawnDynEnt" }, { 0x80000000, "isMatureContent" } };
    const Bit Bo3Extra[] = { { 0x1, "distribX" }, { 0x2, "distribY" }, { 0x4, "distribZ" }, { 0x8, "teamFriendly" },
                             { 0x10, "teamFoe" }, { 0x20, "castShadow" }, { 0x40, "underwaterOnly" }, { 0x80, "overwaterOnly" },
                             { 0x100, "waterRelaxedCull" }, { 0x400, "gameplayIntensive" },
                             { 0x800, "useSizeBasedDensityEmission" }, { 0x1000, "useSizeBasedDensityAttachment" },
                             { 0x2000, "lfOccludeGeometry" }, { 0x4000, "lfOccludeParticles" },
                             { 0x8000, "respectExclusionVolumes" }, { 0x10000, "noGpuFx" }, { 0x20000, "indoor" },
                             { 0x80000, "useGlobalTint" }, { 0x100000, "useTrailTrapezoidFix" } };

    template <size_t N>
    Json Bits(uint32_t V, const Bit (&Table)[N])
    {
        Json Out = Json::array();
        for (const Bit& B : Table)
            if (V & B.Mask) Out.push_back(B.Name);
        return Out;
    }

    Json Range(const FxFloatRange& R) { return Json::array({ R.Base, R.Amplitude }); }
    Json Range(const FxIntRange& R) { return Json::array({ R.Base, R.Amplitude }); }
    Json Ranges(const FxFloatRange* R) { return Json::array({ Range(R[0]), Range(R[1]), Range(R[2]) }); }
    Json Vec(const float* V, int N)
    {
        Json Out = Json::array();
        for (int I = 0; I < N; I++) Out.push_back(V[I]);
        return Out;
    }

    // the Infinity Ward line: CoD4's element, World at War's, MW2's and MW3's
    bool IwLine(EffectLayout L)
    {
        return L == EffectLayout::Iw3 || L == EffectLayout::T4 || L == EffectLayout::Iw4 || L == EffectLayout::Iw5;
    }

    class Reader
    {
    public:
        Reader(const EffectZone& Z, EffectLayout L)
            : Z(Z), Ptr(L == EffectLayout::Bo1 || L == EffectLayout::Bo2 || L == EffectLayout::Iw3 || L == EffectLayout::T4 || L == EffectLayout::Codol ? 4 : 8) {}

        template <class T>
        bool Struct(uint64_t Address, T& Out) const
        {
            const int64_t O = Z.Off(Address);
            if (O < 0 || O + (int64_t)sizeof(T) > (int64_t)Z.Image->size()) return false;
            std::memcpy(&Out, Z.Image->data() + O, sizeof(T));
            return true;
        }
        uint64_t PointerAt(int64_t Pos) const
        {
            return Ptr == 8 ? ExportNaming::At<uint64_t>(*Z.Image, Pos) : ExportNaming::At<uint32_t>(*Z.Image, Pos);
        }
        // the asset an address points at, by the name its first field points to (a name-only
        // header's ",name" as name)
        std::string Asset(uint64_t Address) const { return Z.Asset(Address, Ptr); }
        // a sound or an effect: its name where the zone holds the pointer as the name's text (an
        // effect handle, an alias), else the asset it points at
        std::string Named(uint64_t Address) const
        {
            std::string S = Z.String(Address);
            bool Text = S.size() >= 3;
            for (char C : S)
                Text = Text && ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_' ||
                                C == '/' || C == '\\' || C == '.' || C == '-');
            return Text ? S : Asset(Address);
        }
        std::string Text(const uint8_t* Bytes, size_t Max) const
        {
            size_t N = 0;
            while (N < Max && Bytes[N]) N++;
            return std::string(reinterpret_cast<const char*>(Bytes), N);
        }

        const EffectZone& Z;
        const int Ptr;
    };

    // a material's techset, images and blend factors, as the fx and material kinds write them
    Json MaterialFields(const EffectZone::MaterialInfo& M)
    {
        Json V;
        V["techset"] = M.Techset;
        Json List = Json::array();
        for (const auto& I : M.Images)
            List.push_back(Json{ { "usage", I.Usage }, { "semantic", I.Semantic }, { "name", I.Name } });
        V["images"] = std::move(List);
        if (M.BlendSrc >= 0) V["blend"] = Json::array({ M.BlendSrc, M.BlendDst });
        // the constant blocks, by technique (an empty one: none), when any holds something
        if (std::any_of(M.Constants.begin(), M.Constants.end(), [](const auto& B) { return !B.empty(); }))
        {
            Json C = Json::array();
            for (const auto& Block : M.Constants) C.push_back(Block);
            V["constants"] = std::move(C);
        }
        return V;
    }

    // What an element of a type draws or plays, by name: count 1 is the pointer itself, more an
    // array of them (BO3: 0x40 per visual)
    Json Visuals(const Reader& R, EffectLayout L, uint8_t Type, uint8_t Count, uint64_t Visuals, const uint8_t* Inline)
    {
        const bool Bo3 = L == EffectLayout::Bo3;
        const char* Key = "material";
        if (Bo3)
        {
            switch (Type)
            {
            case 7: Key = "model"; break;
            case 11: Key = "sound"; break;
            case 13: Key = "decal"; break;
            case 14: Key = "effect"; break;
            case 15: return Json::array({ Json{ { "beam", R.Text(Inline, 0x40) } } });
            case 8: case 9: case 10: case 12: case 16: return Json::array();
            default: if (Type > 6) return Json::array();
            }
        }
        else
        {
            switch (Type)
            {
            case 7: Key = "model"; break;
            case 10: Key = "sound"; break;
            case 11: Key = "decal"; break;
            case 12: Key = "effect"; break;
            case 8: case 9: return Json::array();
            default: if (Type > 6) return Json::array();
            }
        }
        Json Out = Json::array();
        const std::string K = Key;
        const auto Named = [&](uint64_t A) { return K == "sound" || K == "effect" ? R.Named(A) : R.Asset(A); };
        // a material visual with its techset, images and blending, when the zone's reader gives them
        const auto Visual = [&](const char* VKey, const std::string& N, uint64_t A) {
            Json V{ { VKey, N } };
            // defined in another zone (a name-only header here): pulled from there
            if (std::strcmp(VKey, "material") == 0) R.Z.AddMaterial(V, N, A);
            return V;
        };
        // a decal's always a list of mark visuals, each its materials (BO3 three, BO1/2 two): the
        // first of each named
        if (K == "decal")
        {
            const int64_t Array = R.Z.Off(Visuals);
            const int64_t Stride = Bo3 ? 0x18 : 2 * R.Ptr;
            for (int I = 0; Array >= 0 && I < Count; I++)
            {
                const uint64_t A = R.PointerAt(Array + I * Stride);
                if (std::string N = R.Asset(A); !N.empty()) Out.push_back(Visual("material", N, A));
            }
            return Out;
        }
        if (Count == 1)
        {
            if (std::string N = Named(Visuals); !N.empty()) Out.push_back(Visual(Key, N, Visuals));
            return Out;
        }
        const int64_t Array = R.Z.Off(Visuals);
        const int64_t Stride = Bo3 ? 0x40 : R.Ptr;
        for (int I = 0; Array >= 0 && I < Count; I++)
        {
            const uint64_t A = R.PointerAt(Array + I * Stride);
            if (std::string N = Named(A); !N.empty()) Out.push_back(Visual(Key, N, A));
        }
        return Out;
    }

    // velocity samples (0x60 each: local velocity and total delta, then world)
    Json VelocitySamples(const Reader& R, uint64_t At, int Count)
    {
        Json Out = Json::array();
        struct Sample { float V[24]; } S{};
        for (int I = 0; I < Count && R.Struct(At + 0x60ull * I, S); I++)
        {
            const auto Part = [&](int K) {
                return Json{ { "velocity", { { "base", Vec(S.V + K, 3) }, { "amplitude", Vec(S.V + K + 3, 3) } } },
                             { "totalDelta", { { "base", Vec(S.V + K + 6, 3) }, { "amplitude", Vec(S.V + K + 9, 3) } } } };
            };
            Out.push_back(Json{ { "local", Part(0) }, { "world", Part(12) } });
        }
        return Out;
    }

    // visual state samples: base then amplitude, each a colour (RGBA bytes), rotation delta and
    // total, size (2), scale, and on BO3 light intensity, radius, fov and child scale (a state of
    // 0x18 bytes, BO3 0x28: the loaders read 48 and 80 bytes a sample). Black Ops stores its colour
    // blue first (a D3D colour: its blood reads 34, 34, 130), written red first as the others
    // (Call of Duty Online: 0x20, two floats more, last)
    Json VisualSamples(const Reader& R, uint64_t At, int Count, bool Bo3, bool Bgra, bool Codol = false)
    {
        Json Out = Json::array();
        const size_t State = Bo3 ? 0x28 : Codol ? 0x20 : 0x18;
        uint8_t B[0x50] = {};
        for (int I = 0; I < Count; I++)
        {
            const int64_t O = R.Z.Off(At + 2 * State * I);
            if (O < 0 || O + (int64_t)(2 * State) > (int64_t)R.Z.Image->size()) break;
            std::memcpy(B, R.Z.Image->data() + O, 2 * State);
            const auto Part = [&](size_t P) {
                float F[9] = {};
                std::memcpy(F, B + P + 4, State - 4);
                Json J{ { "color", Json::array({ B[P + (Bgra ? 2 : 0)], B[P + 1], B[P + (Bgra ? 0 : 2)], B[P + 3] }) }, { "rotationDelta", F[0] },
                        { "rotationTotal", F[1] }, { "size", Json::array({ F[2], F[3] }) }, { "scale", F[4] } };
                if (Bo3)
                {
                    J["lightIntensity"] = F[5];
                    J["lightRadius"] = F[6];
                    J["lightFov"] = F[7];
                    J["childScale"] = F[8];
                }
                if (Codol) J["unknown"] = Json::array({ F[5], F[6] });
                return J;
            };
            Out.push_back(Json{ { "base", Part(0) }, { "amplitude", Part(State) } });
        }
        return Out;
    }

    // what both layouts share, from a struct of either
    template <class E>
    Json Common(const Reader& R, EffectLayout L, const E& E1)
    {
        const bool Bo3 = L == EffectLayout::Bo3;
        Json J;
        const uint8_t Type = E1.ElemType;
        const auto& Names = Bo3 ? Bo3Types : Bo1Types;
        const size_t Known = Bo3 ? std::size(Bo3Types) : std::size(Bo1Types);
        J["type"] = Type < Known ? Json(Names[Type]) : Json(std::to_string(Type));
        J["flags"] = Bo3 ? Bits(E1.Flags, Bo3Flags) : Bits(E1.Flags, Bo1Flags);
        static const char* const Offsets[] = { "none", "sphere", "cylinder", "?" };
        J["spawnOffset"] = Offsets[(E1.Flags >> 4) & 3];
        static const char* const Runs[] = { "world", "spawn", "effect", "offset", "effectNow", "?", "?", "?" };
        J["runRelativeTo"] = Runs[Bo3 ? (E1.Flags >> 6) & 7 : (E1.Flags >> 6) & 3];
        J["spawn"] = Range(E1.Spawn);
        J["spawnRange"] = Range(E1.SpawnRange);
        J["fadeInRange"] = Range(E1.FadeInRange);
        J["fadeOutRange"] = Range(E1.FadeOutRange);
        J["spawnFrustumCullRadius"] = E1.SpawnFrustumCullRadius;
        J["spawnDelayMsec"] = Range(E1.SpawnDelayMsec);
        J["lifeSpanMsec"] = Range(E1.LifeSpanMsec);
        J["spawnOrigin"] = Ranges(E1.SpawnOrigin);
        J["spawnOffsetRadius"] = Range(E1.SpawnOffsetRadius);
        J["spawnOffsetHeight"] = Range(E1.SpawnOffsetHeight);
        J["spawnAngles"] = Ranges(E1.SpawnAngles);
        J["angularVelocity"] = Ranges(E1.AngularVelocity);
        J["initialRotation"] = Range(E1.InitialRotation);
        J["rotationAxisPacked"] = E1.RotationAxis;
        J["gravity"] = Range(E1.Gravity);
        J["elasticity"] = Range(E1.ReflectionFactor);
        J["windInfluence"] = E1.WindInfluence;
        J["atlas"] = Json{ { "behavior", E1.Atlas.Behavior }, { "index", E1.Atlas.Index }, { "fps", E1.Atlas.Fps },
                           { "loopCount", E1.Atlas.LoopCount }, { "colIndexBits", E1.Atlas.ColIndexBits },
                           { "rowIndexBits", E1.Atlas.RowIndexBits },
                           { "entryCount", 1 << ((E1.Atlas.ColIndexBits + E1.Atlas.RowIndexBits) & 15) } };
        J["collision"] = Json{ { "mins", Vec(E1.CollMins, 3) }, { "maxs", Vec(E1.CollMaxs, 3) } };
        J["fxOnImpact"] = R.Named(E1.EffectOnImpact);
        J["fxOnDeath"] = R.Named(E1.EffectOnDeath);
        J["fxEmitted"] = R.Named(E1.EffectEmitted);
        J["emitDist"] = Range(E1.EmitDist);
        J["emitDistVariance"] = Range(E1.EmitDistVariance);
        J["fxAttached"] = R.Named(E1.EffectAttached);
        J["lightingFrac"] = E1.LightingFrac;
        J["alphaFadeTimeMsec"] = E1.AlphaFadeTimeMsec;
        J["maxWindStrength"] = E1.MaxWindStrength;
        J["spawnIntervalAtMaxWind"] = E1.SpawnIntervalAtMaxWind;
        J["lifespanAtMaxWind"] = E1.LifespanAtMaxWind;
        J["billboard"] = Range(E1.Billboard);
        J["billboardPivot"] = Vec(E1.BillboardPivot, 2);
        J["spawnSound"] = E1.SpawnSound ? R.Named(E1.SpawnSound) : std::string();
        J["velocitySamples"] = VelocitySamples(R, E1.VelSamples, E1.VelIntervalCount + 1);
        J["visualSamples"] = VisualSamples(R, E1.VisSamples, E1.VisStateIntervalCount + 1, Bo3,
                                         L == EffectLayout::Bo1 || IwLine(L) || L == EffectLayout::Codol,
                                         L == EffectLayout::Codol);
        return J;
    }

    // An Infinity Ward line element as Black Ops' (its fields where they are, its type renumbered
    // to Black Ops' by Types, a type past them 100 + its own) and the fields of Black Ops' it lacks
    template <class E>
    Bo1ElemOf<uint64_t> BlackOpsOf(const E& C, const uint8_t* Types, size_t TypeCount)
    {
        Bo1ElemOf<uint64_t> X{};
        X.Flags = C.Flags, X.Spawn = C.Spawn, X.SpawnRange = C.SpawnRange, X.FadeInRange = C.FadeInRange;
        X.FadeOutRange = C.FadeOutRange, X.SpawnFrustumCullRadius = C.SpawnFrustumCullRadius;
        X.SpawnDelayMsec = C.SpawnDelayMsec, X.LifeSpanMsec = C.LifeSpanMsec;
        for (int K = 0; K < 3; K++)
        {
            X.SpawnOrigin[K] = C.SpawnOrigin[K], X.SpawnAngles[K] = C.SpawnAngles[K], X.AngularVelocity[K] = C.AngularVelocity[K];
            if constexpr (requires { C.CollMid; })
                X.CollMins[K] = C.CollMid[K] - C.CollHalf[K], X.CollMaxs[K] = C.CollMid[K] + C.CollHalf[K];
            else
                X.CollMins[K] = C.CollMins[K], X.CollMaxs[K] = C.CollMaxs[K];
        }
        X.SpawnOffsetRadius = C.SpawnOffsetRadius, X.SpawnOffsetHeight = C.SpawnOffsetHeight;
        X.InitialRotation = C.InitialRotation, X.Gravity = C.Gravity, X.ReflectionFactor = C.ReflectionFactor;
        X.Atlas = C.Atlas;
        if constexpr (requires { C.WindInfluence; }) X.WindInfluence = C.WindInfluence;
        X.ElemType = C.ElemType < TypeCount ? Types[C.ElemType] : (uint8_t)(100 + C.ElemType);
        X.VisualCount = C.VisualCount, X.VelIntervalCount = C.VelIntervalCount, X.VisStateIntervalCount = C.VisStateIntervalCount;
        X.VelSamples = C.VelSamples, X.VisSamples = C.VisSamples, X.Visuals = C.Visuals;
        X.EffectOnImpact = C.EffectOnImpact, X.EffectOnDeath = C.EffectOnDeath, X.EffectEmitted = C.EffectEmitted;
        X.EmitDist = C.EmitDist, X.EmitDistVariance = C.EmitDistVariance;
        X.SortOrder = C.SortOrder, X.LightingFrac = C.LightingFrac;
        return X;
    }

    // A trail's profile (OAT's FxTrailVertex: pos[2], normal[2], texCoord) and its index pairs,
    // when the zone holds them
    void TrailProfile(const Reader& R, uint64_t Verts, int32_t VertCount, uint64_t Inds, int32_t IndCount, Json& T)
    {
        if (Verts && VertCount > 0 && VertCount <= 256)
        {
            Json V = Json::array();
            for (int K = 0; K < VertCount; K++)
            {
                float F[5];
                if (!R.Struct(Verts + 20 * (uint64_t)K, F)) return;
                V.push_back(Json::array({ F[0], F[1], F[2], F[3], F[4] }));
            }
            T["verts"] = std::move(V);
        }
        if (Inds && IndCount > 0 && IndCount <= 1024)
        {
            Json I = Json::array();
            for (int K = 0; K < IndCount; K++)
            {
                uint16_t N;
                if (!R.Struct(Inds + 2 * (uint64_t)K, N)) return;
                I.push_back(N);
            }
            T["inds"] = std::move(I);
        }
    }

    // An Infinity Ward line element's extended def at A by its own type (OAT's FxTrailDef,
    // FxSparkFountainDef, FxSpotLightDef), keyed as the 64-bit games' (Games/IW/IwEffects.cpp
    // Extended): CoD4's and World at War's trail (3) with an int split distance, MW2's and MW3's
    // with the inverse ones and their spark fountain (6), MW3's spot light (9)
    void IwExtended(const Reader& R, EffectLayout L, uint8_t Type, uint64_t A, Json& Out)
    {
        if (A == 0) return;
        const bool Iw4 = L == EffectLayout::Iw4 || L == EffectLayout::Iw5;
        if (Type == 3 && !Iw4)
        {
            // scrollTimeMsec, repeatDist, splitDist, vertCount, verts, indCount, inds
            int32_t T[7];
            if (!R.Struct(A, T)) return;
            Out["trailDef"] = Json{ { "scrollTimeMsec", T[0] }, { "repeatDist", T[1] }, { "splitDist", T[2] },
                                    { "vertCount", T[3] }, { "indCount", T[5] } };
            TrailProfile(R, (uint32_t)T[4], T[3], (uint32_t)T[6], T[5], Out["trailDef"]);
        }
        else if (Type == 3)
        {
            // ... invSplitTime, vertCount 0x14, verts 0x18, indCount 0x20, inds 0x28
            uint8_t B[0x30];
            if (!R.Struct(A, B)) return;
            const auto I = [&B](size_t P) { int32_t V; std::memcpy(&V, B + P, 4); return V; };
            const auto F = [&B](size_t P) { float V; std::memcpy(&V, B + P, 4); return V; };
            Out["trailDef"] = Json{ { "scrollTimeMsec", I(0) }, { "repeatDist", I(4) }, { "invSplitDist", F(8) },
                                    { "invSplitArcDist", F(0xc) }, { "invSplitTime", F(0x10) }, { "vertCount", I(0x14) },
                                    { "indCount", I(0x20) } };
            uint64_t Verts, Inds;
            std::memcpy(&Verts, B + 0x18, 8);
            std::memcpy(&Inds, B + 0x28, 8);
            TrailProfile(R, Verts, I(0x14), Inds, I(0x20), Out["trailDef"]);
        }
        else if (Type == 6 && Iw4)
        {
            float S[13];
            if (!R.Struct(A, S)) return;
            static const char* const Names[] = { "gravity", "bounceFrac", "bounceRand", "sparkSpacing", "sparkLength", "sparkCount",
                                                 "loopTime", "velMin", "velMax", "velConeFrac", "restSpeed", "boostTime", "boostFactor" };
            Json D;
            for (int K = 0; K < 13; K++)
            {
                int32_t N;
                std::memcpy(&N, &S[K], 4);
                D[Names[K]] = K == 5 ? Json(N) : Json(S[K]);
            }
            Out["sparkFountainDef"] = std::move(D);
        }
        else if (Type == 9 && L == EffectLayout::Iw5)
        {
            float S[6];
            if (!R.Struct(A, S)) return;
            static const char* const Names[] = { "fovInnerFraction", "startRadius", "endRadius", "brightness", "maxLength" };
            Json D;
            for (int K = 0; K < 5; K++) D[Names[K]] = S[K];
            int32_t Exponent;
            std::memcpy(&Exponent, &S[5], 4);
            D["exponent"] = Exponent;
            Out["spotLightDef"] = std::move(D);
        }
    }

    // An Infinity Ward line element (CoD4, World at War, MW2, MW3) at A as JSON, false when not
    // in the zone
    template <class E>
    bool IwElement(const Reader& R, EffectLayout L, uint64_t A, Json& Out)
    {
        E C{};
        if (!R.Struct(A, C)) return false;
        // their types as Black Ops' (billboard, oriented, tail, trail, cloud, model, omni light,
        // spot light, sound, decal, runner); World at War's spark cloud after the cloud, MW2's
        // and MW3's spark cloud and fountain: sprites, named as theirs
        static const uint8_t Iw3Types[] = { 0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12 };
        static const uint8_t T4Types[] = { 0, 1, 3, 5, 6, 0, 7, 8, 9, 10, 11, 12 };
        static const uint8_t Iw4Types[] = { 0, 1, 3, 5, 6, 0, 0, 7, 8, 9, 10, 11, 12 };
        const bool T4 = L == EffectLayout::T4, Iw3 = L == EffectLayout::Iw3;
        const uint8_t* Types = Iw3 ? Iw3Types : T4 ? T4Types : Iw4Types;
        const size_t Count = Iw3 ? std::size(Iw3Types) : T4 ? std::size(T4Types) : std::size(Iw4Types);
        const auto X = BlackOpsOf(C, Types, Count);
        Out.update(Common(R, L, X));
        if (C.ElemType == 5 && !Iw3) Out["type"] = "sparkCloud";
        if (C.ElemType == 6 && !Iw3 && !T4) Out["type"] = "sparkFountain";
        for (const char* Gone : { "rotationAxisPacked", "windInfluence", "fxAttached", "alphaFadeTimeMsec", "maxWindStrength",
                                  "spawnIntervalAtMaxWind", "lifespanAtMaxWind", "billboard", "billboardPivot", "spawnSound" })
            if (!T4 || std::strcmp(Gone, "windInfluence") != 0) Out.erase(Gone);   // not theirs
        Out["sortOrder"] = X.SortOrder;
        if constexpr (requires { C.FadeInfo; }) Out["fadeInfo"] = C.FadeInfo;
        if constexpr (requires { C.RandomSeed; })
            if (L == EffectLayout::Iw5) Out["randomSeed"] = C.RandomSeed;
        Out["visuals"] = Visuals(R, L, X.ElemType, X.VisualCount, X.Visuals, nullptr);
        // MW3's spot light draws its GfxLightDef (its name first)
        if (L == EffectLayout::Iw5 && C.ElemType == 9 && X.VisualCount == 1)
            if (std::string N = R.Asset(X.Visuals); !N.empty()) Out["visuals"] = Json::array({ Json{ { "lightDef", N } } });
        if constexpr (requires { C.Extended; })
            IwExtended(R, L, C.ElemType, C.Extended, Out);
        else
            IwExtended(R, L, C.ElemType, C.TrailDef, Out);
        return true;
    }
}

namespace
{
    // Black Ops 4's types, as Black Ops III's where its elements of a type are that type in
    // Black Ops III (billboard to model, light 8, sound 10, lens flare 11, decal 13, runner 14);
    // the others by number
    const char* const Bo4Types[] = { "billboardSprite", "orientedSprite", "rotatedSprite", "tail", "line", "trail", "cloud",
                                     "model", "dynamicLight", nullptr, "dynamicSound", "lensFlare", nullptr, "decal", "runner" };
    // its flags where Black Ops III's same element sets the same bit (bits 13, 15, 17-19, 24-26
    // and 29 not: "bit<n>")
    const Bit Bo4Flags[] = { { 0x2, "spawnRelative" }, { 0x4, "spawnFrustumCull" }, { 0x8, "runnerUsesRandRot" },
                             { 0x200, "useCollision" }, { 0x400, "dieOnTouch" }, { 0x800, "drawPastFog" },
                             { 0x1000, "drawWithViewModel" }, { 0x4000, "useItemClip" }, { 0x10000, "inheritParentMovement" },
                             { 0x100000, "alignViewpoint" }, { 0x200000, "useBillboardPivot" }, { 0x400000, "useGaussianCloud" },
                             { 0x800000, "useRotationAxis" }, { 0x8000000, "modelUsesPhysics" }, { 0x10000000, "nonUniformScale" },
                             { 0x40000000, "spawnDynEnt" }, { 0x80000000, "isMatureContent" } };
    // Black Ops III's extra flags 0-8, then 10-14 one bit up (the others "bit<n>")
    const Bit Bo4Extra[] = { { 0x1, "distribX" }, { 0x2, "distribY" }, { 0x4, "distribZ" }, { 0x8, "teamFriendly" },
                             { 0x10, "teamFoe" }, { 0x20, "castShadow" }, { 0x40, "underwaterOnly" }, { 0x80, "overwaterOnly" },
                             { 0x100, "waterRelaxedCull" }, { 0x800, "gameplayIntensive" },
                             { 0x1000, "useSizeBasedDensityEmission" }, { 0x2000, "useSizeBasedDensityAttachment" },
                             { 0x4000, "lfOccludeGeometry" }, { 0x8000, "lfOccludeParticles" } };

    // the named bits, then the others as "bit<n>"; Known: the masked fields' bits, not listed
    template <size_t N>
    Json AllBits(uint32_t V, const Bit (&Table)[N], uint32_t Known)
    {
        Json Out = Bits(V, Table);
        for (const Bit& B : Table) Known |= B.Mask;
        for (int I = 0; I < 32; I++)
            if ((V & ~Known) & (1u << I)) Out.push_back("bit" + std::to_string(I));
        return Out;
    }

    // visual state samples (0x34 a state, base then amplitude): colour (RGBA bytes), rotation
    // delta and total, size (2), scale (3: the first as Black Ops III's), light intensity, a
    // float not Black Ops III's, light radius, fov, child scale
    Json Bo4VisualSamples(const Reader& R, uint64_t At, int Count)
    {
        Json Out = Json::array();
        uint8_t B[0x68] = {};
        for (int I = 0; I < Count && R.Struct(At + sizeof(B) * I, B); I++)
        {
            const auto Part = [&](size_t P) {
                float F[12] = {};
                std::memcpy(F, B + P + 4, sizeof(F));
                return Json{ { "color", Json::array({ B[P], B[P + 1], B[P + 2], B[P + 3] }) }, { "rotationDelta", F[0] },
                             { "rotationTotal", F[1] }, { "size", Json::array({ F[2], F[3] }) }, { "scale", F[4] },
                             { "scale3", Json::array({ F[4], F[5], F[6] }) }, { "lightIntensity", F[7] },
                             { "lightRadius", F[9] }, { "lightFov", F[10] }, { "childScale", F[11] } };
            };
            Out.push_back(Json{ { "base", Part(0) }, { "amplitude", Part(0x34) } });
        }
        return Out;
    }

    // What a Black Ops 4 element draws (f2e18b20): one asset at the slot, or the slot's list of
    // Count (0x40 each, the asset first); a decal's list of 0x10 (two materials, the first
    // named); a runner's effect by its hash in place of the pointer
    Json Bo4Visuals(const Reader& R, const Bo4Elem& X)
    {
        Json Out = Json::array();
        const uint8_t Type = X.ElemType;
        uint64_t Slot;
        std::memcpy(&Slot, X.Visuals, 8);
        const bool Material = Type < 7 || Type == 12 || Type > 16;
        if (!Material && Type != 7 && Type != 13 && Type != 14) return Out;
        const auto Named = [&](uint64_t A, const char* Key) {
            // the asset's name hash at its start
            const int64_t O = R.Z.Off(A);
            if (O < 0 || !R.Z.Hashed) return;
            const bool IsMaterial = std::strcmp(Key, "material") == 0;
            Json V{ { Key, R.Z.Hashed(ExportNaming::At<uint64_t>(*R.Z.Image, O), IsMaterial ? "xmaterial" : "xmodel") } };
            EffectZone::MaterialInfo M;
            if (IsMaterial && R.Z.Material && R.Z.Material(A, M)) V.update(MaterialFields(M));
            Out.push_back(std::move(V));
        };
        const int64_t Array = R.Z.Off(Slot);
        if (Type == 14)
        {
            const auto Effect = [&](uint64_t H) {
                if (H != 0 && R.Z.Hashed) Out.push_back(Json{ { "effect", R.Z.Hashed(H, "fx") } });
            };
            if (X.VisualCount < 2) Effect(Slot);
            for (int I = 0; X.VisualCount >= 2 && Array >= 0 && I < X.VisualCount; I++)
                Effect(ExportNaming::At<uint64_t>(*R.Z.Image, Array + I * 0x40));
            return Out;
        }
        if (Type == 13)
        {
            for (int I = 0; Array >= 0 && I < X.VisualCount; I++) Named(R.PointerAt(Array + I * 0x10), "material");
            return Out;
        }
        const char* Key = Type == 7 ? "model" : "material";
        if (X.VisualCount < 2) Named(Slot, Key);
        for (int I = 0; X.VisualCount >= 2 && Array >= 0 && I < X.VisualCount; I++) Named(R.PointerAt(Array + I * 0x40), Key);
        return Out;
    }

    // a Black Ops 4 element as Black Ops III's JSON
    Json Bo4Element(const Reader& R, const Bo4Elem& X)
    {
        Json J;
        const uint8_t Type = X.ElemType;
        J["type"] = Type < std::size(Bo4Types) && Bo4Types[Type] ? Json(Bo4Types[Type]) : Json(std::to_string(Type));
        J["flags"] = AllBits(X.Flags, Bo4Flags, 0x1f0);
        J["extraFlags"] = AllBits(X.ExtraFlags, Bo4Extra, 0);
        static const char* const Offsets[] = { "none", "sphere", "cylinder", "?" };
        J["spawnOffset"] = Offsets[(X.Flags >> 4) & 3];
        static const char* const Runs[] = { "world", "spawn", "effect", "offset", "effectNow", "?", "?", "?" };
        J["runRelativeTo"] = Runs[(X.Flags >> 6) & 7];
        J["spawn"] = Range(X.Spawn);
        J["spawnLoopingSpawnCount"] = Range(X.SpawnLoopingSpawnCount);
        J["spawnRange"] = Range(X.SpawnRange);
        J["fadeInRange"] = Range(X.FadeInRange);
        J["fadeOutRange"] = Range(X.FadeOutRange);
        J["spawnFrustumCullRadius"] = X.SpawnFrustumCullRadius;
        J["spawnDelayMsec"] = Range(X.SpawnDelayMsec);
        J["lifeSpanMsec"] = Range(X.LifeSpanMsec);
        J["spawnOrigin"] = Ranges(X.SpawnOrigin);
        J["spawnOffsetRadius"] = Range(X.SpawnOffsetRadius);
        J["spawnOffsetHeight"] = Range(X.SpawnOffsetHeight);
        J["spawnAngles"] = Ranges(X.SpawnAngles);
        J["angularVelocity"] = Ranges(X.AngularVelocity);
        J["initialRotation"] = Range(X.InitialRotation);
        J["rotationAxisPacked"] = X.RotationAxis;
        J["gravity"] = Range(X.Gravity);
        J["elasticity"] = Range(X.ReflectionFactor);
        J["atlas"] = Json{ { "behavior", X.Atlas.Behavior }, { "index", X.Atlas.Index }, { "fps", X.Atlas.Fps },
                           { "loopCount", X.Atlas.LoopCount }, { "colIndexBits", X.Atlas.ColIndexBits },
                           { "rowIndexBits", X.Atlas.RowIndexBits },
                           { "entryCount", 1 << ((X.Atlas.ColIndexBits + X.Atlas.RowIndexBits) & 15) } };
        J["collision"] = Json{ { "mins", Vec(X.CollMins, 3) }, { "maxs", Vec(X.CollMaxs, 3) } };
        const auto Effect = [&](uint64_t H) { return H != 0 && R.Z.Hashed ? R.Z.Hashed(H, "fx") : std::string(); };
        const auto Sound = [&](uint64_t H) { return H != 0 && R.Z.Hashed ? R.Z.Hashed(H, "sound") : std::string(); };
        J["fxOnImpact"] = Effect(X.EffectOnImpact);
        J["fxOnDeath"] = Effect(X.EffectOnDeath);   // between impact and emitted, as in Black Ops III (none of the 305 set it)
        J["fxEmitted"] = Effect(X.EffectEmitted);
        J["emitDist"] = Range(X.EmitDist);
        J["emitDistVariance"] = Range(X.EmitDistVariance);
        J["emitDensity"] = Range(X.EmitDensity);
        J["emitSizeForDensity"] = X.EmitSizeForDensity;
        J["fxAttached"] = Effect(X.EffectAttached);
        J["attachmentDensity"] = Range(X.AttachmentDensity);
        J["attachmentSizeForDensity"] = X.AttachmentSizeForDensity;
        J["lightingFrac"] = X.LightingFrac;
        J["displacement"] = X.Displacement;
        J["alphaFadeTimeMsec"] = X.AlphaFadeTimeMsec;
        J["maxWindStrength"] = X.MaxWindStrength;
        J["spawnIntervalAtMaxWind"] = X.SpawnIntervalAtMaxWind;
        J["lifespanAtMaxWind"] = X.LifespanAtMaxWind;
        // stored as Black Ops III's: a cloud's density range, written as the floats of its bits
        float Billboard[2];
        std::memcpy(Billboard, &X.Billboard, sizeof(Billboard));
        J["billboard"] = Json::array({ Billboard[0], Billboard[1] });
        J["billboardPivot"] = Vec(X.BillboardPivot, 2);
        J["spawnSound"] = Sound(X.SpawnSound);
        J["followSound"] = Sound(X.FollowSound);
        J["alphaDissolve"] = X.AlphaDissolve;
        J["zFeather"] = X.ZFeather;
        J["falloffAngles"] = Json::array({ X.FallOffBeginAngle, X.FallOffEndAngle });
        J["velocitySamples"] = VelocitySamples(R, X.VelSamples, X.VelIntervalCount + 1);
        J["visualSamples"] = Bo4VisualSamples(R, X.VisSamples, X.VisStateIntervalCount + 1);
        J["visuals"] = Bo4Visuals(R, X);
        return J;
    }
}

namespace
{
    // a Cold War element as far as its tables say: its type (BO3's names, 17 a vector field), its
    // visuals (one asset, a list of 0x40, or a decal's list of material pairs) and four effects
    Json ColdWarElement(const Reader& R, int64_t O, bool Looping)
    {
        using ExportNaming::At;
        const auto& Img = *R.Z.Image;
        const uint8_t Type = At<uint8_t>(Img, O + 0x328), Count = At<uint8_t>(Img, O + 0x332);
        Json E;
        E["type"] = Type < std::size(Bo3Types) ? Json(Bo3Types[Type]) : Type == 17 ? Json("vectorField") : Json(std::to_string(Type));
        const char* Key = Type == 7 ? "model" : Type == 14 ? "effect" : Type == 17 ? "vectorField" : "material";
        const bool Drawn = Type <= 7 || Type == 12 || Type == 13 || Type == 14 || Type == 17;
        Json V = Json::array();
        const uint64_t Visuals = R.PointerAt(O);
        const auto Add = [&](const char* K, uint64_t A) {
            std::string N = R.Asset(A);
            if (N.empty()) return;
            Json Visual{ { K, N } };
            // a material with its techset, images and blending when the reader gives them
            EffectZone::MaterialInfo M;
            if (std::strcmp(K, "material") == 0 && R.Z.Material && R.Z.Material(A, M)) Visual.update(MaterialFields(M));
            V.push_back(std::move(Visual));
        };
        const int64_t Array = R.Z.Off(Visuals);
        if (Type == 13)
            for (int64_t I = 0; Array >= 0 && I < Count; I++) Add("material", R.PointerAt(Array + I * 0x10));
        else if (Drawn && Count == 1) Add(Key, Visuals);
        else if (Drawn)
            for (int64_t I = 0; Array >= 0 && I < Count; I++) Add(Key, R.PointerAt(Array + I * 0x40));
        E["visuals"] = std::move(V);
        Json Fx = Json::array();
        for (int K = 0; K < 4; K++)
            if (std::string N = R.Asset(R.PointerAt(O + 0x160 + 8 * K)); !N.empty()) Fx.push_back(N);
        E["effects"] = std::move(Fx);
        // the timings, BO3's int ranges {base, amplitude} in the same order, placed by their values
        // over zm_common's 24 801 elements (not traced in the exe): the spawn at 0x188 (a looping
        // element's interval in ms and count, INT_MAX forever in 8 744 of 12 464; a one-shot's
        // count, 1 in 7 442 of 12 041), the looping spawn count at 0x190 (0 on every one-shot), the
        // spawn delay at 0x1a4 and the life span at 0x1ac (ms; never both 0, amplitude <= base in
        // 94 %)
        const auto IntRange = [&](int64_t At2) { return Json::array({ At<int32_t>(Img, O + At2), At<int32_t>(Img, O + At2 + 4) }); };
        if (Looping)
        {
            E["spawn"] = Json{ { "intervalMsec", At<int32_t>(Img, O + 0x188) }, { "count", At<int32_t>(Img, O + 0x18c) } };
            E["spawnLoopingSpawnCount"] = IntRange(0x190);
        }
        else
            E["spawn"] = IntRange(0x188);
        E["spawnDelayMsec"] = IntRange(0x1a4);
        E["lifeSpanMsec"] = IntRange(0x1ac);
        return E;
    }
}

decltype(EffectZone::Material) MaterialReader(std::function<XMaterial_t(uint64_t Address)> Read,
                                              std::function<int64_t(uint64_t Address)> Off)
{
    return [Read = std::move(Read), Off = std::move(Off)](uint64_t A, EffectZone::MaterialInfo& Out) {
        if (A == 0 || Off(A) < 0) return false;
        const XMaterial_t M = Read(A);
        // names as the files are named: a reference's "," dropped
        const auto Plain = [](const std::string& N) { return !N.empty() && N[0] == ',' ? N.substr(1) : N; };
        Out.Techset = Plain(M.TechsetName);
        static const char* const Usages[] = { "unknown", "diffuse", "normal", "specular", "gloss" };
        for (const XImage_t& I : M.Images)
        {
            const size_t U = (size_t)I.ImageUsage;
            Out.Images.push_back({ U < std::size(Usages) ? Usages[U] : "unknown", Plain(I.ImageName), I.SemanticHash });
        }
        // Black Ops and II: the reader's blendMode setting (source, destination factors)
        for (const auto& S : M.Settings)
            if (S.Name == "blendMode")
            {
                Out.BlendSrc = (int)S.Data[0];
                Out.BlendDst = (int)S.Data[1];
            }
        for (const auto& Block : M.Constants)
        {
            std::vector<float> F(Block.size() / 4);
            if (!F.empty()) std::memcpy(F.data(), Block.data(), F.size() * 4);
            Out.Constants.push_back(std::move(F));
        }
        return true;
    };
}

std::string MaterialJson(const std::string& Name, const EffectZone::MaterialInfo& M)
{
    Json J{ { "name", Name } };
    J.update(MaterialFields(M));
    return J.dump(1, '\t', false, Json::error_handler_t::replace) + "\n";
}

std::function<bool(const std::string& Name, uint64_t Address, std::string& Json, std::set<std::string>& Images)>
MaterialKind(decltype(EffectZone::Material) Read, std::function<void(uint64_t Address, nlohmann::ordered_json& J)> Extra)
{
    return [Read = std::move(Read), Extra = std::move(Extra)](const std::string& Name, uint64_t A, std::string& Out,
                                                              std::set<std::string>& Images) {
        EffectZone::MaterialInfo M;
        if (!Read(A, M)) return false;
        Out = MaterialJson(Name, M);
        if (Extra)
        {
            auto J = Json::parse(Out);
            Extra(A, J);
            Out = J.dump(1, '\t') + "\n";
        }
        for (const auto& I : M.Images) Images.insert(I.Name);
        return true;
    };
}

std::string EffectZone::Asset(uint64_t Address, int PointerSize) const
{
    const int64_t O = Off(Address);
    if (O < 0) return std::string();
    if (AssetName) return AssetName(O);
    std::string N = String(PointerSize == 8 ? ExportNaming::At<uint64_t>(*Image, O) : ExportNaming::At<uint32_t>(*Image, O));
    if (!N.empty() && N[0] == ',') N.erase(0, 1);
    return N;
}

bool EffectZone::AddMaterial(nlohmann::ordered_json& V, const std::string& Name, uint64_t Address, bool Reference, MaterialInfo* Out) const
{
    if (!Material) return false;
    MaterialInfo M;
    const bool Read = Material(Address, M);
    if (Read) V.update(MaterialFields(M));
    if ((!Read || M.Images.empty() || Reference) && NeedMaterial) NeedMaterial(Name);
    if (Out) *Out = std::move(M);
    return Read;
}

void EffectZone::ReadMaterials(const DiskGame::Context& C, decltype(Material) Read, bool NeedOthers)
{
    if (!C.Kinds.count(DiskGame::Kind::Image)) return;
    Material = std::move(Read);
    if (NeedOthers) NeedMaterial = [&C](const std::string& N) { C.Needs(DiskGame::Kind::Material, N); };
}

bool ReadEffect(const EffectZone& Z, EffectLayout L, int64_t Pos, const std::string& Name, std::string& Out)
{
    using ExportNaming::At;
    const Reader R(Z, L);
    const auto& Img = *Z.Image;
    const bool Bo3 = L == EffectLayout::Bo3;
    // the effect: counts, elements, lifetimes, bounds
    uint32_t Counts[3];
    uint64_t Elements;
    Json J;
    J["name"] = Name;
    if (L == EffectLayout::Iw3 || L == EffectLayout::T4 || L == EffectLayout::Codol)
    {
        // Call of Duty 4's FxEffectDef (0x20): Black Ops' first fields, no bounds; World at War's
        // (0x24) its priority byte before the elements
        for (int I = 0; I < 3; I++) Counts[I] = At<uint32_t>(Img, Pos + 0x10 + 4 * I);
        J["flags"] = At<uint8_t>(Img, Pos + 4);
        J["loopingLifeMsec"] = At<uint32_t>(Img, Pos + 0xc);
        if (L == EffectLayout::T4) J["priority"] = At<uint8_t>(Img, Pos + 0x1c);
        Elements = At<uint32_t>(Img, Pos + (L == EffectLayout::T4 ? 0x20 : 0x1c));
    }
    else if (L == EffectLayout::Iw4 || L == EffectLayout::Iw5)
    {
        // MW2's FxEffectDef (0x28, the iw4 table fdc940) and MW3's (0x40, iw5 f908d0): CoD4's
        // with an 8-byte name; MW3's occlusion query fields before the elements (0x38)
        for (int I = 0; I < 3; I++) Counts[I] = At<uint32_t>(Img, Pos + 0x14 + 4 * I);
        J["flags"] = At<uint32_t>(Img, Pos + 8);
        J["loopingLifeMsec"] = At<uint32_t>(Img, Pos + 0x10);
        if (L == EffectLayout::Iw5)
        {
            J["occlusionQueryDepthBias"] = At<float>(Img, Pos + 0x20);
            J["occlusionQueryFadeMsec"] = Json::array({ At<int32_t>(Img, Pos + 0x24), At<int32_t>(Img, Pos + 0x28) });
            J["occlusionQueryScaleRange"] = Json::array({ At<float>(Img, Pos + 0x2c), At<float>(Img, Pos + 0x30) });
        }
        Elements = At<uint64_t>(Img, Pos + (L == EffectLayout::Iw5 ? 0x38 : 0x20));
    }
    else if (L == EffectLayout::Bo1)
    {
        for (int I = 0; I < 3; I++) Counts[I] = At<uint32_t>(Img, Pos + 0x10 + 4 * I);
        Elements = At<uint32_t>(Img, Pos + 0x1c);
        J["flags"] = At<uint8_t>(Img, Pos + 4);
        J["priority"] = At<uint8_t>(Img, Pos + 5);
        J["loopingLifeMsec"] = At<uint32_t>(Img, Pos + 0xc);
        float B[7];
        for (int I = 0; I < 7; I++) B[I] = At<float>(Img, Pos + 0x20 + 4 * I);
        J["boundingBoxDim"] = Vec(B, 3);
        J["boundingSphere"] = Vec(B + 3, 4);
    }
    else if (L == EffectLayout::Bo4)
    {
        // fx (0xa0, table f2e18660): flags u32 0x18, lifetimes 0x1c/0x20, the element counts at
        // 0x24, the elements at 0x30, bounds at 0x38, priority u8 0x8c (all equal to Black Ops
        // III's on the 305 effects both games hold, but flags: more bits)
        for (int I = 0; I < 3; I++) Counts[I] = At<uint16_t>(Img, Pos + 0x24 + 2 * I);
        Elements = R.PointerAt(Pos + 0x30);
        J["flags"] = At<uint32_t>(Img, Pos + 0x18);
        J["priority"] = At<uint8_t>(Img, Pos + 0x8c);
        J["loopingLifeMsec"] = At<uint32_t>(Img, Pos + 0x1c);
        J["nonLoopingLifeMsec"] = At<uint32_t>(Img, Pos + 0x20);
        float B[6];
        for (int I = 0; I < 6; I++) B[I] = At<float>(Img, Pos + 0x38 + 4 * I);
        J["boundingBoxDim"] = Vec(B, 3);
        J["boundingCenter"] = Vec(B + 3, 3);
    }
    else if (L == EffectLayout::ColdWar)
    {
        // fx (0x90): the element counts at 0x1c (summed by the loader), the elements at 0x28
        for (int I = 0; I < 3; I++) Counts[I] = At<uint16_t>(Img, Pos + 0x1c + 2 * I);
        Elements = R.PointerAt(Pos + 0x28);
    }
    else
    {
        const int64_t P = R.Ptr;   // the name pointer's width moves the rest
        for (int I = 0; I < 3; I++) Counts[I] = At<uint16_t>(Img, Pos + P + 4 + 2 * I);
        J["flags"] = At<uint16_t>(Img, Pos + P);
        J["priority"] = At<uint16_t>(Img, Pos + P + 2);
        J["loopingLifeMsec"] = At<uint32_t>(Img, Pos + P + 0x10);
        J["nonLoopingLifeMsec"] = At<uint32_t>(Img, Pos + P + 0x14);
        Elements = R.PointerAt(Pos + P + 0x18);
        float B[6];
        const int64_t Bounds = Pos + P + 0x18 + P;
        for (int I = 0; I < 6; I++) B[I] = At<float>(Img, Bounds + 4 * I);
        J["boundingBoxDim"] = Vec(B, 3);
        J["boundingCenter"] = Vec(B + 3, 3);
    }
    J["counts"] = Json{ { "looping", Counts[0] }, { "oneShot", Counts[1] }, { "emission", Counts[2] } };
    const bool Cw = L == EffectLayout::ColdWar;
    const size_t Size = Cw                     ? 0x340
                        : L == EffectLayout::Bo4 ? sizeof(Bo4Elem)
                        : Bo3                    ? sizeof(Bo3Elem)
                        : L == EffectLayout::Iw3 ? sizeof(Iw3Elem)
                        : L == EffectLayout::T4  ? sizeof(T4Elem)
                        : L == EffectLayout::Codol ? 0x104
                        : IwLine(L)              ? sizeof(Iw4Elem)
                                                 : sizeof(Bo1Elem);
    const uint32_t Total = Counts[0] + Counts[1] + Counts[2];
    if (Total > 0 && Z.Off(Elements) < 0) return false;
    Json List = Json::array();
    static const char* const Groups[] = { "looping", "oneShot", "emission" };
    uint32_t Index = 0;
    for (int G = 0; G < 3; G++)
        for (uint32_t I = 0; I < Counts[G]; I++, Index++)
        {
            const uint64_t A = Elements + (uint64_t)Index * Size;
            Json E{ { "group", Groups[G] }, { "index", I } };
            if (L == EffectLayout::Bo4)
            {
                Bo4Elem X{};
                if (!R.Struct(A, X)) return false;
                E.update(Bo4Element(R, X));
            }
            else if (Cw)
            {
                const int64_t O = Z.Off(A);
                if (O < 0 || O + (int64_t)Size > (int64_t)Img.size()) return false;
                E.update(ColdWarElement(R, O, G == 0));
            }
            else if (Bo3)
            {
                Bo3Elem X{};
                if (!R.Struct(A, X)) return false;
                E.update(Common(R, L, X));
                E["extraFlags"] = Bits(X.ExtraFlags, Bo3Extra);
                E["spawnLoopingSpawnCount"] = Range(X.SpawnLoopingSpawnCount);
                E["spawnOffsetCylindricalAxis"] = X.SpawnOffsetCylindricalAxis;
                E["attractorLocalPosition"] = Vec(X.AttractorLocalPosition, 3);
                E["emitDensity"] = Range(X.EmitDensity);
                E["emitSizeForDensity"] = X.EmitSizeForDensity;
                E["attachmentDensity"] = Range(X.AttachmentDensity);
                E["attachmentSizeForDensity"] = X.AttachmentSizeForDensity;
                E["displacement"] = X.Displacement;
                E["followSound"] = X.FollowSound ? R.Named(X.FollowSound) : std::string();
                E["alphaDissolve"] = X.AlphaDissolve;
                E["zFeather"] = X.ZFeather;
                E["falloffAngles"] = Json::array({ X.FallOffBeginAngle, X.FallOffEndAngle });
                E["visuals"] = Visuals(R, L, X.ElemType, X.VisualCount, X.Visuals, reinterpret_cast<const uint8_t*>(&X.Visuals));
            }
            else if (IwLine(L))
            {
                const bool Read = L == EffectLayout::Iw3 ? IwElement<Iw3Elem>(R, L, A, E)
                                  : L == EffectLayout::T4 ? IwElement<T4Elem>(R, L, A, E)
                                                          : IwElement<Iw4Elem>(R, L, A, E);
                if (!Read) return false;
            }
            else if (L == EffectLayout::Codol)
            {
                Iw3Elem C4{};
                if (!R.Struct(A, C4)) return false;
                // as Black Ops' element: its fields where they are, its type renumbered (billboard,
                // oriented, tail, trail, cloud, model, omni light, spot light, sound, decal, runner)
                static const uint8_t Iw3Types[] = { 0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12 };
                // Call of Duty Online's (the codol table f8a6a0: model 9, nothing for 10 and 11, a
                // string for 12 and 14, decal 13; f8a880: trail 5, spark fountain 8)
                static const uint8_t CodolTypes[] = { 0, 1, 2, 3, 4, 5, 6, 6, 3, 7, 8, 9, 10, 11, 12 };
                static const char* const CodolNames[] = { "billboardSprite", "orientedSprite", "rotatedSprite", "tail", "line", "trail",
                                                          "cloud", "sparkCloud", "sparkFountain", "model", "omniLight", "spotLight",
                                                          "sound", "decal", "runner" };
                const bool Codol = L == EffectLayout::Codol;
                const uint8_t* Types = Codol ? CodolTypes : Iw3Types;
                const size_t TypeCount = Codol ? std::size(CodolTypes) : std::size(Iw3Types);
                Bo1Elem X{};
                X.Flags = C4.Flags, X.Spawn = C4.Spawn, X.SpawnRange = C4.SpawnRange, X.FadeInRange = C4.FadeInRange;
                X.FadeOutRange = C4.FadeOutRange, X.SpawnFrustumCullRadius = C4.SpawnFrustumCullRadius;
                X.SpawnDelayMsec = C4.SpawnDelayMsec, X.LifeSpanMsec = C4.LifeSpanMsec;
                for (int K = 0; K < 3; K++)
                {
                    X.SpawnOrigin[K] = C4.SpawnOrigin[K], X.SpawnAngles[K] = C4.SpawnAngles[K];
                    X.AngularVelocity[K] = C4.AngularVelocity[K], X.CollMins[K] = C4.CollMins[K], X.CollMaxs[K] = C4.CollMaxs[K];
                }
                X.SpawnOffsetRadius = C4.SpawnOffsetRadius, X.SpawnOffsetHeight = C4.SpawnOffsetHeight;
                X.InitialRotation = C4.InitialRotation, X.Gravity = C4.Gravity, X.ReflectionFactor = C4.ReflectionFactor;
                X.Atlas = C4.Atlas;
                X.ElemType = C4.ElemType < TypeCount ? Types[C4.ElemType] : (uint8_t)(100 + C4.ElemType);
                X.VisualCount = C4.VisualCount, X.VelIntervalCount = C4.VelIntervalCount, X.VisStateIntervalCount = C4.VisStateIntervalCount;
                X.VelSamples = C4.VelSamples, X.VisSamples = C4.VisSamples, X.Visuals = C4.Visuals;
                X.EffectOnImpact = C4.EffectOnImpact, X.EffectOnDeath = C4.EffectOnDeath, X.EffectEmitted = C4.EffectEmitted;
                X.EmitDist = C4.EmitDist, X.EmitDistVariance = C4.EmitDistVariance;
                X.SortOrder = C4.SortOrder, X.LightingFrac = C4.LightingFrac;
                E.update(Common(R, L, X));
                for (const char* Gone : { "rotationAxisPacked", "windInfluence", "fxAttached", "alphaFadeTimeMsec", "maxWindStrength",
                                          "spawnIntervalAtMaxWind", "lifespanAtMaxWind", "billboard", "billboardPivot", "spawnSound" })
                    E.erase(Gone);   // not Call of Duty 4's
                E["sortOrder"] = X.SortOrder;
                E["visuals"] = Visuals(R, L, X.ElemType, X.VisualCount, X.Visuals, nullptr);
                if (Codol)
                {
                    if (C4.ElemType < std::size(CodolNames)) E["type"] = CodolNames[C4.ElemType];
                    // the string the table reads at 0x100
                    const int64_t O = Z.Off(A);
                    E["name100"] = O >= 0 ? R.Named(At<uint32_t>(Img, O + 0x100)) : std::string();
                }
            }
            else
            {
                Bo1Elem X{};
                if (!R.Struct(A, X)) return false;
                E.update(Common(R, L, X));
                E["sortOrder"] = X.SortOrder;
                E["visuals"] = Visuals(R, L, X.ElemType, X.VisualCount, X.Visuals, nullptr);
            }
            List.push_back(std::move(E));
        }
    J["elements"] = std::move(List);
    // a name read off bytes that are not UTF-8 is written with U+FFFD, not thrown
    Out = J.dump(1, '\t', false, Json::error_handler_t::replace);
    Out += "\n";
    return true;
}
}

Treyarch::EffectZone ZonekitZone::EffectView() const
{
    return { &Image(), [this](uint64_t A) { return Off(A); }, [this](uint64_t A) { return StringAt(A); } };
}

void ZonekitZone::ExportsEffects(std::function<XMaterial_t(uint64_t Address)> Read, Treyarch::EffectLayout L)
{
    using namespace DiskGame;
    ReadsMaterials(Read);
    const auto Materials = [this, Read = std::move(Read)] { return Treyarch::MaterialReader(Read, [this](uint64_t A) { return Off(A); }); };
    // the install index lists the zone's materials (an effect's, pulled from where they are)
    ExportsMaterials("material", Treyarch::MaterialKind(Materials()));
    // with the images: each material's, for the preview to find among them
    Exports(Kind::Fx, [this, L, Materials](const Context& C, Totals& T) {
        auto Z = EffectView();
        Z.ReadMaterials(C, Materials());
        ExportEffects(C, T, "fx", [&](int64_t Pos, const std::string& N, std::string& J) { return Treyarch::ReadEffect(Z, L, Pos, N, J); });
    });
}
