#pragma once

// The weaponstats and weapons kinds of the Infinity Ward engine's games after MW3 that keep the
// classic WeaponDef (Ghosts, Advanced Warfare, MW Remastered, Infinite Warfare, WWII): their
// exes keep no weapon field table, so each game gives one (Layout::Fields: the weapon file's
// field name, where the asset keeps it, its unit), checked field by field (the game's README
// says how). Each weapon is written as the weapon file those fields would make
// (WEAPONFILE\<field>\<value>..., times in seconds, distances in inches, as the mod tools write
// them) and handed to WeaponFiles, the older games' table: same columns, same README.

#include "Games/Common/WeaponFiles.h"
#include "Games/IW/IwWeaponStats.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace IwWeaponDefs
{
    // A field: the weapon file's name for it (fireTime, clipSize...: OAT's WeaponFields.h names of the
    // older games), where the asset keeps it. Its unit comes with the name, as the weapon file has it:
    // the *Time fields i32 milliseconds (written in seconds), the ranges and scales f32, weaponType,
    // weaponClass and fireType i32 enums (written by the layout's names), locationDamageMultipliers the
    // pointer to an f32 per hit location (written loc<Location>), the others i32
    struct Field
    {
        std::string Name;
        int64_t At = -1;
        bool InDef = true;   // in the WeaponDef (else in the weapon asset, a WeaponCompleteDef)
        // when the value is 0: the i32 at this offset times clipSize (WWII's ammo, a count of magazines)
        int64_t TimesClip = -1;
        bool Byte = false;   // a one-byte flag (0 or 1), not an i32
        bool Real = false;   // an f32, whatever its name says
        // written only when the i32 at this offset of the same struct is not 0 (-1: always): a field that
        // means nothing without another (Infinite Warfare's charge, without a charge type)
        int64_t If = -1;
    };
    struct Layout
    {
        // the weapon asset: its name string, display name string (a localize key), the WeaponDef
        // pointer (-1: the asset is the WeaponDef itself)
        int64_t Name = 0, Display = -1, Def = -1;
        // the asset's pointer width: 8, or 4 (MW2's and MW3's original 32-bit builds)
        int PointerSize = 8;
        std::vector<Field> Fields;
        // a middle damage stop whose range is 0 (damageRange<n>): false, the stop is unused (Infinite
        // Warfare's); true, its damage holds up to the next stop's range and the next damage is unused
        // (WWII's damage code)
        bool ZeroRangeExtends = false;
        // the times of the weapon's animation package: its pointer in the WeaponDef (-1: none), the
        // pointer to its u16 timer block in the package, and each weapon file time field (fireTime's
        // unit, milliseconds) by its byte offset in that block
        int64_t AnimPkg = -1, AnimTimers = -1;
        std::vector<std::pair<std::string, int64_t>> AnimTimes;
        // the package's asset type: each one the zone defines is written as an animation package file
        // (its name at 0), joined by name to the weapons of another zone that name it (a name-only header)
        std::string AnimPkgType;
        // the exe's name tables: weapType, weapClass, fireType values; the hit locations (index 0 none:
        // the same 22 in each of these exes)
        std::vector<std::string> Types, Classes, FireTypes;
        // the chargeType values (Infinite Warfare's), by the exe's table
        std::vector<std::string> ChargeTypes;
        std::vector<std::string> HitLocations = { "none", "helmet", "head", "neck", "torso_upper", "torso_lower", "right_arm_upper",
                                                  "left_arm_upper", "right_arm_lower", "left_arm_lower", "right_hand", "left_hand",
                                                  "right_leg_upper", "left_leg_upper", "right_leg_lower", "left_leg_lower", "right_foot",
                                                  "left_foot", "gun", "shield", "armor", "soft" };
        // locationDamageMultipliers is the f32 array itself, inline in the asset (Call of Duty Online's
        // WeaponDef), not a pointer to it
        bool HitsInline = false;
        // the weapon file field each written field's offset goes in ("name 0x<offset>;" per field), for
        // the attachment rules that address fields by offset (Call of Duty Online's codol.offsets; WWII's
        // s2.offsets are the attachment rules' own); "" none
        std::string OffsetsField;
        // The models and anims the weapon file names, as OpenAssetTools' weapon files do (MW2's and
        // MW3's original builds, for the anim owners: Games/IwClassic/IwClassicAnimOwners.h): the
        // WeaponDef's gun model array (gunModel: its first) and hand model (handModel), each a model
        // pointer, written by the model's name; the anim name lists, each a pointer to AnimNames.size()
        // string pointers in the weapon asset or its WeaponDef, the k-th written as AnimNames[k] +
        // its Suffix ("" not written); MW3's animOverrides (WeaponCompleteDef: the u32 count, the
        // pointer to the entries of AnimOverrideSize bytes: two u16 attachment combinations, the anim
        // and alt-mode anim string pointers, the u32 anim type, the two times), a line each:
        // attachment, attachment, type, anim, alt-mode anim, times, as OAT writes them. -1: none.
        int64_t GunModels = -1, HandModel = -1;
        struct AnimList
        {
            int64_t At = -1;
            bool InDef = false;
            std::string Suffix;
        };
        std::vector<std::string> AnimNames;
        std::vector<AnimList> AnimLists;
        int64_t AnimOverrideCount = -1, AnimOverrides = -1, AnimOverrideSize = 0;
        WeaponFiles::Notes Notes;
        // WWII's attachments (WeaponFiles' s2 rules): each attachment asset written as an attachment file
        // (ATTACHMENTFILE\s2.format\1\displayName\..\attachmentType\<its type>\s2.alt\..\s2.overrides\<offset
        // operation value per line>, the overrides of the layout's fields only), and in each weapon file its
        // attachment list (attachments), its defaults (s2.defaults), its reload animation overrides
        // (s2.animOverrides) and each field's offset (s2.offsets). Off when List < 0.
        struct AttachmentRules
        {
            int64_t List = -1, Count = -1;      // WeaponDef: the attachment pointers, their u8 count
            int64_t Defaults = -1;              // WeaponDef: DefaultCount u8 list indices (1-based)
            int DefaultCount = 0;
            int64_t Anims = -1, AnimCount = -1; // WeaponDef: the 32-byte animation overrides, their u8 count
            std::vector<int> AnimStates;        // the states kept (reload, reload empty)
            // the attachment: its type (i32), its alternate mode (i32, u8: either set), the override offsets
            // (u16 list), entries (16 bytes) and count (i32)
            int64_t Type = 0x10, AltA = 0x18, AltB = 0x88, Offsets = 0x58, Entries = 0x60, EntryCount = 0x68, DisplayName = 8;
            std::string ComboTable;             // the string table of the class builder's pairs (lower case)
            // whose rules (s2.game: "s2" WWII, "s1" Advanced Warfare, whose attachments keep the same override
            // lists); attachments every weapon's list is given too (rows computed for them: the zombies' level
            // attachments the scripts add by name, not in the weapons' lists)
            std::string Rules = "s2";
            std::vector<std::string> Extra;
        } Attachments;
        // Infinite Warfare's attachments (WeaponFiles' iw7 rules): the weapon asset's attachment lists (a
        // pointer to attachment pointers, its u32 count), written as the weapon file's attachments in that
        // order; each attachment asset written as an attachment file (ATTACHMENTFILE\iw7.format\1\...) with
        // the blocks and scales the exe's accessors read: its kind (i32), the damage table (a block: i32
        // damages 0-0x10, f32 ranges 0x18-0x24), the hit location table (22 f32), the ammo block (i32 max,
        // start, clip), the ammo scale, range scale, fire time and its scale; the weapon type it gives (i32, 0
        // none: named by Types) and its projectile block (pointer: i32 explosion radius 0, inner and outer
        // damage 8 and 0xc, projectile speed 0x14), its charge block (pointer: i32 type 0, f32 gain 4, cost
        // per shot 8, loss when idle 0xc, empty cooldown 0x10), its weapon class (i32, named by Classes), its ammo
        // general block (pointer: i32 fire type 0xc, named by FireTypes), its general block (pointer: u8 bolt
        // action 0), its burst block (pointer: i32 burst cooldown 0) and burst cooldown scale (f32), its ammo used
        // per shot (i32, 0 none) and its ammo block's requireAmmoUsedPerShot (u8 0x18). The alternate
        // mode's list (AltList, among Lists: the gather 0x743cf0 leaves its attachment out of the primary mode
        // unless its class is none) written apart (iw7.altSlot). Off when Lists is empty.
        struct Iw7Attachments
        {
            std::vector<std::pair<int64_t, int64_t>> Lists;
            int64_t AltList = 0x58;
            int64_t DisplayName = 8, Kind = 0x18, WeaponType = 0x1c, Class = 0x20, Ammo = 0xe8, Damage = 0xf0, Projectile = 0x160,
                    Charge = 0x168, Hits = 0x190, AmmoGeneral = 0x88, General = 0xc8, Burst = 0x188,
                    AmmoScale = 0x198, RangeScale = 0x19c, FireTime = 0x1a8, FireTimeScale = 0x1b0, BurstScale = 0x1b4,
                    AmmoPerShot = 0x1f0;
        } Iw7;
    };

    // A game's README notes: its name, the language zone of the display names, the zone the fields were
    // checked on, where they are read (Where) and the known numbers they were checked against
    WeaponFiles::Notes GameNotes(const std::string& Title, const std::string& LanguageZone, const std::string& Checked, const std::string& Where,
                                 const std::string& Known);

    // The zone's weapons as weapon files (ExportStats keeps those the run wants)
    std::vector<WeaponFiles::Weapon> Weapons(const Layout& L, const IwWeaponStats::ZoneView& V);
    // The game's notes for WeaponFiles::ExportStats: its own, and the columns its layout adds to the
    // shared ones (the hit locations after the gun, burstCount)
    WeaponFiles::Notes StatsNotes(const Layout& L);
    // The weaponstats kind: the weapon files and the zone's strings to WeaponFiles::ExportStats
    void Export(const Layout& L, const IwWeaponStats::ZoneView& V, const std::vector<std::pair<std::string, std::string>>& Strings,
                const std::string& Zone, const DiskGame::Context& C, DiskGame::Totals& T, const std::map<std::string, std::string>& Tables = {});
    // The layout's fields as the weapons kind's struct dumps name them (ZonekitZone::DumpType::Fields)
    std::map<std::string, ZonekitZone::DumpField> DumpFields(const Layout& L);
}
