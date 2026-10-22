#include "stdafx.h"

// Black Ops 7 on the disk template: a ZonekitZone with the bo7 profile (zone format iwctx), served to
// Black Ops 7's own readers (GameBlackOps7.cpp). The install is plain Steam files (Call of Duty HQ,
// cod25): each .ff with its .fp update patch beside it (Games/IW/Container25.cpp applies it), the
// streamed meshes, images and sounds in its .xsub packages (XSUBCache, version 23).
// What is Black Ops 7's own: pointers DB_RewindPrepare decides (linked by the walker, ZoneTableWalker
// PointerIf), assets named by a 63-bit hash, the LOD surfaces the game copies at load.

#include "CoDAssets.h"
#include "DiskGame.h"
#include "Games/BO7/TechsetFormat.h"
#include "Games/IW/IwWeaponStats.h"
#include "ExportWriters.h"
#include "FastFile.h"
#include "FileSystems.h"
#include "GameBlackOps7.h"
#include "HalfFloats.h"
#include "MapLive.h"
#include "Games/Common/MeshDecode.h"
#include "Games/Common/StringCipher.h"
#include "Games/IW/IWZone.h"
#include "Games/IW/IwBesideZones.h"
#include "Games/IW/IwctxReaders.h"
#include "Games/IW/IwLanguageNames.h"
#include "Games/IW/IwTerrain.h"
#include "Games/Treyarch/TreyarchPackages.h"
#include "SettingsManager.h"
#include "Strings.h"
#include "WraithNameIndex.h"
#include "XSUBCache.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{
    // XModel (0xd8): its name at 8, the LODs' pointer 0x90 (u8 0x12 records of 0x48: the
    // xmodelsurfs at 0, the surfaces the game copies from it at 8); XModelSurfs keeps them at 8
    constexpr int64_t ModelName = 8, ModelLodCount = 0x12, ModelLods = 0x90, LodSize = 0x48, LodSurfs = 8, ModelSurfsSurfs = 8;

    class BO7Zone final : public IWZone
    {
    public:
        explicit BO7Zone(const std::string& Path) : IWZone(Path)
        {
            using namespace DiskGame;
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            // gscobj (0x18, 0x3861c50): {XHash name, u32 len 8, buffer 0x10}, a GSCOBJ acts reads
            Exports(Kind::Script, [this](const Context& C, Totals& T) {
                ExportScripts(C, T, Scripts::Acts, { { "gscobj", [this](int64_t Pos, Scripts::Script& S) {
                                  S.Compiled = true;
                                  return BytesAt(PointerAt(Pos + 0x10), At<uint32_t>(Image(), Pos + 8), S.Bytes);
                              } } });
            });
            // rawfiles, luafile (0x60) {hash, i32 size 0x3c, buffer 0x40} and string tables, their
            // strings encrypted as the localized ones
            ExportsHashedRawfiles(0x3c, 0x40);
            ExportsMaterialLinks({ 0x68, 0x6d, 0x28, 0x30, 0x20, 0x18, 0x6c, 0x40, 0x6e, 0x88 });
            // soundbank (0x40): aliases 0x20 (u32 count 0x34) of 0x48 {entries 0x10, hash 0x18, u16
            // count 0x34}, an entry 0x30 with its sndasset at 0x20
            ExportsSoundLinks({ 0x20, 0x34, 0x48, 0x18, 0x10, 0x34, 0x30, 0x20 });
            ExportsTechsets(BO7::TechsetFormat());
            const auto Decrypted = [this](const std::string& Raw) {
                std::string Error, Text;
                const auto* Tables = ExportWriters::ZoneProfile(Profile(), Error);
                const auto* Cipher = Tables != nullptr ? StringCipher::Load(Tables->Profile.Dir, Error) : nullptr;
                return StringCipher::Decrypt(Cipher, Raw, Text) ? Text : Raw;
            };
            // scriptbundle (0x30, its loader 0x3887630, a table's 0x3887a40, a value's 0x3888060)
            ExportsScriptBundles({}, Decrypted);
            // mapinfo (0xe8, its loader 0x386cd80): what getactivemapinfobundle() returns, a
            // scriptbundle's table at 0xc8 (the table loader 0x3887a40: destructibles, their loot...);
            // string fields 8 to 0x30, 0x40, 0x48 (8 the map, 0x10 its game types, 0x18 its scripts'
            // folder, 0x30 its loading image's name, 0x40 its omnvars table: their values on the 104
            // mapinfos of code_post_gfx and global), an image at 0x38. Named by HashIWAsset (no type
            // prefix): 0x8 is the name on 84 of them
            ExportsBundleTrees(Kind::MapInfo,
                               { { "mapinfo", 0xc8, -1,
                                   { { "map", 0x08 }, { "gametypes", 0x10 }, { "script_folder", 0x18 }, { "0x20", 0x20 }, { "0x28", 0x28 },
                                     { "loading_image_name", 0x30 }, { "omnvars_table", 0x40 }, { "0x48", 0x48 } },
                                   { { "loading_image", 0x38 } },
                                   [](const std::string& Name) { return GameBlackOps7::HashIWAsset(Name); } } },
                               {}, Decrypted);
            // aitype (0x178): a scriptbundle's table at 0xb0 (the table loader 0x3887a40), a string at
            // 0x78. Named by HashIWAsset("aitype:<name>")
            ExportsBundleTrees(Kind::AiTypes,
                               { { "aitype", 0xb0, -1, { { "0x78", 0x78 } }, {},
                                   [](const std::string& Name) { return GameBlackOps7::HashIWAsset("aitype:" + Name); } } },
                               {}, Decrypted);
            // scriptable (0xd8, its loader 0x3888d90) and scriptablevariant (0x58, 0x388f490): no
            // key/value table, dumped by offset
            ExportsStructDumps(Kind::Scriptables, { { "scriptable" }, { "scriptablevariant" } }, Decrypted);
            // the zone's entities, one TSV (IwctxReaders::ExportMapEnts): the entity strings, the spawn
            // points and the scriptable instances (MapEntities)
            Exports(Kind::MapEnts, [this](const Context& C, Totals& T) {
                std::vector<IwctxReaders::MapEntity> Ents;
                EntityStrings(Ents);
                MapEntities(Ents);
                IwctxReaders::ExportMapEnts(C, T, Name(), Ents);
            });
            ExportsAssetRefs(Decrypted);
            ExportsColumnTables(ColumnTypes::Bo6, Decrypted);
            // the weapon assets as struct dumps, a weapon's name at 8, an attachment's at 0x248, its
            // tunables, accuracy, blueprints named after it; the fields proven so far named (WeaponFields)
            ExportsStructDumps(Kind::Weapons,
                               { { "weapon", 8, WeaponFields() }, { "attachment", 0x248, AttachmentFields() }, { "weapontunables", -1, TunablesFields() },
                                 { "weaponaccuracy" }, { "weapontrigger" }, { "weaponarchetype" }, { "weaponblueprint", -1, WeaponBlueprintFields() },
                                 { "attachmentblueprint", -1, AttachmentBlueprintFields(), 0x40 },
                                 { "grappleweapontunables" }, { "deployableweaponconfig" }, { "animpkg", -1, AnimPkgFields() },
                                 // the suit a player state's 0x1a48 indexes (0x4528a40: DB find, type 0x51); 0x43cde50
                                 // returns 0xe18 as the sprint multiplier (0xdec under a perk; a dvar >= 0
                                 // overrides both, default -1.0), 0x43a28f0 multiplies 0xe78 in when super
                                 // sprinting. Its name hash is at 0x20 (+0 is a key table the zone's suits share)
                                 { "suit", -1, { { "0x0e18", { "sprint_speed_multiplier", "f32" } }, { "0x0e78", { "super_sprint_speed_multiplier", "f32" } } }, 0x20 } },
                               Decrypted);
            // the weapon stats table: this zone's facts, then the table rebuilt from every zone's (Games/IW/IwWeaponStats.h)
            Exports(Kind::WeaponStats, [this, Decrypted](const Context& C, Totals& T) {
                LearnIndexedModels();
                IwWeaponStats::ZoneView V;
                V.Image = &Image();
                V.Walk = &Walk();
                V.Off = [this](uint64_t A) { return Off(A); };
                V.Text = [this, Decrypted](int64_t Field) {
                    const std::string S = TextAt(Field);
                    return S.empty() ? S : Decrypted(S);
                };
                V.LocalizeText = [this](int64_t Pos) {
                    std::string Key, Text;
                    LocalizeEntry(Pos, Key, Text);
                    return Text;
                };
                IwWeaponStats::Export(StatsLayout(), V, fs::path(FastFilePath()).stem().string(), C, T);
            });
        }

        // ---- the default parts' model names: a part's models are named by hash, most of them in the
        // transient zones (mp_av_* view, mp_aw_* world) the hash packages do not name; the install index
        // lists each model by the name it is exported under, so its hash names it. The index kept for the
        // install is read once (whatever tables built it: the names do not depend on them); none, no names.
        static std::unordered_map<uint64_t, std::string>& IndexedModels()
        {
            static std::unordered_map<uint64_t, std::string> Models;
            return Models;
        }
        void LearnIndexedModels() const
        {
            static std::once_flag Once;
            std::call_once(Once, [this] {
                if (Game == nullptr || Install.empty()) return;
                const auto Index = DiskGame::ZoneIndex::Kept(*Game, Options, Install);
                if (!Index) return;
                auto& Models = IndexedModels();
                for (size_t Z = 0; Z < Index->Zones().size(); Z++)
                    for (const auto& A : Index->Defined(Z))
                        if (A.K == DiskGame::Kind::Model) Models.emplace(GameBlackOps7::HashIWAsset(A.Name), A.Name);
            });
        }
        static std::string PartModelName(uint64_t Hash)
        {
            if (const auto It = IndexedModels().find(Hash & 0x7FFFFFFFFFFFFFFFull); It != IndexedModels().end()) return It->second;
            return GameBlackOps7::HashName("xmodel", Hash);
        }
        // ---- the weapon stats' layout (IwWeaponStats.h): every offset proven from the exe (RVAs of
        // bo7_flat.exe, PE timestamp 0x6aadce0e; graywolf-tools fields.md)
        static const IwWeaponStats::Layout& StatsLayout();

        // each sndasset, named by its hash (an index name in another of the game's languages when
        // the index lacks it: IwLanguageNames), decoded by GameBlackOps7::ReadXSound (Games/IW/IwctxReaders'
        // export, shared with Black Ops 6)
        void ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T)
        {
            static IwLanguageNames ByLanguage({ std::begin(GameBlackOps7::Languages), std::end(GameBlackOps7::Languages) });
            std::vector<IwctxReaders::SoundEntry> Found;
            std::vector<uint64_t> Hashes;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "sndasset"))
                {
                    Hashes.push_back(At<uint64_t>(Image(), I.Pos));
                    Found.push_back({ GameBlackOps7::HashName("xsound", Hashes.back()), Live().Addr((uint64_t)I.Pos),
                                      At<uint16_t>(Image(), I.Pos + GameBlackOps7::SoundRate), At<uint32_t>(Image(), I.Pos + GameBlackOps7::SoundFrames) });
                }
            if (const size_t N = ByLanguage.Rename(Hashes, Found)) Counter("sounds_named_by_language") += N;
            IwctxReaders::ExportSounds(C, T, Name(), Found);
        }

        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;
        // the zone's entity strings (the map's and its script brush models'), then the entities
        // map_ents keeps as records (spawn points, scriptable instances)
        void EntityStrings(std::vector<IwctxReaders::MapEntity>& Out) const;
        void MapEntities(std::vector<IwctxReaders::MapEntity>& Out) const;

        // ---- the weapon fields named in the weapons kind's dumps. Each is proven by the game's code
        // (the GSC builtin reading it, from its table at 0xcbf78a0 of the decrypted exe, names by their
        // script hash; addresses are RVAs of that exe, PE timestamp 0x6aadce0e) or by published stats
        // (codmunity.gg, base weapons, 2026-10-17) the dump equals on the weapons listed. Ranges in
        // inches (39.37 a metre), times in ms. Candidates not proven stay unnamed.
        static std::map<std::string, DumpField> WeaponFields()
        {
            std::map<std::string, DumpField> F = {
                // its name as GSC names it: equal to the asset's on all 241 weapons of global_stream_mp
                { "0x0008", { "name", "" } },
                // the weapon definition: weaponclipsize/weaponfiretime reach the tunables through
                // weapon->0x20->0x1b30 (0x4637cf8, 0x463ac27)
                { "0x0020", { "weapon_def", "" } },
                { "0x0020/0x1b30", { "tunables", "" } },
                // the localize key of its display name: sat_weapon/<name>, AK-27 for sat_ar_kite (ens_global)
                { "0x0040", { "display_name", "" } },
            };
            // slot s's default attachment, +0x3a0 + 8 s (0x4697c87, when the weapon blueprint gives none)
            for (int S = 1; S < StatsLayout().SlotCount; S++)
                F[std::format("0x{:04x}", StatsLayout().DefaultAttachments + 8 * S)] = { std::format("default_attachment_slot{}", S), "" };
            return F;
        }
        static std::map<std::string, DumpField> TunablesFields()
        {
            std::map<std::string, DumpField> F = {
                // ms between shots: weaponfiretime (0x463aca4, read with 0x5ce8, returned * 0.001)
                { "0x5cd4", { "fire_time", "i32" } },
                // weaponburstcount (0x4654000)
                { "0x5d1c", { "burst_count", "i32" } },
                // weaponstartammo (0x46795ca): 0x5d38 when not 0, else 0x5d34; weaponmaxammo (0x4669196)
                // takes 0x5d3c when not 0
                { "0x5d34", { "start_ammo", "i32" } },
                { "0x5d38", { "start_ammo_override", "i32" } },
                { "0x5d3c", { "max_ammo_override", "i32" } },
                // weaponclipsize (0x4637d06), scaled by the attachments' clip_size_scale
                { "0x5d48", { "clip_size", "i32" } },
                // Equal to every value the patch notes give (codmunity.gg, 2026-10-17), the mode's
                // tunables: m/s, M15 Mod 0 BR/RES 920, M10 Breacher BR/RES 450, M8A1 900, Sokol 545 BR/RES 890
                { "0x0030", { "muzzle_velocity", "f32" } },
                // ms, first of 4 times 0x462ee30 reads together (0 or less: a default); equal to every
                // aim-down-sight time the patch notes give: Akita BR/RES 210, M8A1 265, M10 Breacher 250,
                // Sturmwolf 45 160
                { "0x5dac", { "ads_time", "i32" } },
                // 0x461f8d0: sprint out time = max(1, (int)(animpkg time * this * the attachments' and perks'
                // scales)): M8A1 220 * 0.91 = 200, M10 Breacher 210 * 0.74 = 155 (patch notes)
                { "0x5a9c", { "sprint_out_time_scale", "f32" } },
                // 0x43a28f0 speed slot 2 (the speed times 0x43cde50's sprint multiplier, times the suit's
                // super sprint multiplier when super sprinting) goes through 0x439f4e0's scale built
                // from 0x5c14, or 0x5c28 when super sprinting
                { "0x5c14", { "sprint_speed_scale", "f32" } },
                { "0x5c28", { "super_sprint_speed_scale", "f32" } },
                // 0x439f4e0 entry e6c: every speed function (0x439ee80, 0x439f0a0, 0x43a05e0, 0x43a0fb0)
                // hands it to 0x439f3f0 as the stance-1 speed, and stance 1 is ps+0x1c bit 0, the bit
                // the getstance builtin (0x74f4e20) answers "prone" for
                { "0x5c08", { "prone_speed_scale", "f32" } },
                // u8 0x5dfc the fire type (weaponisauto, weaponissemiauto, weaponisbeam: 0x46559c0), u8
                // 0x5dfd bolt action (weaponisboltaction: 0x4662f50)
                { "0x5dfc", { "fire_type_u8.bolt_action_u8", "" } },
            };
            // Damage by range. A record (0x28): i32 damage min 0, mid1 4, mid2 8, mid3 0xc, max 0x10
            // (0x46386d0 copies them out, scaled by the attachments' damage scales), f32 ranges 0x14
            // (end of max), 0x18 (end of mid1), 0x1c, 0x20 (0x464a150, scaled by the attachments'
            // range scale in 0x464cb80). 0x464c9e0 picks: distance < range_max -> max, else the first
            // mid whose range is > 0 and > distance, else min (0x4636740: the same record lerped
            // between max and min). A table holds 3 profiles of 3 records (profile * 0x78 + record *
            // 0x28, both chosen by the caller); 0x46386d0 takes the table at 0x70, at 0x1d8 in mode 1,
            // at 0x340 in mode 2 when u8 0x52c is set, the 3 records at 0x4a8 in mode 3. What the
            // modes, profiles and records stand for is not proven: they are numbered.
            const auto Record = [&F](int64_t At, const std::string& Prefix) {
                static const char* Ints[] = { "damage_min", "damage_mid1", "damage_mid2", "damage_mid3", "damage_max" };
                static const char* Floats[] = { "range_max_damage", "range_mid1", "range_mid2", "range_mid3" };
                for (int K = 0; K < 5; K++) F[std::format("0x{:04x}", At + K * 4)] = { Prefix + Ints[K], "i32" };
                for (int K = 0; K < 4; K++) F[std::format("0x{:04x}", At + 0x14 + K * 4)] = { Prefix + Floats[K], "f32" };
            };
            for (const auto& [Table, Name] : { std::pair<int64_t, const char*>{ 0x70, "" }, { 0x1d8, "mode1." }, { 0x340, "mode2." } })
                for (int P = 0; P < 3; P++)
                    for (int R = 0; R < 3; R++)
                    {
                        // the first record of the first profile of the main table: the plain names
                        const std::string Prefix = *Name == 0 && P == 0 && R == 0 ? "" : std::format("{}p{}.r{}.", Name, P, R);
                        Record(Table + P * 0x78 + R * 0x28, Prefix);
                    }
            for (int R = 0; R < 3; R++) Record(0x4a8 + R * 0x28, std::format("mode3.r{}.", R));
            // The damage multiplier of a hit location: 22 floats indexed by the hit location, whose
            // names are the game's table at 0xb8f0620 (0xb1805f0 returns one; 0x737b330 hashes them
            // in that order, 0x737ae40 turns a name back into its index); 0x4642180 gives the table at
            // 0x530, at 0x588 in mode 2 when u8 0x67a is set, at 0x5e0 in mode 3, and the callers
            // multiply by table[index] (0x74e4912, 0x737b23b, 0x77004c8)
            static const char* Locations[] = { "none", "helmet", "head", "neck", "torso_upper", "torso_lower", "right_arm_upper",
                                               "left_arm_upper", "right_arm_lower", "left_arm_lower", "right_hand", "left_hand",
                                               "right_leg_upper", "left_leg_upper", "right_leg_lower", "left_leg_lower", "right_foot",
                                               "left_foot", "gun", "shield", "armor", "soft" };
            for (const auto& [Table, Name] : { std::pair<int64_t, const char*>{ 0x530, "" }, { 0x588, "mode2." }, { 0x5e0, "mode3." } })
                for (int K = 0; K < 22; K++)
                    F[std::format("0x{:04x}", Table + K * 4)] = { std::string(Name) + "hit_multiplier_" + Locations[K], "f32" };
            return F;
        }
        // An animpkg's u16 ms times (block 0x18), read by 0x46556b0 at a byte offset. 0x2:
        // weaponfirstraisetime (0x462cc60). 0xac / 0xae: getweaponreloadtime (0x735cdf0) with an
        // empty / non-empty clip (0x4673070 / 0x4675450); 0xb0 / 0xb2 when traits 0x45b / 0x458
        // hold; player weapon state bits 21-23 pick the other sets
        static std::map<std::string, DumpField> AnimPkgFields()
        {
            const std::pair<int, const char*> Times[] = {
                { 0x002, "first_raise_time" },
                { 0x0ac, "reload_empty_time" },
                { 0x0ae, "reload_time" },
                { 0x0b0, "reload_empty_time.trait45b" },
                { 0x0b2, "reload_time.trait458" },
                { 0x12a, "reload_empty_time.state21" },
                { 0x220, "reload_empty_time.state21.trait45b" },
                { 0x11e, "reload_time.state21" },
                { 0x214, "reload_time.state21.trait458" },
                { 0x172, "reload_empty_time.state22" },
                { 0x268, "reload_empty_time.state22.trait45b" },
                { 0x166, "reload_time.state22" },
                { 0x25c, "reload_time.state22.trait458" },
                { 0x1c4, "reload_empty_time.state23" },
                { 0x2ba, "reload_empty_time.state23.trait45b" },
                { 0x1b8, "reload_time.state23" },
                { 0x2ae, "reload_time.state23.trait458" },
                // sprint to fire: 0x45891e0 waits max(0x522 - time since sprint end (ps 0x4c8), 0x524 -
                // time since super sprint end (ps 0x4d0)); 0x4678c80 / 0x467a360 read them (0x50e instead
                // of 0x522 when flag 0xb439c26 is off), 0x461f8d0 scales by tunables 0x5a9c
                { 0x522, "sprint_out_time" },
                { 0x524, "super_sprint_out_time" },
                { 0x50e, "sprint_out_time.flag_off" },
            };
            std::map<int, std::pair<std::string, std::string>> Words;   // word offset -> low, high names
            for (const auto& [At, Name] : Times)
            {
                auto& W = Words[At & ~3];
                (At & 2 ? W.second : W.first) = Name;
            }
            std::map<std::string, DumpField> F;
            for (const auto& [At, W] : Words)
                F[std::format("0x0018/0x{:04x}", At)] = { (W.first.empty() ? "_" : W.first) + "|" + (W.second.empty() ? "_" : W.second), "u16x2" };
            return F;
        }
        static std::map<std::string, DumpField> AttachmentFields()
        {
            return {
                { "0x0248", { "name", "" } },
                // weaponclipsize (0x4637d88, 0x4637e2c): the weapon's clip size * (1 + the sum of the
                // attachments' scale - 1), or the u32 at +8 of the block at 0x400 when one sets it
                { "0x0400", { "clip_size_override", "" } },
                { "0x0b90", { "clip_size_scale", "f32" } },
                // its blueprints (0x4635510: index k > 0 is the k-th of the list +0x30, count +0x38) and own
                // models: 0x46356d0 returns +0x270 (view) or +0x268 (world) for no blueprint
                { "0x0030", { "blueprints", "" } },
                { "0x0268", { "world_model", "hash" } },
                { "0x0270", { "view_model", "hash" } },
            };
        }
        // an attachmentblueprint (a cosmetic variant of an attachment): its name hash +0x40 (0x4635510
        // compares it), its models +0x70 (view) and +0x78 (world) (0x46356d0)
        static std::map<std::string, DumpField> AttachmentBlueprintFields()
        {
            return { { "0x0040", { "name", "hash" } }, { "0x0070", { "view_model", "hash" } }, { "0x0078", { "world_model", "hash" } } };
        }
        // a weaponblueprint (a weapon's variant): 0x4697c87 takes slot s's attachment at +0x40 + 8 s and
        // its attachmentblueprint at +0xc8 + 8 s (that blueprint's name hash looked up in the attachment's list)
        static std::map<std::string, DumpField> WeaponBlueprintFields()
        {
            std::map<std::string, DumpField> F;
            for (int S = 1; S < StatsLayout().SlotCount; S++)
            {
                F[std::format("0x{:04x}", 0x40 + 8 * S)] = { std::format("attachment_slot{}", S), "" };
                F[std::format("0x{:04x}", 0xc8 + 8 * S)] = { std::format("attachmentblueprint_slot{}", S), "" };
            }
            return F;
        }

    protected:
        const char* Profile() const override { return "bo7"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("BO7"); }
        VirtualGameSession::Handlers Handlers() override
        {
            // the package index's names, its bo7_names.wni among them: names found for the game's
            // hashes in its own strings and the index's (cod-hash-recovery; the hash packages)
            IwctxReaders::LoadNameIndexes();
            static std::once_flag Names;
            std::call_once(Names, [] {
                CoDAssets::AssetNameCache.LoadIndex(
                    (fs::path(FileSystems::GetApplicationPath()) / "package_index" / "bo7_names.wni").string());
            });
            return { SlotString, DiskImage };
        }

        void Fixups() override
        {
            RegisterStrings();
            Served(true);
            // (the index asks what the zone itself defines: the zones beside a map are left out)
            if (!Indexing && !Options.ZoneAlone) LoadBeside();
            Counter("lods_without_surfaces") += LinkLods();
        }
        // the zone's script strings, which name the anims' notetracks (by their hash)
        void RegisterStrings() const
        {
            std::string Error;
            if (const auto* Tables = ExportWriters::ZoneProfile(Profile(), Error))
                IwctxReaders::RegisterScriptStrings(IwctxReaders::ZoneStrings(Zone(), Tables->Profile.Dir));
        }
        // the transient zones served beside this one (ProcessReader's extra regions go with the session)
        void Served(bool On) override
        {
            if (On) Beside.Serve();
        }

        // a model by the name at 8 ("a::b" as a_b), anything else by its hash at 0
        std::string AssetName(DiskGame::Kind K, int64_t Pos) const override
        {
            // Pos may lie past this zone: an asset of a transient zone served beside it
            const uint64_t Address = Live().Addr((uint64_t)Pos);
            if (K == DiskGame::Kind::Model)
            {
                std::string N = CString(Get<uint64_t>(Address + ModelName));
                if (N.empty()) return GameBlackOps7::HashName("xmodel", Get<uint64_t>(Address));
                return Strings::Replace(N, "::", "_");
            }
            if (K == DiskGame::Kind::Image) return GameBlackOps7::HashName("ximage", Get<uint64_t>(Address));
            if (K == DiskGame::Kind::Anim) return GameBlackOps7::HashName("xanim", Get<uint64_t>(Address));
            if (K == DiskGame::Kind::Script) return GameBlackOps7::HashName("script", Get<uint64_t>(Address)) + ".gsc";
            if (K == DiskGame::Kind::Rawfile) return GameBlackOps7::HashName("rawfile", Get<uint64_t>(Address));
            if (K == DiskGame::Kind::Data) return GameBlackOps7::HashName("stringtable", Get<uint64_t>(Address));
            return GameBlackOps7::HashName("xasset", Get<uint64_t>(Address));
        }
        // the NUL-terminated text at an address here or beside, "" when it is in neither
        std::string CString(uint64_t Address) const
        {
            std::string S;
            for (const uint8_t* P; S.size() < 4096 && (P = Locate(Address + S.size(), 1)) != nullptr && *P != 0;) S.push_back((char)*P);
            return S;
        }
        // localize (0x10, 0x3868f60): the key's hash at 0, the text at 8 encrypted with the string
        // cipher ([id 0x80-0xBF][b1][n], strings.cipher.txt from DecryptString 0x8c8ebd0); a text
        // the table cannot decrypt is left empty
        void LocalizeEntry(int64_t Pos, std::string& Key, std::string& Text) const override
        {
            Key = GameBlackOps7::HashName("localize", At<uint64_t>(Image(), Pos));
            std::string Error;
            const auto* Tables = ExportWriters::ZoneProfile(Profile(), Error);
            const auto* Cipher = Tables != nullptr ? StringCipher::Load(Tables->Profile.Dir, Error) : nullptr;
            if (!StringCipher::Decrypt(Cipher, StringAt(PointerAt(Pos + 8)), Text)) Text.clear();
        }
        // every asset of a zone's list is inline here: references are by hash, never a header
        bool IsReference(DiskGame::Kind, const std::string&, int64_t) const override { return false; }
        void Defines(std::vector<DiskGame::AssetId>& Out) const override
        {
            ZonekitZone::Defines(Out);
            DefineImages(Out);
        }

    public:
        // ---- a map's transient zones (<zone>_*_tr.ff beside it), which the game loads after the
        // level: the world's vertices and most of its models live there. Each is walked with the bo7
        // tables, relocated where it is served (above this zone), and every reference the zones make
        // to each other resolved as the game's shared rewind table and asset pools would: by rewind
        // key (the low 60 bits of a loaded pointer) and by asset hash.
        IwBeside::Set Beside;         // the global zones (Before) and the transient zones, served beside
        std::string Install;          // --install: where the global zones are when not beside the level
        bool Indexing = false;        // opened for the install index (ExportOptions::Indexing)

        // the bytes at an address of this zone or of a transient zone served beside it, null when
        // N bytes are not all there
        const uint8_t* Locate(uint64_t Address, uint64_t N) const
        {
            if (Live().Contains(Address) && Address - Live().Origin() + N <= Image().size()) return Image().data() + (Address - Live().Origin());
            return Beside.Locate(Address, N);
        }
        // which zone an address is in (the map debug file's)
        std::string ZoneOf(uint64_t Address) const
        {
            if (Live().Contains(Address)) return Name();
            for (const auto& S : Beside.All())
                if (Address >= S->Base && Address - S->Base < S->Bytes.size()) return S->Name;
            return "-";
        }
        template <class T>
        T Get(uint64_t Address) const
        {
            T V{};
            if (const uint8_t* P = Locate(Address, sizeof(T))) std::memcpy(&V, P, sizeof(T));
            return V;
        }
        // writes a pointer field (an address) with an address
        void Put(uint64_t Field, uint64_t Value)
        {
            if (Live().Contains(Field))
            {
                Live().Poke(Field - Live().Origin(), Value);
                return;
            }
            Beside.Put(Field, Value);
        }

    private:
        VirtualGameSession& Live() const { return const_cast<BO7Zone*>(this)->ZonekitZone::Live(); }

        // the zones the game loads before a level (FastFile load order: code, global, then the
        // mode's ingame zones): their rewind keys and assets are what a level's references name
        // when they name nothing of the level's own (names, LOD surfaces, materials, images)
        std::vector<std::string> GlobalZones() const
        {
            const std::string N = ExportNaming::Lower(Name());
            std::vector<std::string> Out;
            if (N.compare(0, 3, "zm_") == 0)
                Out = { "code_pre_gfx", "code_post_gfx", "global", "global_stream_mp", "global_shared_mp", "ingame", "ingame_shared_mp", "ingame_zm" };
            else
                Out = { "code_pre_gfx", "code_post_gfx", "global", "global_base_mp", "global_stream_mp", "global_shared_mp", "ingame",
                        "ingame_base_mp", "ingame_shared_mp", "ingame_mp" };
            // each global zone's techsets (techsets_<zone>: the code zones have none)
            for (size_t K = 0, Count = Out.size(); K < Count; K++) Out.push_back("techsets_" + Out[K]);
            // and the level's own companions: its techsets, its sounds (ww_, then the language's)
            for (const char* Companion : { "techsets_", "ww_", "ens_" }) Out.push_back(Companion + N);
            return Out;
        }

        void LoadBeside()
        {
            if (Root("gfx_map") < 0) return;
            std::string Error;
            const auto* Loaded = ExportWriters::ZoneProfile("bo7", Error);
            if (Loaded == nullptr) return;
            const fs::path Dir = fs::path(FastFilePath()).parent_path();
            const std::string Prefix = ExportNaming::Lower(Name()) + "_";
            std::vector<fs::path> Transient;
            std::error_code Ec;
            for (const auto& E : fs::directory_iterator(Dir, Ec))
            {
                const std::string F = ExportNaming::Lower(E.path().filename().string());
                if (F.size() > Prefix.size() + 6 && F.compare(0, Prefix.size(), Prefix) == 0 && F.compare(F.size() - 6, 6, "_tr.ff") == 0)
                    Transient.push_back(E.path());
            }
            std::sort(Transient.begin(), Transient.end());
            // the global zones first (they load first: their keys and assets only fill what the level
            // and its transient zones do not define, as a later load overrides an asset of the same
            // name), then the transient zones (Games/IW/IwBesideZones.h)
            std::vector<std::string> Globals, Transients;
            for (const auto& G : GlobalZones())
            {
                // beside the level, else in the install (a level copied out alone)
                if (ExportNaming::Lower(G) == ExportNaming::Lower(Name())) continue;
                for (const fs::path& From : { Dir, fs::path(Install), Install.empty() ? fs::path() : fs::path(Install) / "cod25" })
                    if (!From.empty() && fs::exists(From / (G + ".ff"), Ec))
                    {
                        Globals.push_back((From / (G + ".ff")).string());
                        break;
                    }
            }
            for (const auto& P : Transient) Transients.push_back(P.string());
            std::map<std::string, size_t> Counts;
            const uint64_t Above = Live().Origin() + Image().size();
            Beside.Load(Globals, *Loaded, Above, Counts, true);
            Beside.Load(Transients, *Loaded, Above, Counts);
            // the asset references of the indexed types no zone loaded defines, by type and hash
            std::set<std::pair<std::string, uint64_t>> Wanted;
            if (!Beside.Empty())
            {
                std::map<std::string, size_t> First;
                Beside.Link(Walk(), Live().Origin(), Zone(), Rules(), [this](uint64_t Field, uint64_t Value) { Put(Field, Value); }, First,
                            [&](const std::string&, uint64_t, uint64_t Value, const std::string& Type) {
                                if (std::find(std::begin(IndexedParts), std::end(IndexedParts), Type) != std::end(IndexedParts))
                                    Wanted.emplace(Type, Value & Rules().AssetHashMask);
                            });
            }
            // the zones that define them (other maps' transient zones, other modes' items), found in
            // the install index's parts, loaded after the global zones (filed last, as Before)
            if (!Wanted.empty()) LoadIndexed(Wanted, *Loaded, Above, Counts);
            if (!Beside.Empty())
            {
                // (an asset reference's type when the chain says it; the map debug file lists them all)
                std::map<std::string, size_t> UnlinkedTypes;
                std::vector<std::string> UnlinkedList;   // zone, field, value, type or key
                Beside.Link(Walk(), Live().Origin(), Zone(), Rules(), [this](uint64_t Field, uint64_t Value) { Put(Field, Value); }, Counts,
                            [&](const std::string& Zone, uint64_t Field, uint64_t Value, const std::string& Type) {
                                if (Type != "asset" && Type != "key") UnlinkedTypes[Type]++;
                                if (Maps::DebugFile()) UnlinkedList.push_back(std::format("{} {:x} {:016x} {}", Zone.empty() ? Name() : Zone, Field, Value, Type));
                            });
                if (FILE* Dbg = Maps::DebugFile())
                {
                    std::fprintf(Dbg, "[zones] %s: %zu zones beside (%zu global), %zu links across zones, %zu unresolved (%zu assets; %zu of the global zones' own)\n", Name().c_str(),
                                 Beside.All().size(), (size_t)std::count_if(Beside.All().begin(), Beside.All().end(), [](const auto& S) { return S->Before; }),
                                 Counts["links_across_zones"], Counts["refs_unresolved_across_zones"], Counts["refs_unresolved_across_zones_assets"],
                                 Counts["refs_unresolved_in_zones_before"]);
                    for (const auto& [Type, N] : UnlinkedTypes) std::fprintf(Dbg, "[zones]   unresolved %s: %zu\n", Type.c_str(), N);
                    for (const auto& L : UnlinkedList) std::fprintf(Dbg, "[unresolved] %s\n", L.c_str());
                }
            }
            for (const auto& [K, N] : Counts) Counter(K) += N;
            if (Beside.Empty()) return;
            Served(true);
        }

        // The zones the install index says define the Wanted references (its parts), the zones
        // giving the most first, none already loaded: loaded beside as Before zones. The index is
        // read (or built: this zone's session parked meanwhile) only when something is wanted.
        void LoadIndexed(const std::set<std::pair<std::string, uint64_t>>& Wanted, const ExportWriters::LoadedProfile& Loaded, uint64_t Above,
                         std::map<std::string, size_t>& Counts)
        {
            Counts["refs_wanted_from_index"] += Wanted.size();
            if (Game == nullptr || Install.empty())
            {
                Counts["refs_wanted_no_index"] += Wanted.size();
                return;
            }
            std::string Error;
            Park();
            const auto Index = DiskGame::ZoneIndex::For(*Game, Options, Install, IndexKey(), nullptr, nullptr, Error);
            Resume();
            if (!Index)
            {
                Counts["refs_wanted_no_index"] += Wanted.size();
                return;
            }
            std::set<std::string> Have{ ExportNaming::Lower(Name()) };
            for (const auto& S : Beside.All()) Have.insert(ExportNaming::Lower(S->Name));
            std::map<size_t, std::set<std::pair<std::string, uint64_t>>> Gives;
            for (const auto& W : Wanted)
                for (const size_t Z : Index->PartIn(W.first, W.second))
                    if (!Have.count(ExportNaming::Lower(fs::path(Index->Zones()[Z]).stem().string()))) Gives[Z].insert(W);
            std::set<std::pair<std::string, uint64_t>> Left = Wanted;
            std::vector<std::string> Paths;
            while (!Left.empty() && !Gives.empty())
            {
                const auto Count = [&](const auto& Ws) { return std::count_if(Ws.begin(), Ws.end(), [&](const auto& W) { return Left.count(W) != 0; }); };
                const auto Best = std::max_element(Gives.begin(), Gives.end(), [&](const auto& A, const auto& B) { return Count(A.second) < Count(B.second); });
                size_t Takes = 0;
                for (const auto& W : Best->second) Takes += Left.erase(W);
                if (Takes == 0) break;
                std::vector<std::string> Found;
                std::string Why;
                if (Game->ResolveZones(Install, { Index->Zones()[Best->first] }, Options, Found, Why) && !Found.empty()) Paths.push_back(Found.front());
                Gives.erase(Best);
            }
            Counts["refs_not_indexed"] += Left.size();
            Counts["zones_beside_from_index"] += Beside.Load(Paths, Loaded, Above, Counts, true);
            if (FILE* Dbg = Maps::DebugFile())
                std::fprintf(Dbg, "[zones] %s: %zu references wanted from the index, %zu zones loaded for them, %zu not indexed\n", Name().c_str(),
                             Wanted.size(), Paths.size(), Left.size());
        }

    public:
        // the asset types a map's references are looked up by in the install index (IDiskZone::Parts)
        static constexpr const char* IndexedParts[] = { "xmodelsurfs", "xmodel", "material", "image" };
        // What other zones name and this one holds: every instance of the IndexedParts types by its
        // name hash
        void Parts(std::vector<std::pair<std::string, uint64_t>>& Out) const override
        {
            const auto& R = Walk();
            for (const auto& I : R.Instances)
                if (I.Owner < R.OwnerNames.size() &&
                    std::find(std::begin(IndexedParts), std::end(IndexedParts), R.OwnerNames[I.Owner]) != std::end(IndexedParts))
                    Out.emplace_back(R.OwnerNames[I.Owner], At<uint64_t>(Zone(), I.Pos) & Rules().AssetHashMask);
        }
        std::string IndexKey() const override { return ZonekitZone::IndexKey() + "+parts1"; }
        const DiskGame::IDiskGame* Game = nullptr;   // whose install index LoadIndexed reads
        DiskGame::ExportOptions Options;

    private:
        // the game's load-time copy lod.surfs = lod.modelSurfs->surfs (DB_ModelFixup), for every
        // LOD whose xmodelsurfs is here or in a zone served beside; the others are left without
        // surfaces (counted, the global zones' own models apart)
        size_t LinkLods()
        {
            size_t Unlinked = 0;
            const auto Link = [&](uint64_t Model, bool Count) {
                const uint64_t Lods = Get<uint64_t>(Model + ModelLods);
                if (Locate(Lods, LodSize) == nullptr) return;
                const int64_t N = Get<uint8_t>(Model + ModelLodCount);
                for (int64_t L = 0; L < N; L++)
                {
                    const uint64_t Lod = Lods + (uint64_t)(L * LodSize);
                    const uint64_t Surfs = Get<uint64_t>(Lod);
                    const uint64_t Data = Locate(Surfs, 0x48) ? Get<uint64_t>(Surfs + ModelSurfsSurfs) : 0;
                    if (Locate(Data, 1) == nullptr)
                    {
                        Unlinked += Count;
                        continue;
                    }
                    Put(Lod + LodSurfs, Data);
                }
            };
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "xmodel")) Link(Live().Addr((uint64_t)I.Pos), true);
            for (const auto& S : Beside.All())
                for (const auto& I : S->R.Instances)
                    if (I.Owner < S->R.OwnerNames.size() && S->R.OwnerNames[I.Owner] == "xmodel") Link(S->Base + (uint64_t)I.Pos, !S->Before);
            return Unlinked;
        }

        static std::unique_ptr<XImageDDS> DiskImage(const XImage_t& Image)
        {
            return ServedZone<BO7Zone>() == nullptr ? nullptr : GameBlackOps7::LoadXImage(Image);
        }

        void ExportImages(const DiskGame::Context& C, DiskGame::Totals& T)
        {
            ImageList L;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "image")) L.Order.push_back(I.Pos);
            // a map's transient zones' images too, by their address past this zone (Pos = address - origin);
            // the global zones' only for a selection (the map's textures): the rest are theirs to export
            for (const auto& S : Beside.All())
                for (const auto& I : S->R.Instances)
                    if ((!S->Before || C.Only != nullptr) && I.Owner < S->R.OwnerNames.size() && S->R.OwnerNames[I.Owner] == "image")
                        L.Order.push_back((int64_t)(S->Base + (uint64_t)I.Pos - Live().Origin()));
            L.NameAt = 0;
            // the header past its hash (format, sizes, the mips' package keys), the in-zone pixels, the extension
            L.KeyOf = [this](int64_t Pos, const std::string& Extension) {
                uint64_t H = 0xcbf29ce484222325ull;
                const auto Mix = [&H](const uint8_t* P, uint64_t N) {
                    for (uint64_t K = 0; P != nullptr && K < N; K++) H = (H ^ P[K]) * 0x100000001b3ull;
                };
                const uint64_t Address = Live().Addr((uint64_t)Pos);
                Mix(Locate(Address + 8, 0x38), 0x38);
                const uint64_t MipCount = Get<uint8_t>(Address + 0x2e) & 0xF;
                Mix(Locate(Get<uint64_t>(Address + 0x30), MipCount * 24), MipCount * 24);
                const uint32_t Size = Get<uint32_t>(Address + 0x18);
                Mix(Locate(Get<uint64_t>(Address + 0x38), Size), Size);
                Mix((const uint8_t*)Extension.data(), Extension.size());
                return H ? H : 1;
            };
            L.Decode = [this](int64_t Pos, const std::string& Name) {
                return GameBlackOps7::LoadXImage(XImage_t(ImageUsageType::Unknown, 0, Live().Addr((uint64_t)Pos), Name));
            };
            L.FailWhy = [this](int64_t Pos) {
                return std::string(Get<uint64_t>(Live().Addr((uint64_t)Pos) + 0x38) != 0 ? "in zone" : "streamed");
            };
            L.OnePerName = true;
            ExportImageList(C, T, L);
        }
    };

    const IwWeaponStats::Layout& BO7Zone::StatsLayout()
    {
        static const IwWeaponStats::Layout L = [] {
            IwWeaponStats::Layout L;
            // weapon: name 8, definition 0x20, display name 0x40; the attachment slots weapon +0x80 +
            // slot * 0x10 (0x46843b0 reads the list through 0xec8a200)
            L.WeaponName = 8, L.WeaponDef = 0x20, L.WeaponDisplay = 0x40, L.Slots = 0x80, L.SlotCount = 17;
            // definition: class u8 0x1afb (weaponclass 0x74e41b0 -> 0x46542a0, names 0xec874d0), base
            // animation package 0x350, the tunables sets 0x1b18 / 0x1b20 / 0x1b28 (0x460f300 picks one into
            // 0x1b30), the animation package overrides 0x60 / count 0x1838 (0x464aab0)
            L.DefClass = 0x1afb, L.DefAnimPkg = 0x350, L.DefSets = 0x1b18, L.DefSetCount = 0x1b20, L.DefSetTags = 0x1b28, L.DefTunables = 0x1b30,
            L.DefOverrides = 0x60, L.DefOverrideCount = 0x1838;
            // tunables: see Columns below
            L.FireTime = 0x5cd4, L.BurstCount = 0x5d1c, L.StartAmmo = 0x5d34, L.StartAmmoOverride = 0x5d38, L.ClipSize = 0x5d48, L.MaxAmmo = 0x5d44,
            L.MaxAmmoByClips = 0x5e13, L.FireType = 0x5dfc, L.AdsTime = 0x5dac, L.AdsOutTime = 0x5db0, L.ReloadType = 0x5e46,
            L.ExplosionRadius = 0x794, L.ExplosionRadiusAlt = 0x7bc, L.ExplosionAlt = 0x7f4, L.MuzzleVelocity = 0x30, L.Damage = 0x70, L.Hit = 0x530,
            L.SprintOutScale = 0x5a9c, L.SprintSpeedScale = 0x5c14, L.SuperSprintSpeedScale = 0x5c28, L.ProneSpeedScale = 0x5c08;
            // attachment: name 0x248, clip 0xb90 / 0x400, modifiers 0x250, ADS override 0x458, explosion 0x578
            L.AttachmentName = 0x248, L.AttClipScale = 0xb90, L.AttClipOverride = 0x400, L.AttModifiers = 0x250, L.AttAdsOverride = 0x458,
            L.AttExplosion = 0x578, L.AttExplosionAlt = 0x28;
            // the default parts: 0x4697c87 takes slot s's attachment from the weapon blueprint (+0x40 + 8 s)
            // and, with none, from the weapon (+0x3a0 + 8 s); 0x46356d0 gives an attachment's model with
            // no blueprint: +0x270 the view model, +0x268 the world model (xmodel name hashes)
            L.DefaultAttachments = 0x3a0, L.AttWorldModel = 0x268, L.AttViewModel = 0x270;
            L.ModelName = [](uint64_t Hash) { return PartModelName(Hash); };
            // animation package (0x46556b0 reads its block 0x18 at a byte offset)
            L.PkgBlock = 0x18, L.PkgFirstRaise = 0x2, L.PkgReloadEmpty = 0xac, L.PkgReload = 0xae, L.PkgSprintOut = 0x522, L.PkgSuperSprintOut = 0x524,
            L.PkgRaise = 0x30a, L.PkgDrop = 0x308, L.PkgRaiseQuick = 0x328, L.PkgReloadAdd = 0xb4, L.PkgReloadEmptyAdd = 0xb6, L.PkgReloadStart = 0xc8,
            L.PkgReloadEnd = 0xf8;
            // 0xffff is the package's "no time": the override search skips an entry holding it
            // (0x46557c5); a base package's 0xffff is left out (empty), not written as 65535
            L.PkgFfffIsNone = true;
            // suit (type 0x51, 0x4528a40): name hash 0x20, sprint 0xe18 (0x43cde50), super sprint 0xe78 (0x43a28f0)
            L.SuitId = 0x20, L.SuitSprint = 0xe18, L.SuitSuperSprint = 0xe78;
            // gametype settings keys: the tunables tag 0x4611a20 hands 0x460f300, the suit 0x4528930 maps
            // to the player's suit; an attachment's info bundle (type 76db25c2b8532150): its attachment
            // asset and display name (a localize hash)
            L.TagKey = 0x65811ecb02190e17ull, L.SuitKey = 0x9465580f7953cea8ull, L.AttachmentKey = 0x3be72d7e1d0b16e5ull,
            L.NameKey = 0xc4bcadba8e631b86ull;
            L.TagNames = { "default", "t10_br", "t10_zm", "t10_mp", "t10_ob", "t10_cp" };
            // 0xb3dfed8, the speed the weapon stats UI (0x4ced840) starts from
            L.BaseSpeed = 190.0f;
            L.MpSuit = "sat_defaultsuit_mp";
            L.NameHash = [](const std::string& Name) { return GameBlackOps7::HashIWAsset(Name); };
            L.Title = "Black Ops 7 weapon stats";
            L.Columns =
                "\nA value its zone was not read for stays empty: the guns are in global_stream_mp and\n"
                "global_stream_cfwd_mp, their animation packages (reload, raise, sprint-to-fire) in ingame_shared_mp, the\n"
                "suits and game-mode settings in global_shared_mp, the display names in ens_global. Export those zones\n"
                "into the same --out (a .txt list of zone paths works) and the table is rebuilt from every facts file.\n\n"
                "Columns and units\n"
                "- set: the tunables set (weapon_def 0x1b18[set-1]). tags: the set's mode tags (FNV-1a 64 names). The\n"
                "  game picks the set whose tags hold the gametype settings' tag (0x460f300); the set tagged 'default'\n"
                "  is the fallback. t10_br is the tag of the Warzone settings (suit sat_newsuit_wz), t10_zm of Zombies.\n"
                "- fire_time_ms: tunables 0x5cd4 (weaponfiretime). rpm = 60000 / fire_time_ms.\n"
                "- burst_count: 0x5d1c (weaponburstcount). fire_type: u8 0x5dfc (an attachment's +0x318 overrides it):\n"
                "  0 auto (weaponisauto), 2 burst (weaponburstcount returns the count only for 2), 5 beam (weaponisbeam),\n"
                "  1, 3, 4 semi-auto (weaponissemiauto). 3 fires two rounds a trigger pull when tunables 0x5d18 <= 0\n"
                "  (0x45f84ec); 4 is grouped with 3 in 0x45da0ba and 0x45f9e93, not told apart further. The builtins also\n"
                "  check the weapon's ammo kind, not reproduced.\n"
                "- clip_size: 0x5d48. start_ammo: 0x5d38 when not 0, else 0x5d34. max_ammo: 0x5d44 (weaponmaxammo with\n"
                "  no attachment, no perk); empty when u8 0x5e13 is set (then it counts magazines, not reproduced).\n"
                "- reload_ms, reload_empty_ms, first_raise_ms: the base animation package (weapon_def 0x350), u16 0xae,\n"
                "  0xac and 0x2 of its time block (getweaponreloadtime, weaponfirstraisetime), before attachments and perks.\n"
                "  A package time of 0xffff is none (the override search skips it, 0x46557c5): its cell, and the\n"
                "  sprint-to-fire times computed from it, stay empty (the base getter returns it unchanged).\n"
                "- reload_add_ms (u16 0xb4, 0x4671060) and reload_empty_add_ms (u16 0xb6, 0x46715c0): the reload state\n"
                "  (0x45a66d0) takes the reload time (weapon anim 0x14, ammo left) or the empty one (0x15), and the matching\n"
                "  add time, and stores min(add, reload) as the reload's second timer (ps+0x5a4, 0x4676db0): when the rounds\n"
                "  enter the magazine (the consumer of that timer is not traced).\n"
                "- reload_type: u8 0x5e46 (0x464ba30; an attachment with u8 0xd91 turns 1 into 0): 0 magazine, 1 per round.\n"
                "  For 1, getweaponreloadtime (0x735cdf0) returns reload_start_ms (u16 0xc8, 0x4675260) + reload_ms (one\n"
                "  round) + reload_end_ms (u16 0xf8, 0x46738e0; 0x104 under a weapon-state bit); empty start for the per-round\n"
                "  reload: 0xac + u16 0xc4 or 0xc6 (not in the table).\n"
                "- raise_ms (u16 0x30a), raise_quick_ms (u16 0x328, 0x33a under player flag 0x1c10): the raise state's time\n"
                "  for weapon anims 0x11 and 0x3c (0x4593d40; first raise is 0x12). drop_ms (u16 0x308): the time of weapon\n"
                "  anim 0x10 (0x4594250), which the weapon stats UI (0x4cec52c) adds to the raise as the swap time. Each is\n"
                "  scaled by the player's perks in game (0x4649d40 raise, 0x4649ab0 drop), not here.\n"
                "- sprint_to_fire_ms = max(1, (int)(u16 0x522 x tunables 0x5a9c)), super_sprint_to_fire_ms the same\n"
                "  with u16 0x524 (0x45891e0, 0x461f8d0: a single-precision product truncated like the game: 250 x 0.76f is 190.0f).\n"
                "- ads_time_ms: 0x5dac (aiming in). ads_out_time_ms: 0x5db0 (aiming out). 0x462ee30 reads 4 times,\n"
                "  0x462ea90 turns the first two into rates, and 0x45876f0 uses the first while the aim is wanted (the ADS\n"
                "  fraction ps+0x560, playerads, rising), the second when released.\n"
                "- muzzle_velocity_mps: 0x30 (metres per second).\n"
                "- explosion_radius_m: getweaponexplosionradius (0x462c250): i32 0x794, or 0x7bc when u8 0x7f4 is set\n"
                "  (0x462c460); an attachment's block at 0x578 overrides it. Inches / 39.37 (the engine's distance unit,\n"
                "  as for the ranges). Empty when 0. Most weapons, bullet guns and fists included, hold 300 (7.62 m):\n"
                "  what the builtin returns, whether or not the weapon ever explodes (not decoded yet).\n"
                "- damage_*: the first record of the table at 0x70 (0x46386d0): damage_max up to range_max_m, then the\n"
                "  first mid whose range is set and beyond the distance, else damage_min (0x464c9e0). Ranges are\n"
                "  inches / 39.37. Other tables (modes, profiles) are in the raw dump only.\n"
                "- hit_*: the multipliers of the table at 0x530 (0x4642180), by the game's hit location order.\n"
                "- sprint_speed_scale (0x5c14), super_sprint_speed_scale (0x5c28), prone_speed_scale (0x5c08): scales.\n"
                "- sprint_mps = 190 x suit 0xe18 x sprint_speed_scale / 39.37, super_sprint_mps also x suit 0xe78 and\n"
                "  with super_sprint_speed_scale: the weapon stats UI's formula (0x4ced840). suit: the suit of the\n"
                "  gametype settings whose tag the set holds (empty when no settings name the set's tags, as for\n"
                "  'default'). *_mp_suit: the same with sat_defaultsuit_mp, the suit of the multiplayer settings (they\n"
                "  hold no tag, so the 'default' set is used unless a fallback settings bundle gives one).\n";
            L.AttachmentNotes =
                "\nweapon_attachments.csv: each weapon's default set with one attachment (every slot's list of\n"
                "weapon +0x80 + slot * 0x10, slot and index as 0x46843b0 reads them). weapon_loadouts.csv: the\n"
                "lines of loadouts.txt in this folder ('<weapon asset>: <attachment>, <attachment>...', an attachment by\n"
                "asset or display name), each loadout of the default set. How the attachments combine (the code):\n"
                "- clip_size: the i32 at 8 of an attachment's block 0x400 when >= 0 (0x4637e2c), else\n"
                "  (int)(clip x (1 + sum(attachment 0xb90 - 1))) (0x4637d88).\n"
                "- reload_ms, reload_empty_ms, the sprint-out times: the animation package of the override entry\n"
                "  (weapon_def 0x60) with the most condition groups whose groups all match (a group: any of its\n"
                "  conditions {slot, mask of slot indexes}; a group tied to a blueprint variant never matches here) and\n"
                "  whose time is not 0xffff, else the base package (0x46556b0, 0x464aab0, 0x46843b0). Reload modifiers\n"
                "  of perks and of 0x4757c60 are not applied.\n"
                "- sprint_to_fire_ms = max(1, (int)(time x 0x5a9c x (1 + sum(attachment 0x250->0x08 +0x18 - 1)) +\n"
                "  sum((int)(1000 x attachment 0x250->0x68 +4)))) (0x461f8d0); the same with the super sprint time.\n"
                "- ads_time_ms = max(1, ads_time / (1 + sum(0x250->0x18 +4 - 1)) + (int)(1000 x sum(0x250->0x90 +4)))\n"
                "  (0x462ee30, 0x46318c0); ads_time is replaced by (int)(1000 x t) of the first attachment with ADS\n"
                "  times at 0x458 (0x4652650; an optic's own at 0x550 and the 0x478 variant are not read).\n"
                "  ads_out_time_ms: the same with ads_out_time, 0x90 +8 and the override's second time.\n"
                "- reload_add_ms, reload_empty_add_ms: from the same animation package as the reload times.\n"
                "- Zombies Pack-a-Punch is an attachment of slot 0 (sat_zm_<gun>_pack, _pack4; sat_ob_<gun>_tier1..4 for\n"
                "  the other mode): its row in weapon_attachments.csv is the upgraded weapon (names only: no code link).\n"
                "- muzzle_velocity_mps = velocity x product(attachment 0x250->0xd0 +4) (0x46362d4).\n";
            L.DefaultPartsNotes =
                "\nweapon_default_parts.csv: the parts a weapon is built with when no blueprint (variant) is chosen: one\n"
                "row per slot holding a default attachment, with that attachment's own models (the models to assemble).\n"
                "- slot, attachment: 0x4697c87 takes slot s's attachment from the weapon blueprint (+0x40 + 8 s) and,\n"
                "  with none, from the weapon itself (+0x3a0 + 8 s, s from 1): the default attachment. slot is s (the\n"
                "  slot of weapon_attachments.csv; the default is one of that slot's list).\n"
                "- world_model, view_model: 0x46356d0 (attachment, blueprint index, view) returns, for blueprint index 0\n"
                "  (none), the attachment's +0x270 (view) or +0x268 (world), xmodel name hashes; an index k > 0 takes the\n"
                "  k-th attachmentblueprint of the attachment's list (+0x30, count +0x38, used when byte +0x3c is set),\n"
                "  its +0x70 (view) or +0x78 (world). A blueprint is a cosmetic variant, never the default part. Empty: no\n"
                "  model (sights built in, fire modes, ammo). A model is named as it is exported: the name the install\n"
                "  index lists for its hash (most parts live in the transient zones mp_av_* (view) and mp_aw_* (world)),\n"
                "  else the hash packages', else the other model's name with _vm_ / _wm_ swapped when that hashes to it,\n"
                "  else xmodel_<hash> (no index built yet: GrayWolf disk-index <install> --game bo7). Not applied:\n"
                "  the +0x278 model 0x46356d0 returns under a condition of its own (0xa007ba0 and not 0xa010d50) when set.\n"
                "- attachment_id: the attachment's name hash; its other cells stay empty when no zone read holds it.\n";
            return L;
        }();
        return L;
    }

    // ---- the level (C2M's BlackOps7.cs and BlackOps6 structs, offsets from the gfx_map loader's
    // tables, 0x385b900 and its readers). gfx_map (0x10): the GfxWorld (0xb370) at 8, its name at 8.
    // GfxWorldSurfaces inline at 0xd8: surface count u32 0, material count u32 0xc, brush model
    // count u32 0x14; brush models 0x58 (60 bytes: first surface u32 48, end u32 52), surfaces 0x78
    // (40 bytes), materials 0xa0 (asset pointers), per-surface draw data 0xc0 (128 bytes).
    constexpr int64_t WorldName = 8, Surf = 0xd8;
    constexpr int64_t SurfCount = 0, SurfMaterialCount = 0xc, SurfBrushCount = 0x14, SurfBrushes = 0x58, SurfSurfaces = 0x78,
                      SurfMaterials = 0xa0, SurfData = 0xc0;
    constexpr int64_t BrushSize = 60, BrushStart = 48, BrushEnd = 52;
    // A surface (40): first index u32 4, packed index table u32 8 (in 32-byte tables), its draw data
    // u32 12, material u32 16, face count u16 20, packed table count u16 22, vertex count u16 24,
    // packed indices u32 36
    constexpr int64_t SurfaceSize = 40, SfFirstIndex = 4, SfTable = 8, SfData = 12, SfMaterial = 16, SfFaces = 20, SfTables = 22,
                      SfVertices = 24, SfPacked = 36;
    // Its draw data (128): position offset x y z and scale 0, its transient zone u32 16, uv layers
    // u32 28, byte offsets into the transient zone's position data: positions (u64: 21 bits per
    // axis) 40, tangent frames (u32) 44, colours 52 (0: none), uvs (2 floats per layer) 56
    constexpr int64_t DataSize = 128, DtZone = 16, DtLayers = 28, DtXyz = 40, DtTangent = 44, DtColor = 52, DtUv = 56;
    // gfx_map_trzone (0x190): its index i32 0x14; its draw data inline at 0x18: position data size
    // u32 0x18, index count u32 0x1c, packed table count u32 0x20, packed indices size u32 0x28,
    // the position data 0x30, the u16 indices 0x38, the 32-byte packed tables 0x40, the packed
    // indices 0x48
    constexpr int64_t TzIndex = 0x14, TzPosSize = 0x18, TzIndexCount = 0x1c, TzTableCount = 0x20, TzPackedSize = 0x28, TzPos = 0x30,
                      TzIndices = 0x38, TzTables = 0x40, TzPacked = 0x48;
    // GfxWorldStaticModels inline at 0x258: model count u32 4, instance count u32 8 (24-byte
    // records at 0xa0: first placement u32 0, count u16 4, model u16 8), placement count u32 0xc
    // (24 bytes at 0x128: origin 3 x i32 / 4096, rotation 4 x u16 over [-1, 1] x y z w, scale half
    // float 20), models 0x90 (16 bytes, the XModel at 0)
    constexpr int64_t SModels = 0x258, SmModelCount = 4, SmInstCount = 8, SmPlaceCount = 0xc, SmModels = 0x90, SmInsts = 0xa0,
                      SmPlacements = 0x128;

    bool BO7Zone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        const int64_t Asset = Root("gfx_map");
        if (Asset < 0) return false;
        const int64_t World = Off(PointerAt(Asset + 8));
        if (World < 0) return false;
        const auto& Img = Image();
        FILE* Dbg = Maps::DebugFile();
        Scene.Name = StringAt(PointerAt(World + WorldName));
        if (Scene.Name.empty()) Scene.Name = Name();

        // ---- the transient zones' draw data, by their index: this zone's and its _tr zones'
        struct DrawData
        {
            const uint8_t* Pos = nullptr;
            uint64_t PosSize = 0;
            const uint8_t* Buf = nullptr;   // the zone holding the index data
            size_t BufSize = 0;
            uint64_t Indices = 0, Tables = 0, Packed = 0;   // offsets into Buf
            bool Ok = false;
        };
        std::unordered_map<int32_t, DrawData> Zones;
        const auto AddZone = [&](uint64_t Tz, const uint8_t* Buf, size_t BufSize, uint64_t Base) {
            DrawData D;
            D.PosSize = Get<uint32_t>(Tz + TzPosSize);
            const uint64_t PosAt = Get<uint64_t>(Tz + TzPos), IdxAt = Get<uint64_t>(Tz + TzIndices);
            const uint64_t TabAt = Get<uint64_t>(Tz + TzTables), PackAt = Get<uint64_t>(Tz + TzPacked);
            D.Pos = Locate(PosAt, D.PosSize);
            D.Buf = Buf, D.BufSize = BufSize;
            D.Ok = D.Pos != nullptr && IdxAt >= Base && TabAt >= Base && PackAt >= Base && IdxAt - Base < BufSize && TabAt - Base < BufSize &&
                   PackAt - Base < BufSize;
            if (D.Ok) D.Indices = IdxAt - Base, D.Tables = TabAt - Base, D.Packed = PackAt - Base;
            const int32_t Index = Get<int32_t>(Tz + TzIndex);
            const auto Have = Zones.find(Index);
            if (Have == Zones.end() || (!Have->second.Ok && D.Ok)) Zones[Index] = D;
        };
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, "gfx_map_trzone")) AddZone(Live().Addr((uint64_t)I.Pos), Img.data(), Img.size(), Live().Origin());
        for (const auto& S : Beside.All())
            for (const auto& I : S->R.Instances)
                if (I.Owner < S->R.OwnerNames.size() && S->R.OwnerNames[I.Owner] == "gfx_map_trzone")
                    AddZone(S->Base + (uint64_t)I.Pos, S->Bytes.data(), S->Bytes.size(), S->Base);

        const int64_t W = World + Surf;
        const uint32_t SurfaceCount = At<uint32_t>(Img, W + SurfCount), BrushCount = At<uint32_t>(Img, W + SurfBrushCount);
        const uint32_t MaterialCount = At<uint32_t>(Img, W + SurfMaterialCount);
        const int64_t Surfaces = Off(PointerAt(W + SurfSurfaces)), Brushes = Off(PointerAt(W + SurfBrushes));
        const int64_t Data = Off(PointerAt(W + SurfData)), MaterialsAt = Off(PointerAt(W + SurfMaterials));
        if (Dbg)
            std::fprintf(Dbg, "[map] %s world %llx surfaces %u at %llx brushes %u at %llx data %llx materials %u at %llx trzones %zu (%zu zones beside)\n",
                         Scene.Name.c_str(), (long long)World, SurfaceCount, (long long)Surfaces, BrushCount, (long long)Brushes, (long long)Data,
                         MaterialCount, (long long)MaterialsAt, Zones.size(), Beside.All().size());
        if (Surfaces < 0 || Data < 0) return false;

        // a world vertex: its surface's first vertex key + its index in the surface
        std::vector<uint32_t> FirstKey(SurfaceCount + 1, 0);
        for (uint32_t S = 0; S < SurfaceCount; S++)
            FirstKey[S + 1] = FirstKey[S] + At<uint16_t>(Img, Surfaces + (int64_t)S * SurfaceSize + SfVertices);
        const Maps::WorldBuilder::Fetch Fetch = [&](uint32_t Key, Maps::Vertex& V) {
            const auto It = std::upper_bound(FirstKey.begin(), FirstKey.end(), Key);
            if (It == FirstKey.begin() || It == FirstKey.end()) return false;
            const uint32_t S = (uint32_t)(It - FirstKey.begin() - 1), I = Key - FirstKey[S];
            const int64_t Sf = Surfaces + (int64_t)S * SurfaceSize;
            const int64_t D = Data + (int64_t)At<uint32_t>(Img, Sf + SfData) * DataSize;
            const auto Z = Zones.find((int32_t)At<uint32_t>(Img, D + DtZone));
            if (Z == Zones.end() || !Z->second.Ok) return false;
            const uint8_t* Pos = Z->second.Pos;
            const uint64_t PosSize = Z->second.PosSize;
            const auto Read = [&](uint64_t At, void* Out, size_t N) {
                if (At + N > PosSize) return false;
                std::memcpy(Out, Pos + At, N);
                return true;
            };
            uint64_t P = 0;
            if (!Read(At<uint32_t>(Img, D + DtXyz) + (uint64_t)I * 8, &P, 8)) return false;
            const float Scale = At<float>(Img, D + 12);
            for (int K = 0; K < 3; K++) V.Pos[K] = (float)((P >> (21 * K)) & 0x1FFFFF) * Scale + At<float>(Img, D + K * 4);
            uint32_t Tan = 0;
            if (Read(At<uint32_t>(Img, D + DtTangent) + (uint64_t)I * 4, &Tan, 4))
            {
                float N[3];
                IwctxReaders::UnpackNormalQuat(Tan, N);
                V.Normal[0] = N[0], V.Normal[1] = N[1], V.Normal[2] = N[2];
            }
            const uint32_t Layers = std::max(At<uint32_t>(Img, D + DtLayers), 1u);
            Read(At<uint32_t>(Img, D + DtUv) + (uint64_t)I * 8 * Layers, V.Uv, 8);
            const uint32_t ColorAt = At<uint32_t>(Img, D + DtColor);
            if (ColorAt != 0) Read(ColorAt + (uint64_t)I * 4, V.Color, 4);
            return true;
        };
        std::unordered_map<uint32_t, std::string> Materials;
        const auto MaterialOf = [&](uint32_t Index) -> const std::string& {
            auto Found = Materials.find(Index);
            if (Found == Materials.end())
            {
                const uint64_t Address = MaterialsAt >= 0 && Index < MaterialCount ? PointerAt(MaterialsAt + (int64_t)Index * 8) : 0;
                if (Dbg)
                    std::fprintf(Dbg, "[map material] %u at %llx (%s) hash %llx images %llx (%s) indices %llx\n", Index, (unsigned long long)Address,
                                 ZoneOf(Address).c_str(), (unsigned long long)Get<uint64_t>(Address), (unsigned long long)Get<uint64_t>(Address + 0x30),
                                 ZoneOf(Get<uint64_t>(Address + 0x30)).c_str(), (unsigned long long)Get<uint64_t>(Address + 0x28));
                const XMaterial_t Mat = GameBlackOps7::ReadXMaterial(Address);
                if (Dbg)
                    for (const auto& I : Mat.Images)
                        std::fprintf(Dbg, "[map material]   %s semantic %x %s at %llx (%s)\n", Mat.MaterialName.c_str(), I.SemanticHash, I.ImageName.c_str(),
                                     (unsigned long long)I.ImagePtr, ZoneOf(I.ImagePtr).c_str());
                Found = Materials.emplace(Index, Maps::AddMaterial(Scene, Mat)).first;
            }
            return Found->second;
        };
        const auto Range = [&](uint32_t B) {
            const int64_t Brush = Brushes + (int64_t)B * BrushSize;
            return std::make_pair(At<uint32_t>(Img, Brush + BrushStart), At<uint32_t>(Img, Brush + BrushEnd));
        };
        Maps::BuildWorld(Scene, Brushes >= 0 ? BrushCount : 0, SurfaceCount, Range, Fetch, [&](uint32_t S, Maps::WorldBuilder& Mesh) {
            const int64_t Sf = Surfaces + (int64_t)S * SurfaceSize;
            const int64_t D = Data + (int64_t)At<uint32_t>(Img, Sf + SfData) * DataSize;
            const auto Z = Zones.find((int32_t)At<uint32_t>(Img, D + DtZone));
            if (Z == Zones.end() || !Z->second.Ok)
            {
                Counter(Z == Zones.end() ? "map_surfaces_in_zones_not_read" : "map_surfaces_unread") += 1;
                return;
            }
            const DrawData& Dz = Z->second;
            const std::string& Material = MaterialOf(At<uint32_t>(Img, Sf + SfMaterial));
            const uint16_t Faces = At<uint16_t>(Img, Sf + SfFaces), Vertices = At<uint16_t>(Img, Sf + SfVertices);
            const uint32_t TableCount = At<uint16_t>(Img, Sf + SfTables);
            const uint64_t TableAt = Dz.Tables + (uint64_t)At<uint32_t>(Img, Sf + SfTable) * 32;
            const uint64_t PackedAt = Dz.Packed + At<uint32_t>(Img, Sf + SfPacked);
            const uint64_t IndexAt = Dz.Indices + (uint64_t)At<uint32_t>(Img, Sf + SfFirstIndex) * 2;
            for (uint32_t F = 0; F < Faces; F++)
            {
                uint32_t X[3]{};
                if (!MeshDecode::UnpackDeltaFace(Dz.Buf, Dz.BufSize, TableAt, TableCount, PackedAt, IndexAt, F, X, 32, MeshDecode::Edge::Partial) || X[0] >= Vertices ||
                    X[1] >= Vertices || X[2] >= Vertices)
                {
                    Counter("map_triangles_unread") += 1;
                    continue;
                }
                if (!Mesh.Triangle(Material, FirstKey[S] + X[0], FirstKey[S] + X[1], FirstKey[S] + X[2])) Counter("map_triangles_unread") += 1;
            }
        });

        // ---- the static models: an instance record names a model and a run of placements. A
        // model in this zone is read by ExportMap; one a transient zone defines is read here.
        const int64_t Sm = World + SModels;
        const uint32_t ModelCount = At<uint32_t>(Img, Sm + SmModelCount), InstCount = At<uint32_t>(Img, Sm + SmInstCount);
        const uint32_t PlaceCount = At<uint32_t>(Img, Sm + SmPlaceCount);
        const int64_t SModelsAt = Off(PointerAt(Sm + SmModels)), Insts = Off(PointerAt(Sm + SmInsts)), Places = Off(PointerAt(Sm + SmPlacements));
        std::unordered_map<uint64_t, std::string> Beside;   // a transient zone's model -> its name ("" unread)
        const auto NameOf = [&](uint64_t Address) -> std::string {
            if (Live().Contains(Address))
            {
                if (Dbg)
                    std::fprintf(Dbg, "[map model] %llx name ptr %llx '%s' -> %s\n", (unsigned long long)Address, (unsigned long long)Get<uint64_t>(Address + ModelName),
                                 CString(Get<uint64_t>(Address + ModelName)).c_str(), ModelAt(Address).c_str());
                return ModelAt(Address);
            }
            if (Locate(Address, 0xd8) == nullptr) return std::string();
            auto Found = Beside.find(Address);
            if (Found != Beside.end()) return Found->second;
            std::string N = CoDAssets::GameInstance->ReadNullTerminatedString(Get<uint64_t>(Address + ModelName));
            N = N.empty() ? GameBlackOps7::HashName("xmodel", Get<uint64_t>(Address)) : Strings::Replace(N, "::", "_");
            CoDModel_t Model;
            Model.AssetName = ExportNaming::SafeFileName(N);
            Model.AssetPointer = Address;
            ExportWriters::LiveModel Read;
            std::string Crash;
            const bool Added = ExportWriters::ReadModel(Model, Read, Crash) == 0 && Maps::AddModel(Scene, N, Read);
            Counter(Added ? "map_models_beside" : "map_models_beside_unread") += 1;
            Beside.emplace(Address, Added ? N : std::string());
            return Added ? N : std::string();
        };
        size_t Placed = 0;
        for (uint32_t K = 0; SModelsAt >= 0 && Insts >= 0 && Places >= 0 && K < InstCount; K++)
        {
            const int64_t Rec = Insts + (int64_t)K * 24;
            const uint32_t FirstPlace = At<uint32_t>(Img, Rec), Count = At<uint16_t>(Img, Rec + 4);
            const uint16_t Model = At<uint16_t>(Img, Rec + 8);
            if (Model >= ModelCount) continue;
            const uint64_t Address = PointerAt(SModelsAt + (int64_t)Model * 16);
            const std::string ModelName = NameOf(Address);
            if (ModelName.empty())
            {
                if (Dbg)
                    std::fprintf(Dbg, "[map no model] instance %u model %u address %llx located %d\n", K, Model, (unsigned long long)Address,
                                 Locate(Address, 0xd8) != nullptr);
                Counter("map_placements_without_model") += Count;
                continue;
            }
            if (Live().Contains(Address)) Models.emplace(ModelName, Address);
            for (uint32_t J = FirstPlace; J < FirstPlace + Count && J < PlaceCount; J++)
            {
                const int64_t P = Places + (int64_t)J * 24;
                Maps::Instance In;
                In.Name = "smodel_" + std::to_string(J);
                In.Model = ModelName;
                for (int A = 0; A < 3; A++) In.Origin[A] = (float)At<int32_t>(Img, P + A * 4) / 4096.0f;
                float Q[4];
                for (int A = 0; A < 4; A++) Q[A] = (float)At<uint16_t>(Img, P + 12 + A * 2) / 65535.0f * 2.0f - 1.0f;
                Maps::AxisFromQuat(Q, In.Axis);
                In.Scale = HalfFloats::ToFloat(At<uint16_t>(Img, P + 20));
                Scene.StaticInstances.push_back(In);
                Placed++;
            }
        }
        if (Dbg)
            std::fprintf(Dbg, "[map] static models %u instances %u placements %u placed %zu\n", ModelCount, InstCount, PlaceCount, Placed);

        // ---- the terrain: vhmdata's heightmap, drawn where stterrain says (Games/IW/IwTerrain.h)
        if (const int64_t Vhm = Root("vhmdata"); Vhm >= 0)
        {
            const int64_t St = Root("stterrain");
            IwTerrain::Source Src;
            Src.Locate = [this](uint64_t A, uint64_t N) { return Locate(A, N); };
            Src.ImageName = [this](uint64_t Field) {
                if (Field == 0) return std::string();
                return GameBlackOps7::HashName("ximage", Locate(Field, 8) != nullptr ? Get<uint64_t>(Field) : Field);
            };
            if (const int64_t Vtm = Root("vtmdata"); Vtm >= 0) Src.Vtm = Live().Addr((uint64_t)Vtm);
            Src.LayerMaterial = [this, &Scene](uint64_t Field) {
                return Locate(Field, 0x78) != nullptr ? Maps::AddMaterial(Scene, GameBlackOps7::ReadXMaterial(Field)) : std::string();
            };
            std::map<std::string, size_t> Read;
            IwTerrain::Read(Scene, Src, Live().Addr((uint64_t)Vhm), St >= 0 ? Live().Addr((uint64_t)St) : 0, IwTerrain::BlackOps7, IwTerrain::Options(), Read);
            for (const auto& [K, N] : Read) Counter("map_" + K) += N;
        }

        // ---- the scriptables: map_ents (0x948) holds their instances at 0x318 (count u32 0x318, 0x60
        // records at 0x388: definition 0, origin 8, angles 20 (pitch, yaw, roll)); a definition
        // (scriptable, 0xd8) lists its models (count u32 0xb4, xmodel pointers 0xb8), each placed
        if (const int64_t Ents = Root("map_ents"); Ents >= 0)
        {
            const uint32_t Count = At<uint32_t>(Img, Ents + 0x318);
            const uint64_t Records = PointerAt(Ents + 0x388);
            size_t Dynamic = 0;
            for (uint32_t K = 0; K < Count && Locate(Records + (uint64_t)K * 0x60, 0x60) != nullptr; K++)
            {
                const uint64_t Rec = Records + (uint64_t)K * 0x60, Def = Get<uint64_t>(Rec);
                if (Locate(Def, 0xd8) == nullptr)
                {
                    Counter("map_scriptables_without_definition")++;
                    continue;
                }
                float Angles[3];
                Maps::Instance In;
                for (int A = 0; A < 3; A++) In.Origin[A] = Get<float>(Rec + 8 + A * 4), Angles[A] = Get<float>(Rec + 20 + A * 4);
                Maps::AnglesToAxis(Angles, In.Axis);
                const uint32_t ModelsIn = Get<uint32_t>(Def + 0xb4);
                const uint64_t List = Get<uint64_t>(Def + 0xb8);
                for (uint32_t M = 0; M < ModelsIn && M < 64 && Locate(List + M * 8, 8) != nullptr; M++)
                {
                    const uint64_t Address = Get<uint64_t>(List + M * 8);
                    const std::string Name = NameOf(Address);
                    if (Name.empty())
                    {
                        Counter("map_scriptable_models_unread")++;
                        continue;
                    }
                    if (Live().Contains(Address)) Models.emplace(Name, Address);
                    In.Name = "scriptable_" + std::to_string(K) + (M ? "_" + std::to_string(M) : std::string());
                    In.Model = Name;
                    Scene.DynamicInstances.push_back(In);
                    Dynamic++;
                }
            }
            Counter("map_scriptables") += Count;
            Counter("map_scriptable_instances") += Dynamic;
        }

        // ---- the entities (EntityStrings)
        std::vector<IwctxReaders::MapEntity> Ents;
        EntityStrings(Ents);
        Scene.MapEnts += IwctxReaders::EntityText(Ents);
        Counter("map_ents") += Ents.size();
        return true;
    }

    void BO7Zone::EntityStrings(std::vector<IwctxReaders::MapEntity>& Out) const
    {
        // map_game_objects' pairs (its 0x60 struct at 0x10), then each script brush model's (at
        // 0x10); keys by IwEntityKeys' SaltFirst (the parser 0x78bb741)
        // a pair: type bits 28-31 (0 a string, 4 a vector, 10 starts an entity: its ordinal the
        // value), key bits 21-27, value bits 0-20 (mp_sat_sm_liminal's string pool is past 0xffff)
        constexpr IwctxReaders::PairPacking EntityPairs{ 21, 21, 10, 4 };
        const auto& Img = Image();
        const auto OffOf = [this](uint64_t A) { return Off(A); };
        const auto PtrAt = [this](int64_t P) { return PointerAt(P); };
        if (const int64_t Go = Root("map_game_objects"); Go >= 0)
            if (const int64_t P = Off(PointerAt(Go + 0x10)); P >= 0)
                IwctxReaders::EntityPairs(Img, P, OffOf, PtrAt, Out, IwEntityKeys::Scheme::SaltFirst, EntityPairs);
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, "scriptbrushmodel"))
                IwctxReaders::EntityPairs(Img, I.Pos + 0x10, OffOf, PtrAt, Out, IwEntityKeys::Scheme::SaltFirst, EntityPairs);
    }

    void BO7Zone::MapEntities(std::vector<IwctxReaders::MapEntity>& Out) const
    {
        const int64_t Ents = Root("map_ents");
        if (Ents < 0) return;
        const auto& Img = Image();
        // a script string the records keep by the low 32 bits of its hash: its text when a zone
        // registered it, else "#<hex>"
        const auto Str = [](uint32_t H) {
            const std::string S = IwctxReaders::ScriptString32(H);
            return !S.empty() ? S : std::format("#{:08x}", H);
        };
        const auto Vec = [&](int64_t P) { return std::format("{} {} {}", At<float>(Img, P), At<float>(Img, P + 4), At<float>(Img, P + 8)); };
        // the spawn points: count u32 0x38, 0x48-byte records at 0x40 {classname u32 4, origin 0x10,
        // angles 0x1c, its pairs: count u32 0x34, 16-byte records at 0x40 {key (IwEntityKeys) 0, u32
        // value 8}}
        const uint32_t Spawns = At<uint32_t>(Img, Ents + 0x38);
        const int64_t SpawnsAt = Off(PointerAt(Ents + 0x40));
        for (uint32_t K = 0; SpawnsAt >= 0 && K < Spawns && SpawnsAt + (int64_t)(K + 1) * 0x48 <= (int64_t)Img.size(); K++)
        {
            const int64_t R = SpawnsAt + (int64_t)K * 0x48;
            IwctxReaders::MapEntity E;
            E.emplace_back("classname", Str(At<uint32_t>(Img, R + 4)));
            E.emplace_back("origin", Vec(R + 0x10));
            E.emplace_back("angles", Vec(R + 0x1c));
            const uint32_t N = At<uint32_t>(Img, R + 0x34);
            const int64_t Pairs = Off(PointerAt(R + 0x40));
            for (uint32_t J = 0; Pairs >= 0 && J < N && J < 256 && Pairs + (int64_t)(J + 1) * 16 <= (int64_t)Img.size(); J++)
            {
                const std::string Key = IwEntityKeys::Name(At<uint64_t>(Img, Pairs + J * 16), IwEntityKeys::Scheme::SaltFirst);
                if (Key != "classname") E.emplace_back(Key, Str(At<uint32_t>(Img, Pairs + J * 16 + 8)));
            }
            Out.push_back(std::move(E));
        }
        // the scriptable instances: count u32 0x318, 0x60-byte records at 0x388 {its scriptable 0,
        // origin 8, angles 0x14 (pitch, yaw, roll), u64 0x20 and 0x28 (one of their own each), u32
        // script strings: classname 0x30, target 0x34, targetname 0x38, script_noteworthy 0x44, one
        // more at 0x48 ("1" to "6"); u32 0x4c to 0x54}. The keys: the values the scripts look these
        // entities up by (getentitylessscriptablearray(value, #key): 1 010 matches over the 7 zm maps,
        // each in its field only); zm_sat_hornet's 11 lanterns linked by target. A field not named
        // yet written by its offset when not 0
        const uint32_t Count = At<uint32_t>(Img, Ents + 0x318);
        const int64_t Records = Off(PointerAt(Ents + 0x388));
        for (uint32_t K = 0; Records >= 0 && K < Count && Records + (int64_t)(K + 1) * 0x60 <= (int64_t)Img.size(); K++)
        {
            const int64_t R = Records + (int64_t)K * 0x60;
            IwctxReaders::MapEntity E;
            const auto Named = [&](const char* Key, int64_t At_) {
                if (const uint32_t V = At<uint32_t>(Img, R + At_); V != 0) E.emplace_back(Key, Str(V));
            };
            Named("classname", 0x30);
            Named("targetname", 0x38);
            Named("target", 0x34);
            Named("script_noteworthy", 0x44);
            E.emplace_back("origin", Vec(R + 8));
            E.emplace_back("angles", Vec(R + 0x14));
            const int64_t Def = Off(PointerAt(R));
            E.emplace_back("scriptable", Def >= 0 ? GameBlackOps7::HashName("scriptable", At<uint64_t>(Img, Def)) : std::string());
            for (const int64_t F : { 0x20, 0x28 })
                if (const uint64_t V = At<uint64_t>(Img, R + F); V != 0) E.emplace_back(std::format("0x{:02x}", F), std::format("{:016x}", V));
            Named("0x48", 0x48);
            for (const int64_t F : { 0x3c, 0x40, 0x4c, 0x50, 0x54, 0x58, 0x5c })
                if (const uint32_t V = At<uint32_t>(Img, R + F); V != 0) E.emplace_back(std::format("0x{:02x}", F), std::format("0x{:08x}", V));
            Out.push_back(std::move(E));
        }
    }


    class BlackOps7Game final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "bo7"; }
        const char* Title() const override { return "Call of Duty: Black Ops 7"; }
        const char* Engine() const override { return "BO7"; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<BO7Zone>(std::string()); }

        // (the install index opens every zone, the 41 333 transient (_tr) ones included: a map's
        // references name assets of other maps' transient zones and of other modes' items)

        // its zones open on the index's workers: their decompression and walk share nothing, its
        // handlers and fixups run under the session lock, ResolveZones is serialized
        bool IndexInParallel() const override { return true; }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions& Options,
                                                  std::string& Error) const override
        {
            // the packages the readers stream mesh and image data from (--install: the cod25 folder or the HQ install)
            if (!Options.Install.empty() && TreyarchPackages::Open<XSUBCache>(Options.Install) == nullptr)
            {
                Error = "could not open the packages of " + Options.Install;
                return nullptr;
            }
            auto Zone = std::make_unique<BO7Zone>(Path);
            Zone->Install = Options.Install;
            Zone->Indexing = Options.Indexing;
            Zone->Game = this;
            Zone->Options = Options;
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const BlackOps7Game Game;
    const DiskGame::Registration Registered(Game);
}
