#pragma once

// The weapon stats kind (DiskGame::Kind::WeaponStats) of the iwctx family: one row per weapon and
// tunables set, in user units, only fields whose meaning the game's exe proves. The values sit in
// several zones (the guns in global_stream_mp, their animation packages in ingame_shared_mp, the
// suits and game-mode settings in global_shared_mp, the names in ens_global), so each zone writes
// what it holds as facts (<kind>/facts/<zone>.json) and Merge joins every facts file of the folder
// into weapon_stats.csv, weapon_stats.json and README.txt (and, when the layout gives them, the
// attachment tables and weapon_default_parts.csv: each weapon's default attachments and their models).
// Each game gives its Layout: where its assets keep each field (-1: not proven for that game, the
// field is left out of the facts and its column stays empty) and the README's text with its exe's
// proofs.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "DiskGame.h"
#include "ZoneTableWalker.h"
#include "ZonekitZone.h"

namespace IwWeaponStats
{
    // A tunables field: its offset in the weapontunables asset, or in the weapon's definition or the
    // weapon asset itself (a game that keeps no tunables asset)
    struct Field
    {
        enum class In : uint8_t { Tunables, Def, Weapon };
        int64_t At = -1;
        In Base = In::Tunables;
        constexpr Field() = default;
        constexpr Field(int64_t A, In B = In::Tunables) : At(A), Base(B) {}
    };

    struct Layout
    {
        // weapon: its name string, its definition, the display name (a localize hash, or its key's
        // string when DisplayIsKey: hashed by LocalizeHash); the attachments by slot (Slots + slot *
        // 0x10: the attachment ** at SlotList, the u32 count at SlotCountAt), SlotCount slots
        int64_t WeaponName = -1, WeaponDef = -1, WeaponDisplay = -1, Slots = -1, SlotList = 0, SlotCountAt = 8;
        int SlotCount = 0;
        bool DisplayIsKey = false;
        std::function<uint64_t(const std::string&)> LocalizeHash;
        // the bits of a localize asset's hash at 0 that name it (the others: flags)
        uint64_t StringIdMask = 0x7FFFFFFFFFFFFFFFull;
        // a game whose assets are named by a string, not a hash at 0 (Modern Warfare 2019): the name
        // string's offset (its FNV-1a 64 stands as the asset's id; a name-only header's ',' left out),
        // the localize assets keyed by LocalizeHash of the key string at 0; -1: hashes
        int64_t AssetNameAt = -1;
        // the weapon class names by the class value (empty: the family's list, none first)
        std::vector<std::string> ClassNames;
        // weapon definition: class u8, base animation package (asset), the tunables sets {tunables
        // **, u32 count, tags {u32 count, u64 *} per set} and the single tunables asset, the animation
        // package overrides (0x30 each) and their count
        int64_t DefClass = -1, DefAnimPkg = -1, DefSets = -1, DefSetCount = -1, DefSetTags = -1, DefTunables = -1, DefOverrides = -1,
                DefOverrideCount = -1;
        // the tunables (a game without tunables assets, DefSets and DefTunables -1: one set per weapon,
        // read from its definition and itself): i32 times in ms, counts; u8 kinds; f32 scales; the
        // damage table's first record (i32 damages 0-0x10, f32 ranges 0x14-0x20) and the 22 hit
        // location multipliers (f32)
        Field FireTime, BurstCount, StartAmmo, StartAmmoOverride, ClipSize, MaxAmmo, MaxAmmoByClips, FireType, AdsTime, AdsOutTime, ReloadType,
            ExplosionRadius, ExplosionRadiusAlt, ExplosionAlt, MuzzleVelocity, Damage, Hit, SprintOutScale, SprintSpeedScale,
            SuperSprintSpeedScale, ProneSpeedScale;
        // the hit location multipliers behind a pointer at Hit (Modern Warfare 2019), not inline
        bool HitIsPointer = false;
        // attachment: its name string, clip scale (f32), the clip override block (i32 at 8), the modifier
        // table (blocks at 0x08, 0x18, 0x68, 0x90, 0xd0), the ADS times override block, the explosion
        // radius override block (i32 at 0, the alternate radius at AttExplosionAlt)
        int64_t AttachmentName = -1, AttClipScale = -1, AttClipOverride = -1, AttModifiers = -1, AttAdsOverride = -1, AttExplosion = -1,
                AttExplosionAlt = -1;
        // a clip scale of 0 is no scale (the code skips it: Modern Warfare 2019), written as 1
        bool AttZeroScaleIsNone = false;
        // the default parts (weapon_default_parts.csv): a weapon's default attachment of slot s (1 to
        // SlotCount - 1) at DefaultAttachments + 8 s (an attachment pointer); an attachment's own models
        // (the part with no blueprint), 8-byte xmodel name hashes (0: none), named by ModelName
        int64_t DefaultAttachments = -1, AttWorldModel = -1, AttViewModel = -1;
        std::function<std::string(uint64_t)> ModelName;
        // animation package: its time block's pointer, the u16 times in it
        int64_t PkgBlock = -1, PkgFirstRaise = -1, PkgReloadEmpty = -1, PkgReload = -1, PkgSprintOut = -1, PkgSuperSprintOut = -1,
                PkgRaise = -1, PkgDrop = -1, PkgRaiseQuick = -1, PkgReloadAdd = -1, PkgReloadEmptyAdd = -1, PkgReloadStart = -1,
                PkgReloadEnd = -1;
        // a time of 0xffff is none (left out of the facts: Modern Warfare 2019's getter skips it)
        bool PkgFfffIsNone = false;
        // suit: its name hash, the sprint and super sprint multipliers (f32)
        int64_t SuitId = -1, SuitSprint = -1, SuitSuperSprint = -1;
        // gametype settings keys (a scriptbundle's): the tunables tag, the suit; an attachment info
        // bundle's attachment and display name (0: none)
        uint64_t TagKey = 0, SuitKey = 0, AttachmentKey = 0, NameKey = 0;
        // the tunables tags' names (their FNV-1a 64 is the tag)
        std::vector<std::string> TagNames;
        // the base speed the weapon stats UI starts from (0: not proven), the suit of the multiplayer
        // settings ("" none), an asset name's 63-bit hash (a suit's)
        float BaseSpeed = 0.0f;
        std::string MpSuit;
        std::function<uint64_t(const std::string&)> NameHash;
        // README.txt: its title, the columns' documentation, the attachments', the default parts' (with
        // the exe's proofs)
        std::string Title, Columns, AttachmentNotes, DefaultPartsNotes;
    };

    // What a zone gives the facts reader
    struct ZoneView
    {
        const std::vector<uint8_t>* Image = nullptr;
        const ZoneTables::ZoneResult* Walk = nullptr;
        std::function<int64_t(uint64_t)> Off;              // a relocated pointer's zone offset, -1 elsewhere
        std::function<std::string(int64_t)> Text;          // the decrypted text of the string pointer field at a zone offset
        std::function<std::string(int64_t)> LocalizeText;  // a localize asset's decrypted text, by its position
    };
    struct FactsCount
    {
        size_t Weapons = 0, Tunables = 0, AnimPkgs = 0, Suits = 0, Settings = 0, Strings = 0, Attachments = 0;
    };
    // The zone's facts as JSON text; WantsWeapon filters the weapons by name (the rest is kept)
    std::string Facts(const Layout& L, const ZoneView& V, const std::string& Zone, const std::function<bool(const std::string&)>& WantsWeapon,
                      FactsCount& N);
    // Joins every <Dir>/facts/*.json into <Dir>/weapon_stats.csv, .json and README.txt (and the
    // attachment and default parts tables)
    bool Merge(const Layout& L, const std::string& Dir, size_t& Rows, std::string& Error);
    // The kind's exporter: the zone's facts written to <kind>/facts/<Zone>.json, the table rebuilt
    // from every facts file once the run's zones are done (Context::After)
    void Export(const Layout& L, const ZoneView& V, const std::string& Zone, const DiskGame::Context& C, DiskGame::Totals& T);
    // The layout's fields as the weapons kind names them in its struct dumps (ZonekitZone::DumpType::
    // Fields) for an asset type: weapon, weapontunables, attachment, animpkg, suit
    std::map<std::string, ZonekitZone::DumpField> DumpFields(const Layout& L, const std::string& Type);
}
