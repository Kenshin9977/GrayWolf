#pragma once

#include "ByteView.h"

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
        // the definition is the weapon asset itself (Modern Warfare 4: its Def fields at the weapon's offsets)
        bool DefIsWeapon = false;
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
        // further tunables, each column written only when the layout gives its field: the wait after
        // a burst (f32 ms, rounded half up), the base and aim-down-sight move scales (f32), the
        // magazines of reserve of a by-magazine weapon (i32: max_ammo = clip x it), the explosion's
        // centre and edge damages (i32 at +0 / +4) and its three-ring set (i32 radii +0 / +4 / +8,
        // damages +0xc / +0x10 / +0x14, used when ExplosionAlt is set); the ballistic block (its flag
        // u8 at +BallisticFlag, gravity factor f32 at +BallisticGravity, x GravityInches in/s2)
        Field BurstDelay, MoveSpeedScale, AdsMoveSpeedScale, ReserveClips, ExplosionDamage, ExplosionRings, BallisticBlock;
        int64_t BallisticFlag = -1, BallisticGravity = -1;
        float GravityInches = 0.0f;
        // attachment: its name string, clip scale (f32), the clip override block (i32 at 8), the modifier
        // table (blocks at 0x08, 0x18, 0x68, 0x90, 0xd0), the ADS times override block, the explosion
        // radius override block (i32 at 0, the alternate radius at AttExplosionAlt)
        int64_t AttachmentName = -1, AttClipScale = -1, AttClipOverride = -1, AttModifiers = -1, AttAdsOverride = -1, AttExplosion = -1,
                AttExplosionAlt = -1;
        // a clip scale of 0 is no scale (the code skips it: Modern Warfare 2019), written as 1
        bool AttZeroScaleIsNone = false;
        // attachment, further: the burst delay override (an i32 * at AttBurstDelay; not from an
        // attachment whose u8 AttAltMode is 1, an alternate mode's) and the modifier table block whose
        // f32 +4 scales it; in the explosion block, the centre / edge damages (i32 at +0 / +4 from
        // AttExplosionDamage), the three-ring set (as ExplosionRings, from AttExplosionRings) and the
        // u8 that picks it (AttExplosionFlag); the ballistic block pointer (the tunables' block replaced)
        int64_t AttBurstDelay = -1, AttAltMode = -1, AttBurstDelayScale = -1, AttExplosionDamage = -1, AttExplosionRings = -1,
                AttExplosionFlag = -1, AttBallistic = -1;
        // attachment, damage by range and hits (each table's first record, the one the stats UI shows):
        // the damage table override (a block laid out as the tunables' damage table: i32 damages 0-0x10,
        // f32 ranges 0x14-0x20; the first attachment holding one replaces the weapon's), the per-record
        // scale table (f32: the 5 damage scales, then the 4 range scales; multiplied over the
        // attachments), the modifier table block whose f32 +4 scales the 5 damages (an attachment
        // without a scale table), the f32 range scale (likewise; 0 none), the hit location table
        // override (22 f32, the first attachment's); penetration: a block {u8 level, f32 depth scale}
        // (the first attachment's replaces the weapon's) and an i32 added to the level (summed); the
        // perks the attachment gives (u32 script string hashes, count u8 AttPerkCount)
        int64_t AttDamageTable = -1, AttDamageScales = -1, AttDamageScale = -1, AttRangeScale = -1, AttHit = -1, AttPenetration = -1,
                AttPenetrationAdd = -1, AttPerks = -1, AttPerkCount = -1;
        // the weapon's penetration: the level (u8 in its definition) and the depth scale (f32 in the
        // weapon asset); a level is clamped to [1, PenetrationMax] once an attachment adds to it
        int64_t DefPenetration = -1, WeaponPenetrationScale = -1;
        int PenetrationMax = 0;
        // attachment, fire time: the i32 that replaces the tunables' fire time (0 none; the first
        // attachment, not an alternate mode's, AttAltMode) and the modifier table block whose f32 +4
        // scales it (1 + the sum of the attachments' scale - 1)
        int64_t AttFireTime = -1, AttFireTimeScale = -1;
        // the shot cycle beyond the fire time: fire delay (i32 ms; attachment block i32 override, f32 scale
        // pointer product), bolt action (u8; attachment block u8) with the package's rechamber (u16), a
        // charge (u8 kind, f32 s time x f32 fraction, f32 s wait after a full charge else the alt wait;
        // attachment block override, f32 time scale product). Proofs: the game's README columns
        // (BoltReloadOnly: u8, set = no rechamber between shots)
        Field FireDelay, BoltAction, BoltReloadOnly, ChargeKind, ChargeTime, ChargeFraction, ChargeCooldown, ChargeCooldownAlt;
        int64_t AttFireDelay = -1, AttFireDelayAt = -1, AttFireDelayScale = -1, AttBolt = -1, AttBoltAt = -1, AttCharge = -1,
                AttChargeKind = -1, AttChargeTime = -1, AttChargeCooldown = -1, AttChargeCooldownAlt = -1, AttChargeScale = -1;
        // the order the game gathers a weapon's attachments (slot indexes): "the first attachment holding
        // a field" is the first in this order (empty: the order the attachments are given)
        std::vector<int> SlotOrder;
        // the alternate mode slot (-1 none): its attachment counts only while the mode is active, unless
        // its u8 AttAltNormal is set (a normal attachment); u8 AttAltActive (0: the mode is never active,
        // its fields never apply), u8 AttAltWithOthers (0: while active, the weapon's stats are the
        // attachment's alone)
        int AltSlot = -1;
        int64_t AttAltActive = -1, AttAltWithOthers = -1, AttAltNormal = -1;
        // (or the u8 at +AttAltBlockKeep of the block AttAltBlock points to, when set)
        int64_t AttAltBlock = -1, AttAltBlockKeep = -1;
        // the clip override replaces the base clip and is then scaled (true), or replaces the result (false)
        bool ClipOverrideScaled = false;
        // max ammo: the attachment's f32 scale (1 + the sum of scale - 1) and, in the clip override
        // block, the i32 max ammo, the i32 reserve magazines and the u8 that counts the reserve in magazines
        int64_t AttMaxAmmoScale = -1, AttAmmoMax = -1, AttAmmoReserve = -1, AttAmmoByClips = -1;
        // pellets a shot: the tunables' i32, replaced by the first attachment's clip override block i32
        // +AttAmmoPellets when >= 0
        Field Pellets;
        int64_t AttAmmoPellets = -1;
        // the attachment's u8 weapon class override (0 none; the first in gather order wins); pellets only for SpreadClass
        int64_t AttClass = -1;
        int SpreadClass = -1;
        // Zombies Pack-a-Punch: the bundle type (as the bundles kind names it; "" none) and its keys' FNV-1a
        // 64 hashes: weapon, base, pap, pap4 attachments, the variants array and, in an item, the attachment
        // held, the pap and pap4 variants
        std::string PapBundleType;
        uint64_t PapKeys[8] = {};
        // a weapon's custom settings (weapon_custom_settings/<weapon>.json): the script bundle pointer in
        // its definition (-1 none; the bundles kind must write the bundles in the same --out); the Pack-a-Punch
        // damage multipliers of levels 1-4 the Zombies damage reads: a weaponmetadataentry's own (PapEntryScales),
        // else its list's (a PapListType bundle: entries in the array PapListKeys[0], each an asset PapListKeys[1];
        // the four scales PapListKeys[2..5])
        int64_t DefCustomSettings = -1;
        std::string PapListType;
        uint64_t PapEntryScales[4] = {}, PapListKeys[6] = {};
        // README.txt: the custom settings' notes
        std::string CustomSettingsNotes;
        // compatibility: an attachment's group (u32), its own id (u32: the low 32 bits of its name's
        // script string hash), the groups it excludes (u32 * AttExGroups, count u8 AttExGroupCount), the
        // slots it excludes (u32 *, a slot named by SlotId(SlotNames[slot])), the attachments it excludes
        // by own id (u32 inline, count u8); the attachments it brings (inline 16-byte entries, the id
        // u64 at 0, count u8 AttBringCount)
        int64_t AttGroup = -1, AttSelfId = -1, AttExGroups = -1, AttExGroupCount = -1, AttExSlots = -1, AttExSlotCount = -1, AttExIds = -1,
                AttExIdCount = -1, AttBrings = -1, AttBringCount = -1;
        std::vector<std::string> SlotNames;
        std::function<uint32_t(const std::string&)> SlotId;
        // the rules' notes (README.txt, weapon_attachment_rules.csv)
        std::string RulesNotes;
        // animation package: the anim table's pointer, its i64 count, the sorted u16 anim ids and the
        // xanim pointers of the same index (weapon_anims.csv); an xanim named by its hash (AnimName)
        int64_t PkgAnims = -1, PkgAnimCount = -1, PkgAnimIds = -1, PkgAnimXanims = -1;
        std::function<std::string(uint64_t)> AnimName;
        // the default parts (weapon_default_parts.csv): a weapon's default attachment of slot s (1 to
        // SlotCount - 1) at DefaultAttachments + 8 s (an attachment pointer); an attachment's own models
        // (the part with no blueprint), 8-byte xmodel name hashes (0: none), named by ModelName
        int64_t DefaultAttachments = -1, AttWorldModel = -1, AttViewModel = -1;
        // or (Modern Warfare III) the defaults as a slot table: DefaultSlotCount entries at DefaultSlots +
        // 0x10 s {u32 count 0, attachment ** 8}, every attachment of an entry's list a default of slot s;
        // and the attachment's models as arrays (AttWorldModel, AttViewModel: a pointer to xmodel pointers,
        // the first the part's own) of AttModelCount (u32) entries
        int64_t DefaultSlots = -1, DefaultSlotCount = 0, AttModelCount = -1;
        std::function<std::string(uint64_t)> ModelName;
        // animation package: its time block's pointer, the u16 times in it
        int64_t PkgBlock = -1, PkgFirstRaise = -1, PkgReloadEmpty = -1, PkgReload = -1, PkgSprintOut = -1, PkgSuperSprintOut = -1,
                PkgRaise = -1, PkgDrop = -1, PkgRaiseQuick = -1, PkgReloadAdd = -1, PkgReloadEmptyAdd = -1, PkgReloadStart = -1,
                PkgReloadEnd = -1, PkgRechamber = -1;
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
        std::string Title, Columns, AttachmentNotes, DefaultPartsNotes, AnimNotes;
    };

    // What a zone gives the facts reader
    struct ZoneView
    {
        const ByteView* Image = nullptr;
        const ZoneTables::ZoneResult* Walk = nullptr;
        std::function<int64_t(uint64_t)> Off;              // a relocated pointer's zone offset, -1 elsewhere
        std::function<std::string(int64_t)> Text;          // the decrypted text of the string pointer field at a zone offset
        std::function<std::string(int64_t)> LocalizeText;  // a localize asset's decrypted text, by its position
        std::function<std::string(uint32_t)> ScriptString; // a script string by the low 32 bits of its hash ("" unknown)
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
