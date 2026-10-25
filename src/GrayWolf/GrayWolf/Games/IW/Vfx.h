#pragma once

// The vfx kind's core: the particle system of Infinite Warfare and its successors (emitters ->
// states -> init/update/test module groups -> typed modules), read through a game's Layout (where
// each level sits, its module types by id with their fields) into the fx kind's JSON, one shape
// for every game: modules by canonical name (IW7's, from its exe's module table), fields, curves,
// linked assets; a type the game does not describe written raw. ReadPreview maps that JSON to
// FxPreview's model by canonical names only. Format notes: graywolf-tools docs/vfx-format.md.

#include "Games/Treyarch/TreyarchEffects.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace FxPreview
{
    struct Effect;
}

namespace Vfx
{
    enum class FieldType
    {
        U8, Bool, U16, U32, I32, F32,
        Hex32,      // flags, written as "0x..."
        Hex64,
        Range,      // {float min, max}
        IntRange,   // {int min, max}
        Vec4,       // float4
        Curve,      // {keys pointer, i32 count, float scale}; a key {time, value, invTimeDelta, pad}
        CurveArray, // a pointer to Count curves {keys pointer, i32 count, pad} (BO6/BO7), their scales apart (Field::Scale)
    };
    struct Field
    {
        const char* Name = "";
        int64_t Offset = 0;
        FieldType Type = FieldType::F32;
        int Count = 1;       // an array of Count values (curves 16 bytes apart)
        int64_t Scale = -1;  // CurveArray: the f32[Count] scales in the data (-1: none, scale 1)
        int64_t Via = -1;    // read through the pointer at this offset of the parent (-1: in the parent)
    };

    // A's fields then B's (a layout's shared field lists, as the shapes' first bytes)
    inline std::vector<Field> With(std::vector<Field> A, const std::vector<Field>& B)
    {
        A.insert(A.end(), B.begin(), B.end());
        return A;
    }

    // What a module's linked asset list holds (one entry per Layout::AssetSize bytes)
    enum class AssetKind { None, Material, Model, Effect, Sound, VectorField, LightDef, ParticleSim, Physics, Decal };

    struct Module
    {
        int Id = -1;                 // the game's type id
        const char* Name = "";       // canonical name (IW7's: "InitSpawn", "ColorGraph"...)
        int64_t Size = 0;            // its data's bytes
        std::vector<Field> Fields;   // offsets from the data's start ({type, flags} header included)
        AssetKind Assets = AssetKind::None;
        int64_t AssetList = -1;      // the linked asset list {pointer, i32 count} in the data, -1 none
    };

    struct Layout
    {
        const char* Game = "";
        // ParticleSystemDef: name pointer (not read: the caller names it; -1 a hash), emitter array and count
        int64_t SystemName = 0, SystemEmitters = 8, SystemEmitterCount = 0x1c;
        std::vector<Field> SystemFields;
        // ParticleEmitterDef: size, state array and count
        int64_t EmitterSize = 0x90, EmitterStates = 0, EmitterStateCount = 8;
        std::vector<Field> EmitterFields;
        // ParticleStateDef: size, module group array (GroupCount: init, update, test), element type u32
        int64_t StateSize = 0x20, StateGroups = 0;
        int GroupCount = 3;
        int64_t StateElementType = 8;
        std::vector<Field> StateFields;
        // ParticleModuleGroupDef: size, module array and count
        int64_t GroupSize = 0x10, GroupModules = 0, GroupModuleCount = 8;
        // ParticleModuleDef: size, u16 type, its data inline at ModuleData or behind the pointer there
        int64_t ModuleSize = 0xa0, ModuleType = 0, ModuleData = 0x10;
        bool ModuleDataPointer = false;
        int64_t ModuleFlags = 4;        // the data's u32 flags (MWIII, BO6/BO7: 0)
        int64_t AssetSize = 0x20;       // ParticleLinkedAssetDef
        // a material's atlas: its u8 row and column counts in the Material (-1: not read)
        int64_t MaterialAtlasRows = -1, MaterialAtlasCols = -1;
        std::vector<const char*> ElementTypes;
        std::vector<Module> Modules;

        const Module* Find(int Id) const;
    };

    // What a read found, for the kind's counters
    struct Counts
    {
        size_t Emitters = 0, States = 0, Modules = 0, Unknown = 0;
        std::map<int, size_t> UnknownIds;   // unknown type id -> modules
        std::set<std::string> Images;       // the images the materials read list ("$" ones left out)
        // the kind's counters by name (vfx_emitters, vfx_modules, vfx_modules_unknown)
        std::map<std::string, size_t> Named() const
        {
            return { { "vfx_emitters", Emitters }, { "vfx_modules", Modules }, { "vfx_modules_unknown", Unknown } };
        }
    };

    // The ParticleSystemDef at Pos as the fx kind's JSON text; false when its emitters are not in
    // the zone. Outside: the name of an asset the zone does not hold (Black Ops 6/7: a name hash, or
    // an asset of a zone served beside), "" when unset. Materials as Treyarch's effect reader writes them (Z.Material, Z.NeedMaterial).
    bool ReadSystem(const Treyarch::EffectZone& Z, const Layout& L, int64_t Pos, const std::string& Name, std::string& Json,
                    Counts* Count = nullptr, const std::function<std::string(uint64_t Address)>& Outside = {});

    // A material's atlas (its MaterialAtlasRows and MaterialAtlasCols counts), what an effect's
    // frames are, as the material kind's "atlas": the material at zone offset O (-1: none), Size
    // of its bytes checked to be in the zone (0: not checked)
    void AddAtlas(nlohmann::ordered_json& J, const Layout& L, const std::vector<uint8_t>& Image, int64_t O, int64_t Size = 0);

    // The preview adapter: a vfx JSON (format "vfx") into FxPreview's model, each emitter's first
    // state an element; false with Error when it is not one.
    bool ReadPreview(const nlohmann::json& J, FxPreview::Effect& Fx, std::string& Error);
}
