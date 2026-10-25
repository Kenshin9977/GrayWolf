#pragma once

// What the readers of Black Ops 6 and Black Ops 7 (the iwctx zone format: their zones served as
// game memory by the disk template) read alike: packed normals, images and sounds.
// Each game passes what its build moves (GameBlackOps6::Layout, the campaign's included).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "CoDXModelTranslator.h"
#include "DiskGame.h"
#include "Games/IW/IwEntityKeys.h"
#include "Games/Treyarch/TreyarchEffects.h"

namespace IwctxReaders
{
    // A vertex normal from its packed tangent frame (a quaternion, its largest component dropped:
    // 10, 10, 9 bits and the dropped one's index in the top 2)
    void UnpackNormalQuat(uint32_t Packed, float (&Normal)[3]);

    // GfxImage (0x40): hash 0, in-zone size u32 0x18, width u16 0x20, height 0x22, format u8 0x2a,
    // mip count u8 0x2e (low 4 bits), 24-byte mip records {u64 package key, u64, u64 packed} at 0x30
    // (smallest first), in-zone pixels at 0x38. Formats: the game's format -> DXGI
    std::unique_ptr<XImageDDS> LoadXImage(const XImage_t& Image, const uint8_t* Formats, uint32_t FormatCount);

    // sndasset: flags u8 at Flags, seek table size u32 0x20, frames' size u32 0x24, frame count
    // u32 0x28, rate u16 at Rate. Where its Opus frames are, by the flags: the .xsub object a key
    // names (Key, -1 none; the object a 32-byte header when it carries the -4 tag at 8, else none,
    // the seek table, then the frames), else in the zone (ZoneFrames pointer, ZoneSize u32; -1 none),
    // else, that pointer null, the object KeyElse names (Black Ops 7's {frames, size} or {0, key})
    struct SoundSource
    {
        int64_t Key = -1, ZoneFrames = -1, ZoneSize = -1, KeyElse = -1;
    };
    struct SoundLayout
    {
        int64_t Flags, Rate;
        SoundSource (*Where)(uint8_t Flags);
        bool ZoneCappedByLoaded;      // frames in the zone: at most the frames' size (BO7) or all ZoneSize (BO6)
        int32_t PreSkip;              // the decoded samples before the sound's first (SABSupport::OpusPreSkip...)
    };
    std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound, const SoundLayout& L);    constexpr int64_t SoundSeek = 0x20, SoundLoaded = 0x24, SoundFrames = 0x28;

    // The sndassets of a zone written in the settings' format, each once by name (the name's
    // folders kept), decoded by the game's ReadXSound: Full its name, Address where it is served
    struct SoundEntry
    {
        std::string Full;
        uint64_t Address = 0;
        uint16_t Rate = 0;
        uint32_t Frames = 0;
    };
    void ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T, const std::string& Zone, const std::vector<SoundEntry>& Sounds);

    // xanim: Modern Warfare's packed keys (the bone counts in the header) in 1 to 4 parts, each a
    // .xsub object or in the zone. The first part opens with a table of u32 section offsets (-1:
    // none): 0 data bytes, 1 data shorts, 2 data ints, 3 + k part k's random shorts, 7 + k its
    // random bytes, 13 the bone names (u64 FNV-1a 64), 15 the u16 frame indices, 24 the notetracks.
    // A part list entry (0x18): {data in the zone, u32 size at 8} or {0, the .xsub key at 8, size}.
    // The header's offsets per game:
    struct XAnimLayout
    {
        int64_t Rate, Frames, Flags, Type, Counts, PartCount, Parts;
        // Black Ops 7's sampler (cod.exe 0x6aadce0e: 0xabf8eb0 / 0xabfd3e0 for frame counts under /
        // from 0x100, run under emulation against these keys): a table's per-part key counts only when it has 4 keys or more (fewer: all in
        // the first part); the coarse lookup table before u16 frame indices only with a single part;
        // part k's random bytes at section 7 + k; a bone's keys merged across the parts by frame.
        // Off: Black Ops 6's reading as it was written (not checked against its sampler)
        bool GameRules;
        // Notetracks (Black Ops 7's iterator 0x40e65f0 / 0x40e6570): parts 0 and 1 may hold 16-byte
        // records {u16 name index, u16, float time (0 to 1), ...} at the u64 section 16 + 2k of the
        // first part's table (u32 -1: none), their count byte k of section 22, the total the header's
        // byte NoteCount; yielded by time (a tie: part 0's first). The names: the header's list at
        // NoteNames (u16 count at NoteNameCount) holds each name's 64-bit script string hash, its low
        // u32s then its high u32s; NoteName names a hash. NoteCount -1: no notetracks read
        int64_t NoteCount = -1, NoteNames = 0, NoteNameCount = 0;
        std::string (*NoteName)(uint64_t) = nullptr;
        // the list holds zone string slots (u32 each, Black Ops 6's campaign: Load_ScriptString
        // 0x26f58a0 maps a slot of the zone's list) instead of the 64-bit hashes
        bool NoteNameSlots = false;
    };
    // The parts' bytes (the first missing: none; a later one missing: empty, its keys left out as
    // the game plays an animation whose on-demand part is not streamed)
    std::vector<std::vector<uint8_t>> XAnimParts(uint64_t Anim, const XAnimLayout& L);
    // The header (frames, rate, flags, bone names) read, the keys left to LoadXAnim
    std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation, const XAnimLayout& L, std::string (*BoneName)(uint64_t));
    void LoadXAnim(const std::unique_ptr<XAnim_t>& Anim, std::unique_ptr<WraithAnim>& Result, const XAnimLayout& L);

    // A zone's script strings by slot: XAssetList's count u32 0 (none when its pointer at 8 is 0), its
    // slots (u64) after the list (ListSize: the profile's zone_load.asset_list_size, 0x30; Black Ops 6's
    // campaign 0x20), then the text of each slot with bit 61 set, NUL-terminated, encrypted with the
    // string cipher of the profile in ProfileDir (strings.cipher.txt); "" for the others
    std::vector<std::string> ZoneStrings(const std::vector<uint8_t>& Zone, const std::string& ProfileDir, size_t ListSize = 0x30);
    // Script strings by their 64-bit hash, XXH3-64 with the seed 0x100000001b3 (the game's
    // DB_RegisterScrString: Black Ops 7 0x3ad7010 -> 0xae3ccc0; Black Ops 6 the same "end" hash):
    // each zone's own registered as it opens, "end" built in (a script constant); unknown: its hex
    uint64_t ScriptStringHash(const std::string& Text);
    void RegisterScriptStrings(const std::vector<std::string>& Texts);
    std::string ScriptString(uint64_t Hash);
    // The served zone's strings by slot, for names given as a slot (SlotScriptString: the text,
    // else "string_<slot>")
    void ServeSlotStrings(const std::vector<std::string>& Slots);
    std::string SlotScriptString(uint64_t Slot);

    // The live material reader for the material and fx kinds with the images another zone holds:
    // a Material (Size bytes) keeps slots {u8 semantic, u8 image} at Slots (u8 count at SlotCount)
    // into its image pointers at Images (u8 count at ImageCount); a slot whose image is a name hash
    // (bit 63) not in the zone is added, named by Name, a diffuse map when its semantic is one of
    // Colour. The first layer's emissive, opacity and transmittance (1 - opacity) maps and the ramp
    // an emissive grey looks its colour up in (vfx_ramp_*) take those usages by their semantic,
    // for the effect preview.
    struct MaterialSlots
    {
        int64_t Size, Slots, SlotCount, Images, ImageCount;
        std::vector<uint8_t> Colour;
        uint8_t Emissive, Opacity, Transmittance, Ramp;
    };
    decltype(Treyarch::EffectZone::Material) SlotMaterials(const MaterialSlots& L, std::function<XMaterial_t(uint64_t Address)> Read,
                                                           std::function<const std::vector<uint8_t>&()> Zone,
                                                           std::function<int64_t(uint64_t Address)> Off,
                                                           std::function<std::string(uint64_t Hash)> Name);

    // An asset's name from its 63-bit hash: the name indexes' (LoadNameIndexes), else
    // "<type>_<hex>"
    std::string HashedName(const char* Type, uint64_t Hash);
    // An asset name's 63-bit hash: FNV-1a 64 from 0x47f5817a5ef961ba of the lower-case name, '\' as
    // '/' (Black Ops 7's HashIWAsset; a Black Ops 6 weapon's and a Modern Warfare III mapinfo's hash
    // at 0 are their names' too)
    uint64_t AssetHash(const std::string& Name);
    // package_index's name indexes for these games (assets, strings, bones by their FNV-1a 64),
    // loaded once
    void LoadNameIndexes();

    // A map's entity string from a GfxEntityPairs (0x40) at Pairs, one "{ "key" "value" ... }" per
    // entity, its keys named by IwEntityKeys (Scheme): Off maps an address to a position of Img,
    // Ptr reads the pointer at a position; Count gets the entities
    // A pair is a u32: its type in the top 4 bits, its key's index from KeyShift up to bit 27, its
    // value (a string pool offset, a vector's index) in the low AtBits bits
    struct PairPacking
    {
        int AtBits = 16, KeyShift = 20;   // Black Ops 6: {u16 at, u8 key low bits << 4, u8 type << 4 | key high bits}
        uint32_t NewEntity = 9;           // the type that starts an entity
        uint32_t Vector = 2;              // the type of a vector (three floats at 0x30)
    };
    std::string EntityText(const std::vector<uint8_t>& Img, int64_t Pairs, const std::function<int64_t(uint64_t)>& Off,
                           const std::function<uint64_t(int64_t)>& Ptr, size_t& Count, IwEntityKeys::Scheme KeyScheme,
                           const PairPacking& Packing = {});
    // The same entities as key/value pairs, in their order (EntityText writes them as text)
    using MapEntity = std::vector<std::pair<std::string, std::string>>;
    void EntityPairs(const std::vector<uint8_t>& Img, int64_t Pairs, const std::function<int64_t(uint64_t)>& Off,
                     const std::function<uint64_t(int64_t)>& Ptr, std::vector<MapEntity>& Out, IwEntityKeys::Scheme KeyScheme,
                     const PairPacking& Packing = {});
    std::string EntityText(const std::vector<MapEntity>& Entities);
    // The mapents kind: a zone's entities, one line per pair "entity_index<TAB>key<TAB>value",
    // mapents/<zone>.tsv (a tab or line break in a value written as a space), indexed
    void ExportMapEnts(const DiskGame::Context& C, DiskGame::Totals& T, const std::string& Zone, const std::vector<MapEntity>& Entities);
    // A script string by the low 32 bits of its hash (what map_ents' records keep), from the strings
    // registered so far; "" when none
    std::string ScriptString32(uint32_t Hash);
}
