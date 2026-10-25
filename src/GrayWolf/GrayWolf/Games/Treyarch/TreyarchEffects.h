#pragma once

// The fx kind for Treyarch's FxEffectDef as Black Ops 1, 2 and 3 lay it out: an effect read from the
// relocated zone into JSON - its lifetimes and bounds, then each element (looping, one-shot,
// emission) with its type, flags, spawn and life timings, ranges, atlas, velocity and visual state
// samples, and what it draws or plays by name (materials, models, effects, sounds). Values are the
// stored ones (angles in radians, times in milliseconds, ranges as base and amplitude). The
// structs are the ones Greyhound's CoDEffectTranslator read (removed upstream
// in 2022, e66d8f7), their offsets checked against the zonekit tables. BO4's element (0x280) is
// BO3's fields rearranged, its references hashes; Cold War's (0x340) only as far as its tables
// say (EffectLayout).

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

struct XMaterial_t;
namespace DiskGame
{
    struct Context;
}

namespace Treyarch
{
    // ColdWar: what its zonekit tables say of the element (0x340: its type at 0x328, its visuals at
    // 0, their count at 0x332, four effect references at 0x160) - the type, what it draws and the
    // effects it starts, no timing: the rest of the struct is not mapped.
    // Iw3: Call of Duty 4's FxElemDef (0xfc), the one Black Ops grew from: read as Black Ops',
    // its types renumbered to theirs, no bounds on the effect. T4: World at War's (0x100, CoD4's
    // with a wind influence). Iw4, Iw5: MW2's and MW3's 64-bit rebuild (0x120, CoD4's with 8-byte
    // pointers, the collision box as centre and half size, MW3's effect with occlusion fields).
    // Bo4: Black Ops 4's (0x280), written as Black Ops III's; its effects and sounds are name
    // hashes (EffectZone::Hashed). Codol: Call of Duty Online's (0x104), CoD4's with a visual state
    // of 0x20 and a string at 0x100, its types numbered its own way.
    enum class EffectLayout { Bo1, Bo2, Bo3, ColdWar, Iw3, T4, Iw4, Iw5, Bo4, Codol };

    // What the reader reads a zone through.
    struct EffectZone
    {
        const std::vector<uint8_t>* Image = nullptr;
        std::function<int64_t(uint64_t Address)> Off;            // zone offset, -1 outside
        std::function<std::string(uint64_t Address)> String;     // a string pointer's text, "" outside
        // the asset at a zone offset by name; unset: the string its first field points to
        std::function<std::string(int64_t Pos)> AssetName;
        // a name hash's name as the game's kinds name it (Prefix_<hex> when unknown): Black Ops 4
        std::function<std::string(uint64_t Hash, const char* Prefix)> Hashed;
        // A material's techset, images (usage, semantic hash, name) and blend factors (the
        // game's GFXS_BLEND_* of its first state bits, -1 unknown) by its address, read by the
        // game's material reader; set when the run exports images too (FxPreview finds them by
        // name): each material visual then lists them. Unset: names only.
        struct MaterialImage { std::string Usage, Name; uint32_t Semantic = 0; };
        // Constants: the material's constant blocks by technique (Black Ops III: each technique's
        // static pixel cbuffer), as floats
        struct MaterialInfo { std::string Techset; std::vector<MaterialImage> Images; int BlendSrc = -1, BlendDst = -1;
                              std::vector<std::vector<float>> Constants; };
        std::function<bool(uint64_t Address, MaterialInfo& Out)> Material;
        // A material that zone does not define (no images read): needed from the zone that does
        // (the material kind, through the install index)
        std::function<void(const std::string& Material)> NeedMaterial;

        // With the image kind in the run (C): the materials read by Read, one another zone defines
        // needed from there (the material kind) unless NeedOthers is false
        void ReadMaterials(const DiskGame::Context& C, decltype(Material) Read, bool NeedOthers = true);
        // The asset at an address by name: AssetName's, else the text its first field (a
        // PointerSize-byte pointer) points to, a name-only header's "," dropped; "" outside the zone
        std::string Asset(uint64_t Address, int PointerSize = 8) const;
        // A material visual's techset, images and blending (MaterialJson's fields but its name)
        // added to V when Material reads it; one not read whole (no images, or Reference: a
        // name-only header here) needed from the zone defining it (NeedMaterial). Out: what was
        // read. False: not read.
        bool AddMaterial(nlohmann::ordered_json& V, const std::string& Name, uint64_t Address, bool Reference = false,
                         MaterialInfo* Out = nullptr) const;
    };

    // A material as the material kind writes it: its name, techset, images and blend factors
    std::string MaterialJson(const std::string& Name, const EffectZone::MaterialInfo& M);

    // The material kind's reader (ZonekitZone::ExportsMaterials) through a material reader: each
    // as MaterialJson writes it, Extra adding a game's fields (its address; unset: none), and
    // its images' names
    std::function<bool(const std::string& Name, uint64_t Address, std::string& Json, std::set<std::string>& Images)>
    MaterialKind(decltype(EffectZone::Material) Read, std::function<void(uint64_t Address, nlohmann::ordered_json& J)> Extra = {});

    // EffectZone::Material through a game's material reader (Read: the material at an address);
    // Off tells the addresses in the zone, the only ones read.
    decltype(EffectZone::Material) MaterialReader(std::function<XMaterial_t(uint64_t Address)> Read,
                                                  std::function<int64_t(uint64_t Address)> Off);

    // The FxEffectDef at Pos as JSON text (Name its name); false when its elements are not in the zone.
    bool ReadEffect(const EffectZone& Z, EffectLayout L, int64_t Pos, const std::string& Name, std::string& Json);
}
