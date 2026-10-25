#pragma once

// The fx kind for the Infinity Ward engine's 64-bit FxEffectDef (Ghosts, Advanced Warfare, Modern
// Warfare Remastered): an effect read from the relocated zone into the JSON the Treyarch reader
// writes (TreyarchEffects.h) - its element counts and lifetime, then each element (looping,
// one-shot, emission) with its type, flags, spawn and life timings, ranges, atlas, velocity and
// visual state samples, what it draws or plays by name (materials, models, effects, sounds, light
// definitions) and its trail, spark fountain or spot light definition. Values are the stored ones
// (angles in radians, times in milliseconds, ranges as base and amplitude), a colour scaled to
// 0..255 as the others write theirs. The layouts are x64-zt's structs (zonetool's IW6, S1 and H1
// structs.hpp), every offset a pointer or a count sits at checked against the game's zonekit
// table (Layout).

#include "Games/Treyarch/TreyarchEffects.h"

#include <cstdint>
#include <string>
#include <vector>

namespace IwEffects
{
    struct Layout
    {
        const char* Game = "";
        int64_t Elements = 0x38;                // FxEffectDef: the element array (counts at 0x14, 0x18, 0x1c)
        int64_t ElemSize = 0;                   // FxElemDef
        int64_t Inserted = 0;                   // bytes before the atlas (WWII 8: a range), moving the rest
        int64_t VelSample = 0x60, VelFirst = 0; // FxElemVelStateSample, where its local and world frames start
        int64_t VisState = 0;                   // one FxElemVisualState (a sample is two: base, amplitude)
        bool EmissiveRgb = false;               // the state's colour followed by emissiveScale[3] and a pivot (S1, H1), else by unlitHDRScale
        int64_t DecalStride = 0, DecalMaterials = 0;   // a decal's mark visuals (materials per record)
        int64_t TrailVertCount = 0x14;          // FxTrailDef's vertCount (the fields before it are its floats)
        std::vector<const char*> Types;         // the game's element types by number, the names the preview reads
        struct Bit { uint32_t Mask; const char* Name; };
        std::vector<Bit> Flags;
        uint32_t RunMask = 0x1c0;               // flags' run-relative field, from bit 6
    };

    const Layout& Ghosts();
    const Layout& AdvancedWarfare();
    const Layout& ModernWarfareRemastered();
    const Layout& WorldWar2();

    // The FxEffectDef at Pos as JSON text (Name its name); false when its elements are not in the zone.
    bool ReadEffect(const Treyarch::EffectZone& Z, const Layout& L, int64_t Pos, const std::string& Name, std::string& Json);
}
