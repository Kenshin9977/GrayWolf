#include "stdafx.h"

// Modern Warfare 2019 (IW8) on the disk template: a ZonekitZone with the iw8 profile, served to the
// live Modern Warfare readers (GameModernWarfare4.cpp). The install is a Battle.net CASC storage:
// its fast files are pulled with their .fp update patches (Games/IW8/Container.cpp applies them),
// and the streamed meshes and images are read from its .xpak packages (XPAKCache, opened once as
// CoDAssets' package cache). What is IW8's own: the LOD surfaces the game fills in at load time,
// the script strings (inline for -2 and -1), and the zones the game itself refuses.

#include "Games/Common/MeshDecode.h"
#include "CoDAssets.h"
#include "CoDFileSystem.h"
#include "CoDPackageCache.h"
#include "DiskGame.h"
#include "ExportWriters.h"
#include "GameModernWarfare4.h"
#include "MapLive.h"
#include "ScriptExport.h"
#include "Games/IW8/EntityKeys.h"
#include "Games/IW8/VfxLayout.h"
#include "Games/IW/IWMaterialBooks.h"
#include "Games/IW/IwWeaponStats.h"
#include "Games/IW/IWZone.h"
#include "Games/Treyarch/TreyarchEffects.h"
#include "Games/Treyarch/TreyarchPackages.h"
#include "XPAKCache.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace
{
    // XModel (0x2b0, MW4XModel): the LOD count u8 at 0xa, six 0x40 LOD records from 0xe0, each its
    // xmodelsurfs at +0 and the surfaces the game copies from it at +8; XModelSurfs (0x60) keeps
    // them at +8
    IWZone::LodLayout ModelLods()
    {
        IWZone::LodLayout L;
        L.Lods = 0xe0, L.LodSize = 0x40, L.LodCount = 0xa, L.LodSurfs = 8;
        L.LodModelSurfs = 0, L.ModelSurfsSurfs = 8;
        return L;
    }

    // GfxImage (0xe8, MW4GfxImage): the name at 0, the in-zone pixels at 0xe0, their size at 0x1c
    constexpr int64_t ImageNameAt = 0, ImageLoadedSizeAt = 0x1c, ImagePixelsAt = 0xe0;

    // XFileVersions (0x73ec7d0 in the dump): the zone version the game accepts for each value of the
    // header's byte 0x12 (0x36a5de0 refuses any other). The .fp's patched header (at 0xc0) holds the
    // version the patched zone has.
    constexpr uint32_t AcceptedVersions[] = { 0x10c1, 0x10c1, 0xff3, 0xff3, 0xff3, 0xff3, 0x10c1, 0xfcd, 0xfcd,
                                              0xfcd,  0xfcd,  0xfcd, 0xfcd, 0xfcd, 0xfcd, 0xfcd,  0xfcd };
    bool Refused(const std::string& FastFilePath, std::string& Why)
    {
        std::ifstream F(FastFilePath, std::ios::binary);
        uint8_t H[0x20] = {};
        if (!F.read(reinterpret_cast<char*>(H), sizeof(H)) || std::memcmp(H, "IWffa100", 8) != 0) return false;
        uint32_t Version = 0;
        std::memcpy(&Version, H + 0xc, 4);
        const uint8_t Index = H[0x12];
        fs::path Patch(FastFilePath);
        Patch.replace_extension(".fp");
        std::ifstream P(Patch, std::ios::binary);
        uint8_t PH[0xd0] = {};
        if (P && P.read(reinterpret_cast<char*>(PH), sizeof(PH)) && std::memcmp(PH, "IWffd100", 8) == 0 &&
            std::memcmp(PH + 0xc0, "IWffa100", 8) == 0)
            std::memcpy(&Version, PH + 0xc0 + 0xc, 4);
        if (Index < std::size(AcceptedVersions) && Version == AcceptedVersions[Index]) return false;
        Why = std::format("the game refuses this zone: version {:#x}, its kind {} wants {:#x}", Version, Index,
                          Index < std::size(AcceptedVersions) ? AcceptedVersions[Index] : 0u);
        return true;
    }

    // Material (0x78, the material table): the image count u8 at 0x1c, the 0x10-byte slots {u32
    // semantic, image* 8} at 0x48, the atlas rows and columns u8 0x22, 0x23 (vfx_fluid_water_drips_loop:
    // 1 x 16 over a 512 x 128 image), the techset at 0x40. A zone's materials are name-only headers
    // (",name"): the game defines them in the techsets_ zones it loads first, the level's own
    // (techsets_<level>) and the common ones.
    IWMaterialBooks::Layout MaterialLayout()
    {
        IWMaterialBooks::Layout L;
        L.Profile = "iw8";
        L.ImageCount = 0x1c, L.Images = 0x48, L.ImageName = ImageNameAt;
        L.AtlasRows = 0x22, L.AtlasCols = 0x23, L.Techset = 0x40;
        return L;
    }

    // The techsets_ zones defining what a zone's headers name: the zone's own, then the common and
    // global ones, beside the .ff; pulled there from the install's storage when missing
    std::vector<fs::path> BookZones(const std::string& Path)
    {
        const fs::path Dir = fs::path(Path).parent_path();
        std::error_code Ec;
        if (!fs::exists(Dir / "techsets_common.ff", Ec) && CoDAssets::GamePackageCache != nullptr)
        {
            std::string Error;
            TreyarchPackages::ExtractFastFiles(CoDAssets::GamePackageCache.get(), "techsets_*.ff", Dir.string(), Error, ".fp");
        }
        std::vector<fs::path> Out;
        if (const fs::path Own = Dir / ("techsets_" + fs::path(Path).filename().string()); fs::exists(Own, Ec)) Out.push_back(Own);
        for (const fs::path& P : IWMaterialBooks::ZonesMatching(Dir, "techsets_(common|global)[a-z0-9_]*[.]ff")) Out.push_back(P);
        return Out;
    }

    // ---- the weapon stats' layout (Games/IW/IwWeaponStats.h): offsets read off the code of the
    // dump (iw8_flat.exe, RVAs). The weapon table 0x131aa260 holds the weapon assets (0x290), the
    // table 0x131acb00 their definitions (weapon + 8, 0x1740), both indexed by the weapon index
    // (0x35c3cc0 fills entry 0). The getters walk the weapon's attachments (0x35f1d30: the slot
    // lists at weapon 0x30 + slot * 0x10) before the definition. Several MWIII proof functions
    // were found here by their code (mnemonic shape, MWIII's are the same functions one engine later).
    const IwWeaponStats::Layout& StatsLayout()
    {
        static const IwWeaponStats::Layout L = [] {
            using F = IwWeaponStats::Field;
            constexpr auto Def = F::In::Def, Weapon = F::In::Weapon;
            IwWeaponStats::Layout L;
            // weapon (0x290): name 0, definition 8, display name key 0x18 (a string, WEAPON/AR_MIKE4);
            // 15 slots at 0x30 + slot * 0x10 {u32 count 0, attachment ** 8} (0x35f1d30: list[index - 1])
            L.WeaponName = 0, L.WeaponDef = 8, L.WeaponDisplay = 0x18, L.Slots = 0x30, L.SlotList = 8, L.SlotCountAt = 0, L.SlotCount = 15;
            L.DisplayIsKey = true, L.AssetNameAt = 0;
            L.LocalizeHash = [](const std::string& Key) {
                uint64_t H = 0xCBF29CE484222325ull;
                for (const char C : Key) H = (H ^ (uint8_t)std::tolower((unsigned char)C)) * 0x100000001B3ull;
                return H & 0x7FFFFFFFFFFFFFFFull;
            };
            // class i32 0x208 (0x35f2d00; the builtin 0x375e750 names it from the table 0xa1303e0)
            L.DefClass = 0x208, L.ClassNames = { "rifle", "sniper", "mg", "smg", "spread", "pistol", "grenade", "rocketlauncher",
                                                   "turret", "throwingknife", "non-player", "item", "beam", "ball", "none" };
            // base animation package 0x98 (0x35f3540: its time block at 0x10, a u16 per time)
            L.DefAnimPkg = 0x98;
            L.FireTime = F(0x190, Def), L.BurstCount = F(0x21c, Def), L.FireType = F(0x214, Def), L.ClipSize = F(0x1e8, Weapon),
            L.StartAmmo = F(0x41c, Def), L.MaxAmmo = F(0x430, Def), L.ReloadType = F(0x240, Def), L.ExplosionRadius = F(0x74c, Def),
            L.ExplosionRadiusAlt = F(0x7d4, Def), L.ExplosionAlt = F(0xf01, Def), L.SprintOutScale = F(0x1a0, Def);
            // the damage table: records of 0x28 at 0xe8 (profile 0, the player's), i32 damages 0-0x10,
            // f32 ranges 0x14-0x20. The hit location multipliers: 22 floats behind the pointer 0xf28
            // (0x35e3530 returns it, 0xf30 under u8 0x172f; its callers 0x3726f40 and 0x62d8900 index it
            // by the hit location, x 4)
            L.Damage = F(0xe8, Def), L.Hit = F(0xf28, Def), L.HitIsPointer = true;
            // attachment (0x458): name 0, clip scale 0x2b8 (0: none, the code skips it), ammo override
            // block 0x148 (clip i32 at 8, start 4, max 0), explosion override block 0x208 (i32 0,
            // alternate 0x1c)
            L.AttachmentName = 0, L.AttClipScale = 0x2b8, L.AttClipOverride = 0x148, L.AttExplosion = 0x208, L.AttExplosionAlt = 0x1c;
            L.AttZeroScaleIsNone = true;
            // animation package (0x40): time block 0x10, 0xffff a time it has not (0x35f3540); reload 0x46
            // (0x36027d0; 0x48 under a trait), empty 0x4c (0x3601240; 0x4e)
            L.PkgBlock = 0x10, L.PkgReload = 0x46, L.PkgReloadEmpty = 0x4c, L.PkgFfffIsNone = true;
            L.Title = "Modern Warfare (2019) weapon stats";
            L.Columns =
                "\nA value its zone was not read for stays empty: the guns, their attachments and animation packages are\n"
                "in global_stream_mp, the display names in the language's code_post_gfx (fra_code_post_gfx). Export them\n"
                "into the same --out (a .txt list of zone paths works) and the table is rebuilt from every facts file.\n\n"
                "Columns and units (RVAs of iw8_flat.exe, the dump of ModernWarfare.exe). This game keeps no\n"
                "weapontunables asset: one set per weapon, its fields in the weapon (W, 0x290) and its definition (D,\n"
                "behind weapon 8, 0x1740). Each getter takes an attachment's override first (0x35f1d30 lists them).\n"
                "- class: D i32 0x208 (0x35f2d00), named by the table 0xa1303e0 (builtin 0x375e750): rifle, sniper, mg,\n"
                "  smg, spread, pistol, grenade, rocketlauncher, turret, throwingknife, non-player, item, beam, ball, none.\n"
                "- fire_time_ms: D 0x190 (0x35de310, its builtin 0x375ef00 returns it x 0.001; 0x1a4 under u8 D 0x172d\n"
                "  and a player state, 0x198 for another mode, a wind-up ramp from D 0x1334/0x133c, attachments' 0x31c\n"
                "  override and 0x324 scale not applied). rpm = 60000 / fire_time_ms.\n"
                "- fire_type: D i32 0x214 (0x35f38a0; an attachment's block 0xa8 overrides it); the names of the table\n"
                "  0xa130510: 0 Full Auto, 1 Single Shot, 2 Burst, 3 Double Barrel, 4 Double Barrel (Dual Trigger), 5\n"
                "  Beam. burst_count: D 0x21c (0x35f2c70), which the builtin 0x375e5b0 returns only for fire type 2.\n"
                "- clip_size: W 0x1e8 (0x35d8e60). start_ammo: D 0x41c (0x3605420; 0x420 for some weapon types).\n"
                "  max_ammo: D 0x430 (0x35c3cc0 copies it to the table 0x131af3a0 that 0x35fe050 reads).\n"
                "- reload_ms, reload_empty_ms: the base animation package (D 0x98), u16 0x46 and 0x4c of its time block\n"
                "  (0x10; 0x36027d0, 0x3601240; 0x48 and 0x4e under a trait), before attachments and perks; empty\n"
                "  when 0xffff, a time the package has not (0x35f3540).\n"
                "- reload_type: D i32 0x240 (0x35e8cb0; an attachment's block 0x108 overrides it). Its values are\n"
                "  not named for this game (0 for the 870's round-by-round reload too): per-round times not given.\n"
                "- damage_max .. damage_min at range_*: D 0xe8, the player's record (0x28: i32 damages 0-0x10, f32\n"
                "  ranges 0x14-0x20, profile 0; other profiles follow); 0x35e4f70 returns the i32 0 (min) and 0x10\n"
                "  (max) (the min and max damage builtins' common function), 0x35ea6d0 the four ranges (inches / 39.37,\n"
                "  a range of 0 unused); a record whose f32 0x24 is set uses two ranges (0x35e3160), not read here.\n"
                "  The alternate record D 0x168 (u8 D 0x172c) and attachments' records (block 0x150) not applied.\n"
                "- explosion_radius_m: 0x35cf210: D 0x74c, or 0x7d4 when u8 D 0xf01 is set; an attachment's block\n"
                "  0x208 overrides it. Inches / 39.37. Empty when 0.\n"
                "- hit_*: 22 f32 behind the pointer D 0xf28 (0x35e3530 returns it; D 0xf30 under u8 D 0x172f and a\n"
                "  player state, an attachment's 0x2a8 first: not applied), indexed by the hit location (0x3726f40,\n"
                "  0x62d8900: table + location x 4), in the order of the table 0x9beee80. Most weapons share another's\n"
                "  table by a back reference (878 of global_stream_mp's 998 definitions).\n"
                "- Not given for this game (empty): ADS times, muzzle velocity, first raise, raise, drop and reload\n"
                "  add times, the sprint-out times\n"
                "  (0x3604c40 takes u16 0x1ec, or 0x1f8 under a flag it decrypts at run time, then a value of another\n"
                "  table when it has one; 0x1ec is 0 for the MP5 and the M19: not the whole story), movement scales\n"
                "  and speeds, the tunables sets and tags.\n";
            L.AttachmentNotes =
                "\nweapon_attachments.csv: each weapon with one attachment (every slot's list, weapon 0x30 + slot *\n"
                "0x10 {u32 count, list at 8}, as 0x35f1d30 reads them). weapon_loadouts.csv: the lines of loadouts.txt\n"
                "in this folder, each loadout. How the attachments combine (the code):\n"
                "- clip_size: the i32 at 8 of an attachment's block 0x148 when >= 0, else (int)(clip x (1 + sum(attachment\n"
                "  0x2b8 - 1))) (0x35d8e60; a scale of 0 is skipped).\n"
                "- The animation package overrides (D 0x1470), the attachments' ADS and modifier blocks are not read\n"
                "  for this game.\n";
            return L;
        }();
        return L;
    }

    class IW8Zone final : public IWZone
    {
    public:
        IW8Zone(const std::string& Path, std::string CacheDir)
            : IWZone(Path), Books(MaterialLayout(), std::move(CacheDir), [Path] { return BookZones(Path); })
        {
            using namespace DiskGame;
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
            ExportsScriptFiles(Scripts::GscToolBin("iw8"));
            // rawfiles and Lua files as the earlier titles keep them; the string table its own way:
            // stringtable (0x30) {name, u32 columns 8, rows 0xc, i32 values 0x10, a u16 value index
            // per cell 0x18, the values' u32 hashes 0x20, the values' strings 0x28}
            ExportsIwRawfiles();
            Exports(Kind::Data, [this](const Context& C, Totals& T) {
                ExportStringTables(C, T, "stringtable", [this](int64_t Pos, uint32_t& Columns, uint32_t& Rows, std::vector<std::string>& Cells) {
                    return PooledCells(Pos, Columns, Rows, Cells);
                });
            });
            // the weapon assets as struct dumps: each named by the string at its 0 (no name hash, IdAt
            // -1); weapon (0x290) its definition behind 8 (0x1740), attachment 0x458, animpkg 0x40,
            // suit 0x720; the fields the weapon stats prove named (StatsLayout)
            {
                const auto& L = StatsLayout();
                ExportsStructDumps(Kind::Weapons, { { "weapon", 0, IwWeaponStats::DumpFields(L, "weapon"), -1 },
                                                    { "attachment", 0, IwWeaponStats::DumpFields(L, "attachment"), -1 },
                                                    { "animpkg", 0, IwWeaponStats::DumpFields(L, "animpkg"), -1 },
                                                    { "suit", 0, {}, -1 },
                                                    { "weapontrigger", 0, {}, -1 } });
            }
            // the weapon stats table: this zone's facts, then the table rebuilt from every zone's
            Exports(Kind::WeaponStats, [this](const Context& C, Totals& T) {
                IwWeaponStats::ZoneView V;
                V.Image = &Image();
                V.Walk = &Walk();
                V.Off = [this](uint64_t A) { return Off(A); };
                V.Text = [this](int64_t Field) { return TextAt(Field); };
                V.LocalizeText = [this](int64_t Pos) {
                    std::string Key, Text;
                    LocalizeEntry(Pos, Key, Text);
                    return Text;
                };
                IwWeaponStats::Export(StatsLayout(), V, fs::path(FastFilePath()).stem().string(), C, T);
            });
            // vfx (ParticleSystemDef) as the vfx core's JSON (Games/IW/Vfx.h); a material's images
            // and atlas from the techsets_ zones' books, the zone's materials being name-only headers
            Exports(Kind::Fx, [this](const Context& C, Totals& T) {
                auto Z = EffectView();
                Z.ReadMaterials(C, Treyarch::MaterialReader([](uint64_t A) { return GameModernWarfare4::ReadXMaterial(A); },
                                                            [this](uint64_t A) { return Off(A); }),
                                false);
                Vfx::Counts N;
                ExportEffects(C, T, "vfx", [&](int64_t Pos, const std::string& Name, std::string& J) {
                    if (!Vfx::ReadSystem(Z, IW8::VfxLayout(), Pos, Name, J, &N)) return false;
                    FromBooks(J);
                    return true;
                });
                // their materials' images, the others needed from the zones holding them
                if (C.Kinds.count(Kind::Image) && !N.Images.empty()) ExportTextures(N.Images, C, T);
                for (const auto& [K, V] : N.Named()) Counter(K) += V;
            });
        }

        // an effect's materials with their atlas grid and techset, which their name-only headers here
        // lack: the books' (each module's "assets" entries naming a material)
        void FromBooks(std::string& Json)
        {
            auto J = nlohmann::ordered_json::parse(Json, nullptr, false);
            if (J.is_discarded()) return;
            bool Changed = false;
            const std::function<void(nlohmann::ordered_json&)> Visit = [&](nlohmann::ordered_json& V) {
                if (V.is_array())
                    for (auto& E : V) Visit(E);
                if (!V.is_object()) return;
                IWMaterialBooks::Extra X;
                if (const auto M = V.find("material"); M != V.end() && M->is_string() && Books.Details(M->get<std::string>(), X))
                {
                    if (!V.contains("atlas") && X.Rows > 0 && X.Cols > 0) V["atlas"] = { X.Rows, X.Cols };
                    if (V.value("techset", std::string()).empty() && !X.Techset.empty()) V["techset"] = X.Techset;
                    Changed = true;
                }
                for (auto& [K, E] : V.items()) Visit(E);
            };
            Visit(J);
            if (Changed) Json = J.dump(1, '\t', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
        }

        bool PooledCells(int64_t Pos, uint32_t& Columns, uint32_t& Rows, std::vector<std::string>& Cells) const
        {
            Columns = At<uint32_t>(Image(), Pos + 8), Rows = At<uint32_t>(Image(), Pos + 0xc);
            const int32_t Values = At<int32_t>(Image(), Pos + 0x10);
            const uint64_t All = (uint64_t)Columns * Rows;
            Cells.clear();
            if (All == 0) return true;
            const int64_t Index = Off(PointerAt(Pos + 0x18)), Strings = Off(PointerAt(Pos + 0x28));
            if (All > (1u << 24) || Values <= 0 || Index < 0 || Strings < 0 || (uint64_t)Index + All * 2 > Image().size() ||
                (uint64_t)Strings + (uint64_t)Values * 8 > Image().size())
                return false;
            Cells.reserve((size_t)All);
            for (uint64_t K = 0; K < All; K++)
            {
                const uint16_t V = At<uint16_t>(Image(), Index + (int64_t)K * 2);
                Cells.push_back(V < Values ? TextAt(Strings + (int64_t)V * 8) : std::string());
            }
            return true;
        }

        ~IW8Zone() override
        {
            if (IsServed()) GameModernWarfare4::ImagesElsewhere = nullptr;
        }

        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;

        // the material headers the techsets_ zones' books filled, and those they did not
        void Measures(DiskGame::Totals& T) const override
        {
            ZonekitZone::Measures(T);
            Books.Measure(T, "materials_from_techsets", "materials_in_no_techsets_zone");
        }

    protected:
        const char* Profile() const override { return "iw8"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("IW8"); }
        VirtualGameSession::Handlers Handlers() override { return { SlotString, DiskImage }; }
        // resolve_deref 0x2d961d0: an asset's back reference names the slot holding its pointer
        bool DerefAssetPointers() const override { return true; }
        void Fixups() override
        {
            Served(true);   // opened: the zone the handlers reach
            Slots = InlineScriptStrings(Zone());
            Counter("lods_without_surfaces") += LinkLodSurfaces(ModelLods());
        }
        void Defines(std::vector<DiskGame::AssetId>& Out) const override
        {
            ZonekitZone::Defines(Out);
            DefineImages(Out);
        }
        std::string IndexKey() const override { return ZonekitZone::IndexKey() + "_images1"; }
        void Served(bool On) override
        {
            GameModernWarfare4::ImagesElsewhere = nullptr;
            if (On)
                GameModernWarfare4::ImagesElsewhere = [this](const std::string& Name, IWMaterialBooks::ImageList& Out) {
                    return Books.Find(Name, Out);
                };
        }
        // localize (0x10): the key at 0, the text at 8 encrypted with the string cipher
        // (strings.cipher.txt, DecryptString 0x2642700); a text the table cannot decrypt is left empty
        void LocalizeEntry(int64_t Pos, std::string& Key, std::string& Text) const override
        {
            Key = StringAt(PointerAt(Pos));
            if (!CipherDecrypt(StringAt(PointerAt(Pos + 8)), Text)) Text.clear();
        }

    private:
        // a material's texture: the live reader over the served zone, its streamed levels from the packages
        static std::unique_ptr<XImageDDS> DiskImage(const XImage_t& Image)
        {
            return ServedZone<IW8Zone>() == nullptr ? nullptr : GameModernWarfare4::LoadXImage(Image);
        }

        // the zone's sound banks: <zone>.sabl / <zone>.sabs beside the .ff, pulled from the install's
        // CASC storage (the packages' file system) when they are not there yet. A <zone>.sabs is whole
        // there: the .sabs_1 / .sabs_2 the storage also lists (fra_common_base_mp, mp_br_quarry) are
        // its two spans (the first under the bank's own content key, the second byte for byte its
        // end), not banks of their own.
        // Its patch bank, patch_<zone>.sabs, goes with it: the exe builds both paths from the zone's
        // name (iw8_flat.exe 0x6b63dd0: "%s%s%s" or "%s\%s_%s%s%s", folder, language, "patch_" or "",
        // zone, extension) and the patch bank names the zone in its header; it holds the entries the
        // patch changed (same key, new data) and added (new keys): ExportSoundBanks' Patches.
        void ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T)
        {
            std::vector<std::string> Banks, Patches;
            CoDFileSystem* FS = CoDAssets::GamePackageCache != nullptr ? CoDAssets::GamePackageCache->GetFileSystem() : nullptr;
            const fs::path Zone(FastFilePath());
            const auto Local = [&](const fs::path& Bank, std::vector<std::string>& To) {
                std::error_code Ec;
                if (!fs::exists(Bank, Ec) && FS != nullptr) Pull(*FS, Bank);
                if (fs::exists(Bank, Ec)) To.push_back(Bank.string());
            };
            for (const char* Ext : { ".sabl", ".sabs" })
                Local(fs::path(Zone).replace_extension(Ext), Banks);
            // a language's zone (french\fra_<zone>) has its patch as french\fra_patch_<zone>
            const std::string Stem = Zone.stem().string();
            std::string Patch = "patch_" + Stem;
            const size_t Under = Stem.find('_');
            if (Under != std::string::npos)
            {
                // a zone in a language's folder in the storage; without a storage, the one beside it
                const std::string Language = Stem.substr(0, Under) + "_patch_" + Stem.substr(Under + 1);
                std::error_code Ec;
                if (FS != nullptr ? StorageName(*FS, Stem + ".ff").find_first_of("\\/") != std::string::npos
                                  : fs::exists(Zone.parent_path() / (Language + ".sabs"), Ec))
                    Patch = Language;
            }
            Local(Zone.parent_path() / (Patch + ".sabs"), Patches);
            ExportSoundBanks(Banks, C, T, Patches);
        }

        // File's name in the storage (its folder with it), "" when it has none
        static std::string StorageName(CoDFileSystem& FS, const std::string& File)
        {
            const std::string Want = ExportNaming::Lower(File);
            std::string Name;
            FS.EnumerateFiles("*" + File, [&](const std::string& N, const size_t) {
                if (ExportNaming::Lower(fs::path(N).filename().string()) == Want) Name = N;
            });
            return Name;
        }

        // Local's file name, from anywhere in the storage (a localized zone's banks are one folder down)
        static void Pull(CoDFileSystem& FS, const fs::path& Local)
        {
            const std::string Want = ExportNaming::Lower(Local.filename().string());
            std::string Name;
            size_t Size = 0;
            FS.EnumerateFiles("*" + Local.filename().string(), [&](const std::string& N, const size_t S) {
                if (ExportNaming::Lower(fs::path(N).filename().string()) == Want) Name = N, Size = S;
            });
            if (Name.empty() || Size == 0) return;
            HANDLE H = FS.OpenFile(Name, "r");
            if (H == NULL) return;
            auto Buf = std::make_unique<uint8_t[]>(Size);
            const size_t Got = FS.Read(H, Buf.get(), 0, Size);
            FS.CloseHandle(H);
            if (Got != Size) return;
            const fs::path Temp = fs::path(Local.string() + ".part");
            {
                std::ofstream F(Temp, std::ios::binary | std::ios::trunc);
                F.write(reinterpret_cast<const char*>(Buf.get()), (std::streamsize)Size);
                if (!F) return;
            }
            std::error_code Ec;
            fs::rename(Temp, Local, Ec);
        }

        void ExportImages(const DiskGame::Context& C, DiskGame::Totals& T)
        {
            ImageList L;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "image")) L.Order.push_back(I.Pos);
            L.NameAt = ImageNameAt;
            // what the file is made of: the header past its name (format, sizes, the levels' package
            // keys), the in-zone pixels when there are any, and the extension
            L.KeyOf = [this](int64_t Pos, const std::string& Extension) {
                uint64_t H = 0xcbf29ce484222325ull;
                const auto Mix = [&H](uint8_t V) { H = (H ^ V) * 0x100000001b3ull; };
                const auto& Img = Image();
                for (int64_t K = 8; K < ImagePixelsAt && Pos + K < (int64_t)Img.size(); K++) Mix(Img[(size_t)(Pos + K)]);
                const int64_t Pixels = Off(PointerAt(Pos + ImagePixelsAt));
                const uint32_t Size = At<uint32_t>(Img, Pos + ImageLoadedSizeAt);
                if (Pixels >= 0 && Pixels + (int64_t)Size <= (int64_t)Img.size())
                    for (uint32_t K = 0; K < Size; K++) Mix(Img[(size_t)Pixels + K]);
                for (const char Ch : Extension) Mix((uint8_t)Ch);
                return H ? H : 1;
            };
            L.Decode = [this](int64_t Pos, const std::string& Name) {
                return GameModernWarfare4::LoadXImage(XImage_t(ImageUsageType::Unknown, 0, Live().Addr((uint64_t)Pos), Name));
            };
            L.FailWhy = [this](int64_t Pos) {
                return std::string(Off(PointerAt(Pos + ImagePixelsAt)) >= 0 ? "in zone" : "streamed");
            };
            L.OnePerName = true;
            L.NamesChecked = true;
            ExportImageList(C, T, L);
        }

        IWMaterialBooks::Lookup Books;
    };

    // ---- the level (C2M's ModernWarfare4.cs, checked against the gfx_map loader's table)
    // GfxWorld (gfx_map, 0x4610): its base name at 8; GfxWorldDraw inline at 0xc8 (surface count u32
    // 0xc8, buffer count u32 0xcc, 0x28-byte surfaces at 0x100, 88-byte buffer records at 0x148);
    // GfxWorldDpvsStatic inline at 0x190 (model count u32 0x194, 16-byte models at 0x1d0, the
    // XModel at 0; collection count u32 0x198, 16-byte collections at 0x1e0; instance count u32
    // 0x1a0, 24-byte placements at 0x2e0); brush models (count i32 0x3e70, 0x60 each at 0x3e78:
    // first surface u32 84, surface count u32 88).
    constexpr int64_t WorldBaseName = 8, SurfaceCountAt = 0xc8, SurfacesAt = 0x100, BuffersAt = 0x148;
    constexpr int64_t SModelCountAt = 0x194, SModelsAt = 0x1d0, CollectionCountAt = 0x198, CollectionsAt = 0x1e0;
    constexpr int64_t InstanceCountAt = 0x1a0, InstancesAt = 0x2e0;
    constexpr int64_t BrushCountAt = 0x3e70, BrushesAt = 0x3e78, BrushSize = 0x60, BrushFirst = 84, BrushSurfaces = 88;
    // A surface (0x28): its vertex count u16 8, triangle count u16 10, first index u32 12 (into the
    // transient zone's index buffer, its indices counted in its buffer), material 0x10, buffer u32 0x18
    constexpr int64_t SurfaceSize = 0x28, SurfVertices = 8, SurfTris = 10, SurfFirstIndex = 12, SurfMaterial = 0x10, SurfBuffer = 0x18;
    // A buffer record (88): its transient zone i32 0, uv layers i32 4, then byte offsets: the
    // positions (3 floats) in the zone's position data 8, the packed normals (u32) 12, the colours
    // (4 bytes) 20 and the uvs (2 floats per layer) 24 in its other data
    constexpr int64_t BufferSize = 88, BufLayers = 4, BufPositions = 8, BufNormals = 12, BufColors = 20, BufUvs = 24;
    // GfxWorldTransientZone (gfx_map_trzone, 0x148): position data size u32 0x10, other data size
    // u32 0x14, their pointers 0x18 and 0x20; index count u32 0xa8, the u16 indices at 0xb0
    constexpr int64_t TrPosSize = 0x10, TrAuxSize = 0x14, TrPos = 0x18, TrAux = 0x20, TrIndexCount = 0xa8, TrIndices = 0xb0;

    bool IW8Zone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        const int64_t World = Root("gfx_map");
        const int64_t Tr = Root("gfx_map_trzone");
        if (World < 0) return false;
        const auto& Img = Image();
        const int64_t Size = (int64_t)Img.size();
        FILE* Dbg = Maps::DebugFile();
        Scene.Name = StringAt(PointerAt(World + WorldBaseName));
        if (Scene.Name.empty()) Scene.Name = Name();

        // ---- the world: one transient zone here (a multiplayer map's own), its buffers by number
        const uint32_t SurfaceCount = At<uint32_t>(Img, World + SurfaceCountAt);
        const uint32_t BufferCount = At<uint32_t>(Img, World + SurfaceCountAt + 4);
        const int64_t Surfaces = Off(PointerAt(World + SurfacesAt)), Buffers = Off(PointerAt(World + BuffersAt));
        const int64_t Pos = Tr < 0 ? -1 : Off(PointerAt(Tr + TrPos)), Aux = Tr < 0 ? -1 : Off(PointerAt(Tr + TrAux));
        const int64_t PosSize = Tr < 0 ? 0 : At<uint32_t>(Img, Tr + TrPosSize), AuxSize = Tr < 0 ? 0 : At<uint32_t>(Img, Tr + TrAuxSize);
        const uint32_t IndexCount = Tr < 0 ? 0 : At<uint32_t>(Img, Tr + TrIndexCount);
        const int64_t Indices = Tr < 0 ? -1 : Off(PointerAt(Tr + TrIndices));
        const int32_t BrushCount = At<int32_t>(Img, World + BrushCountAt);
        const int64_t Brushes = Off(PointerAt(World + BrushesAt));
        if (Dbg)
            std::fprintf(Dbg, "[map] %s surfaces %u at %llx buffers %u at %llx positions %llx (%lld) other %llx (%lld) indices %u at %llx brushes %d\n",
                         Scene.Name.c_str(), SurfaceCount, (long long)Surfaces, BufferCount, (long long)Buffers, (long long)Pos,
                         (long long)PosSize, (long long)Aux, (long long)AuxSize, IndexCount, (long long)Indices, BrushCount);
        // a vertex: its buffer's number in the high 16 bits, its index in that buffer in the low
        const Maps::WorldBuilder::Fetch Fetch = [&](uint32_t Key, Maps::Vertex& V) {
            const uint32_t B = Key >> 16, I = Key & 0xffff;
            if (B >= BufferCount || Buffers < 0 || Pos < 0 || Aux < 0) return false;
            const int64_t Rec = Buffers + (int64_t)B * BufferSize;
            const int64_t P = At<int32_t>(Img, Rec + BufPositions) + (int64_t)I * 12;
            if (P < 0 || P + 12 > PosSize) return false;
            for (int K = 0; K < 3; K++) V.Pos[K] = At<float>(Img, Pos + P + K * 4);
            const int64_t N = At<int32_t>(Img, Rec + BufNormals) + (int64_t)I * 4;
            const int64_t Cl = At<int32_t>(Img, Rec + BufColors) + (int64_t)I * 4;
            const int32_t Layers = std::max(At<int32_t>(Img, Rec + BufLayers), 1);
            const int64_t Uv = At<int32_t>(Img, Rec + BufUvs) + (int64_t)I * 8 * Layers;
            if (N >= 0 && N + 4 <= AuxSize)
            {
                const Vector3 Normal = MeshDecode::UnpackPackedQuat(At<uint32_t>(Img, Aux + N));
                V.Normal[0] = Normal.X, V.Normal[1] = Normal.Y, V.Normal[2] = Normal.Z;
            }
            if (Cl >= 0 && Cl + 4 <= AuxSize) std::memcpy(V.Color, Img.data() + Aux + Cl, 4);
            if (Uv >= 0 && Uv + 8 <= AuxSize)
                for (int K = 0; K < 2; K++) V.Uv[K] = At<float>(Img, Aux + Uv + K * 4);
            return true;
        };
        std::unordered_map<uint64_t, std::string> Materials;
        const auto MaterialOf = [&](uint64_t Address) {
            auto Found = Materials.find(Address);
            if (Found == Materials.end() && Dbg && Off(Address) >= 0)
                std::fprintf(Dbg, "[mat] %llx '%s' images %u table %llx techset %llx\n", (unsigned long long)Address, StringAt(PointerAt(Off(Address))).c_str(),
                             At<uint8_t>(Img, Off(Address) + 0x1c), (unsigned long long)PointerAt(Off(Address) + 0x48), (unsigned long long)PointerAt(Off(Address) + 0x40));
            if (Found == Materials.end())
                Found = Materials.emplace(Address, Off(Address) < 0 ? std::string()
                                                                    : Maps::AddMaterial(Scene, GameModernWarfare4::ReadXMaterial(Address))).first;
            return Found->second;
        };
        const auto Range = [&](uint32_t B) {
            const int64_t Brush = Brushes + (int64_t)B * BrushSize;
            const uint32_t First = At<uint32_t>(Img, Brush + BrushFirst);
            return std::make_pair(First, First + At<uint32_t>(Img, Brush + BrushSurfaces));
        };
        const bool Readable = Surfaces >= 0 && Brushes >= 0 && Indices >= 0;
        Maps::BuildWorld(Scene, Readable ? (uint32_t)std::max(BrushCount, 0) : 0, SurfaceCount, Range, Fetch, [&](uint32_t S, Maps::WorldBuilder& Mesh) {
            const int64_t Sf = Surfaces + (int64_t)S * SurfaceSize;
            const uint32_t Buffer = At<uint32_t>(Img, Sf + SurfBuffer);
            const uint16_t Vertices = At<uint16_t>(Img, Sf + SurfVertices), Tris = At<uint16_t>(Img, Sf + SurfTris);
            const uint32_t FirstIndex = At<uint32_t>(Img, Sf + SurfFirstIndex);
            if (Buffer >= BufferCount || Buffer > 0xffff)
            {
                Counter("map_surfaces_unread") += 1;
                return;
            }
            const std::string& Material = MaterialOf(PointerAt(Sf + SurfMaterial));
            for (uint32_t T = 0; T < Tris; T++)
            {
                const int64_t I = (int64_t)FirstIndex + (int64_t)T * 3;
                if (I + 2 >= IndexCount || Indices + (I + 3) * 2 > Size) break;
                uint32_t Key[3];
                bool Inside = true;
                for (int K = 0; K < 3; K++)
                {
                    const uint16_t Local = At<uint16_t>(Img, Indices + (I + K) * 2);
                    Inside = Inside && Local < Vertices;
                    Key[K] = Buffer << 16 | Local;
                }
                if (!Inside || !Mesh.Triangle(Material, Key[0], Key[1], Key[2])) Counter("map_triangles_unread") += 1;
            }
        });

        // ---- the static models: collections of placements of one model each, a placement its
        // origin (3 i32, 1/4096 units), its rotation (4 u16 over [-1, 1], x y z w) and its scale
        const uint32_t ModelCount = At<uint32_t>(Img, World + SModelCountAt);
        const uint32_t CollectionCount = At<uint32_t>(Img, World + CollectionCountAt);
        const uint32_t InstanceCount = At<uint32_t>(Img, World + InstanceCountAt);
        const int64_t SModels = Off(PointerAt(World + SModelsAt)), Collections = Off(PointerAt(World + CollectionsAt));
        const int64_t Instances = Off(PointerAt(World + InstancesAt));
        for (uint32_t K = 0; SModels >= 0 && Collections >= 0 && Instances >= 0 && K < CollectionCount; K++)
        {
            const int64_t Col = Collections + (int64_t)K * 16;
            const uint32_t FirstInstance = At<uint32_t>(Img, Col), Count = At<uint32_t>(Img, Col + 4);
            const uint16_t Model = At<uint16_t>(Img, Col + 8);
            if (Model >= ModelCount) continue;
            const uint64_t Address = PointerAt(SModels + (int64_t)Model * 16);
            const std::string ModelName = ModelAt(Address);
            if (ModelName.empty()) continue;
            Models.emplace(ModelName, Address);
            for (uint32_t J = FirstInstance; J < FirstInstance + Count && J < InstanceCount; J++)
            {
                const int64_t P = Instances + (int64_t)J * 24;
                Maps::Instance In = Maps::PackedPlacement(Img, P, false);
                In.Name = "smodel_" + std::to_string(J);
                In.Model = ModelName;
                Scene.StaticInstances.push_back(In);
            }
        }
        if (Dbg)
            std::fprintf(Dbg, "[map] static models %u collections %u instances %u placed %zu\n", ModelCount, CollectionCount, InstanceCount,
                         Scene.StaticInstances.size());

        // ---- the scriptables (the iw8 tables): map_ents (0x470) holds their instances in a struct
        // inline at 0x200 (0xd8: count u32 +4, 0x48-byte records +0x38: the scriptable 0, origin 8,
        // angles 0x14 (pitch, yaw, roll), C2M's MW4ScriptableInstance); a scriptable (0x70) lists its
        // models at 0x60 (count u32 0x5c, xmodel pointers): the first is placed, as C2M does (the
        // others are its parts and states, a vehicle's wheels and damaged mirrors, which the game
        // attaches to the first one's tags)
        if (const int64_t Ents = Root("map_ents"); Ents >= 0)
        {
            const uint32_t Count = At<uint32_t>(Img, Ents + 0x204);
            const int64_t Records = Off(PointerAt(Ents + 0x238));
            size_t Placed = 0;
            for (uint32_t K = 0; Records >= 0 && K < Count && Records + (int64_t)(K + 1) * 0x48 <= Size; K++)
            {
                const int64_t Rec = Records + (int64_t)K * 0x48, Def = Off(PointerAt(Rec));
                if (Def < 0 || Def + 0x70 > Size)
                {
                    Counter("map_scriptables_without_definition")++;
                    continue;
                }
                float Angles[3];
                Maps::Instance In;
                for (int A = 0; A < 3; A++) In.Origin[A] = At<float>(Img, Rec + 8 + A * 4), Angles[A] = At<float>(Img, Rec + 0x14 + A * 4);
                Maps::AnglesToAxis(Angles, In.Axis);
                const uint32_t ModelsIn = At<uint32_t>(Img, Def + 0x5c);
                const int64_t List = Off(PointerAt(Def + 0x60));
                for (uint32_t M = 0; List >= 0 && M < ModelsIn && M < 1 && List + (int64_t)(M + 1) * 8 <= Size; M++)
                {
                    const uint64_t Address = PointerAt(List + (int64_t)M * 8);
                    const std::string ModelName = ModelAt(Address);
                    if (ModelName.empty())
                    {
                        Counter("map_scriptable_models_unread")++;
                        continue;
                    }
                    Models.emplace(ModelName, Address);
                    In.Name = "scriptable_" + std::to_string(K) + (M ? "_" + std::to_string(M) : std::string());
                    In.Model = ModelName;
                    Scene.DynamicInstances.push_back(In);
                    Placed++;
                }
            }
            Counter("map_scriptables") += Count;
            Counter("map_scriptable_instances") += Placed;
        }

        // ---- ComWorld's lights (com_map: count u32 0x30, 0xa0-byte records at 0x38: the type byte
        // 1, colour 32, direction 44, origin 68, radius 80, cosHalfFovOuter 108, the definition 0x98)
        LightLayout L;
        L.Count = 0x30, L.Array = 0x38, L.Size = 0xa0, L.TypeAt = 1;
        L.Color = 32, L.Dir = 44, L.Origin = 68, L.Radius = 80, L.CosHalfFov = 108, L.Def = 0x98;
        AddPrimaryLights(Scene, L);
        AddMapEnts(Scene);
        for (const auto& [Number, Key] : IW8Keys::EntityKeys) Scene.EntityKeys[std::to_string(Number)] = Key;
        return true;
    }

    class ModernWarfare2019Game final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "mw2019"; }
        const char* Title() const override { return "Call of Duty: Modern Warfare (2019)"; }
        const char* Engine() const override { return "IW8"; }
        std::vector<std::string> Aliases() const override { return { "mw2019-disk-export" }; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<IW8Zone>(std::string(), std::string()); }

        // a .ff, a folder of them, or casc:<pattern> pulled from the install (--install) with the .fp patches
        bool Zones(const std::string& Input, const DiskGame::ExportOptions& Options, std::vector<std::string>& Out,
                   std::string& Error) const override
        {
            if (Input.rfind("casc:", 0) != 0) return IDiskGame::Zones(Input, Options, Out, Error);
            CoDPackageCache* P = Packages(Options, Error);
            if (P != nullptr) Out = TreyarchPackages::ExtractFastFiles(P, Input.substr(5), FastFileDir(Options), Error, ".fp");
            if (P != nullptr && Out.empty()) Error = "no fast file matches " + Input + " " + Error;
            return !Out.empty();
        }

        bool ListZones(const std::string& Install, const DiskGame::ExportOptions& Options, std::vector<std::string>& Names,
                       std::string& Error) const override
        {
            CoDPackageCache* P = TreyarchPackages::Open<XPAKCache>(Install);
            if (P != nullptr) Names = TreyarchPackages::ListFastFiles(P);
            if (Names.empty()) Error = "no fast file in " + Install;
            return !Names.empty();
        }

        // the index opens the base and localized zones only: the 25 000 transient (_tr) zones would
        // take hours, and a run exports one of them directly
        bool IndexedZones(const std::string& Install, const DiskGame::ExportOptions& Options, std::vector<std::string>& Names,
                          std::string& Error) const override
        {
            if (!ListZones(Install, Options, Names, Error)) return false;
            std::erase_if(Names, [](const std::string& N) { return N.size() > 6 && N.compare(N.size() - 6, 6, "_tr.ff") == 0; });
            return !Names.empty();
        }

        bool ResolveZones(const std::string& Install, const std::vector<std::string>& Names, const DiskGame::ExportOptions& Options,
                          std::vector<std::string>& Out, std::string& Error) const override
        {
            return TreyarchPackages::ExtractZones(TreyarchPackages::Open<XPAKCache>(Install), Install, Names, FastFileDir(Options), Out, Error, ".fp");
        }

        // its zones open on the index's workers: their decompression and walk share nothing, its
        // handlers and fixups run under the session lock, ResolveZones is serialized
        bool IndexInParallel() const override { return true; }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions& Options,
                                                  std::string& Error) const override
        {
            if (Refused(Path, Error)) return nullptr;
            // the packages the live readers stream mesh and image data from (--install)
            if (!Options.Install.empty() && TreyarchPackages::Open<XPAKCache>(Options.Install) == nullptr)
            {
                Error = "could not open the packages of " + Options.Install;
                return nullptr;
            }
            auto Zone = std::make_unique<IW8Zone>(Path, (fs::path(FastFileDir(Options)) / "iw8_materials").string());
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }

    private:
        static std::string FastFileDir(const DiskGame::ExportOptions& Options)
        {
            return !Options.Cache.empty() ? Options.Cache : (fs::temp_directory_path() / "greyhound_iw8ff").string();
        }
        static CoDPackageCache* Packages(const DiskGame::ExportOptions& Options, std::string& Error)
        {
            CoDPackageCache* P = Options.Install.empty() ? nullptr : TreyarchPackages::Open<XPAKCache>(Options.Install);
            if (P == nullptr) Error = "casc: zones need --install";
            return P;
        }
    };

    const ModernWarfare2019Game Game;
    const DiskGame::Registration Registered(Game);
}
