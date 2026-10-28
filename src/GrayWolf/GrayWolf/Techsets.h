#pragma once

#include "ByteView.h"

// The techsets kind (DiskGame::Kind::Techsets), for the games whose materials draw through
// techsets of DXIL shaders and a bindless material buffer (Modern Warfare III, Black Ops 6 and 7):
// each techset of a zone, its techniques and their shaders (the bytecode as stored, its
// reflection), the arguments binding code and material data to the shaders' registers, and what
// each material of the zone binds. Under <out>/techsets/<zone>/: techsets.json, shaders.json,
// shaders/<id>.<stage>.dxbc, materials.csv, recipes.csv. One reader for every game: a game gives
// its layouts and what its argument types bind (a Techsets::Format, Games/<GAME>/Techsets.cpp).

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace DiskGame
{
    struct Context;
    struct Totals;
}

namespace Techsets
{
    // A zone as the reader reads it: the walked one served, or a techset zone of the install
    struct ZoneView
    {
        std::string Name;
        const ByteView* Image = nullptr;
        std::function<int64_t(int64_t Field)> Ptr;               // where the pointer field at Field points in Image, -1 elsewhere
        std::vector<std::pair<std::string, int64_t>> Assets;     // the zone's assets: type, position
        std::function<std::string(uint64_t Hash)> NameOf;        // a 63-bit hash's name, "" unknown
        std::function<bool(uint64_t Key, std::vector<uint8_t>& Out)> Package;   // a package object (.xsub) by key
        std::string Folder;                                      // the zone's fast file folder (the install's techsets)
        std::string Profile;                                     // its zonekit profile
    };

    // A technique argument (8 bytes): u8 type 0, u8 stages 1 (bit 0 vertex, bit 1 pixel), u16 dest
    // 2, u16 source 4, u8 6 (the view: a D3D_SHADER_INPUT_TYPE), u8 7 (the view's dimension)
    struct Argument
    {
        uint8_t Type = 0, Stages = 0, View = 0, Dimension = 0;
        uint16_t Dest = 0, Source = 0;
    };

    // What an argument binds, as the game's shaders prove it
    struct Binding
    {
        enum class Source
        {
            Code,              // the engine's (a code constant, texture, buffer)
            MaterialTexture,   // the material's image of semantic <source>
            MaterialConstant,  // the material's constant of semantic <source>
            Sampler,           // a sampler state <source>
        };
        std::string Kind;      // code_constant, code_texture, material_buffer, sampler...
        std::string Register;  // t42, b2+0x30, perObject[3]...; "" unknown
        int Space = -1;        // -1 none or unknown
        Source From = Source::Code;
    };

    // Where each of a material's slots sits in its block of the material buffer (byte offsets)
    struct Block
    {
        std::vector<uint32_t> Constants, Images;   // by slot, in the material's slot order
        uint32_t Layers = 0;                       // a layered material's layer count (0 none)
    };
    // A material's slot tables, for the game's block layout
    struct MaterialSlots
    {
        int64_t Pos = -1;
        std::vector<uint8_t> ImageSemantics, ConstantSemantics;
    };

    // A game's techsets: where its structures keep what the reader needs (offsets in bytes; -1
    // where the game has no such field)
    struct Layout
    {
        // techset: technique masks (MaskWords u64 from Masks: a technique per set bit, its slot the
        // bit), the techniques (a pointer each, in mask order)
        int64_t TechsetMasks = 0, TechsetMaskWords = 0, TechsetTechniques = 0;
        // technique: its shader records by stage (offset, vs / hs / ds / ps...), its arguments (8
        // bytes each, u8 count)
        std::vector<std::pair<int64_t, const char*>> TechniqueStages;
        int64_t TechniqueArgs = 0, TechniqueArgCount = 0;
        // shader record: its HLSL file name (a string pointer), its DXBC size (u32), its bytecode
        // record {pointer, u32 size, u32 flags (bit 31: the bytes follow in the zone)} or, when not
        // in the zone, {pointer, u64 package key}
        int64_t ShaderName = -1, ShaderDxbcSize = 0, ShaderCode = 0;
        // material: its techset; image slots {u8 semantic, u8 image} (u8 count), images (pointers, u8
        // count); constant slots {u8 semantic, u8 row} (u8 count), 16-byte rows (u8 count); its block's
        // size (u32); a kind field (the u64 there, masked) whose value NoTechset says the techset field
        // is something else (a decal volume's)
        int64_t MaterialTechset = 0, MaterialImageSlots = 0, MaterialImageSlotCount = 0, MaterialImages = 0, MaterialImageCount = 0;
        int64_t MaterialConstSlots = 0, MaterialConstSlotCount = 0, MaterialConsts = 0, MaterialConstCount = 0;
        int64_t MaterialBlockSize = -1, MaterialKind = -1;
        uint64_t MaterialKindMask = ~0ull, MaterialNoTechset = 0;
        // the material buffer (a ByteAddressBuffer t<MaterialBuffer>, space 1, every material's block
        // at a base the draw passes) and a block's header
        uint32_t MaterialBuffer = 0, BlockHeader = 0;
        // the fields not named, written raw: (offset, bytes) of a techset, a technique, a shader
        // record; a material's as "other" rows
        std::vector<std::pair<int64_t, int64_t>> TechsetRaw, TechniqueRaw, ShaderRaw, MaterialRaw;
    };

    // A game's techsets: its layouts, what its arguments bind, and what differs from the common reading
    class Format
    {
    public:
        virtual ~Format() = default;
        virtual const Layout& Layouts() const = 0;
        virtual Binding Bind(const Argument& A) const = 0;
        // The material's slots in its block: the header, a 16-byte row per constant slot, then a
        // u32 per image slot (the image's index in the bindless table), in slot order
        virtual Block BlockOf(const ZoneView& Z, const MaterialSlots& M) const;
        // Game fields of a technique (Extra: named ones, Raw: hex), beside Layout's raw ones
        virtual void TechniqueFields(const ZoneView& Z, int64_t Pos, nlohmann::ordered_json& Extra, nlohmann::ordered_json& Raw) const {}
        // Whether a material's techset is looked up in the install's techset zones when this zone
        // does not define it (its techniques' own material arguments): the game's materials bind
        // through arguments as well as their block
        virtual bool TechniquesBindMaterials() const { return false; }
    };

    // N bytes of B from Pos as hex (what is in range)
    std::string Hex(ByteView B, int64_t Pos, int64_t N);

    // Writes the zone's files (Format: the game's) under C's techsets folder
    void Export(const ZoneView& Z, const Format& F, const DiskGame::Context& C, DiskGame::Totals& T);
}
