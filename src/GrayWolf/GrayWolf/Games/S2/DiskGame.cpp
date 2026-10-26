#include "stdafx.h"

// WWII (S2) on the disk template: a ZonekitZone with the s2 profile, served to the live WWII
// readers (GameWorldWar2). What is WWII's own: the LOD surfaces the game fills in at load time,
// streamed images named by a hash in the .ff header and found through the imagefile*.toc, streamed
// meshes through the xpakfile*.toc (XPTOCCache), and sounds through the aliases (soundfile*.pak).

#include "CoDAssets.h"
#include "DiskGame.h"
#include "ExportWriters.h"
#include "GameWorldWar2.h"
#include "Games/IW/IWMaterialBooks.h"
#include "Games/IW/IWPakSounds.h"
#include "Games/IW/IWSoundAliases.h"
#include "FileSystems.h"
#include "Games/IW/IWZone.h"
#include "Games/IW/IwEffects.h"
#include "Games/IW/IwWeaponDefs.h"
#include "Games/IW/IwZoneAnimOwners.h"
#include "PAKCache.h"
#include "Strings.h"
#include "XPTOCCache.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
    // XModel (xmodel, 0x30) points at its XModelBase (0x378): six 0x50 LOD records from 0x88,
    // the LOD count at 0x268, a LOD's surfaces at +0x48
    constexpr int64_t ModelBaseAt = 8, LodCountAt = 0x268;
    IWZone::LodLayout ModelLods()
    {
        IWZone::LodLayout L;
        L.Owner = "xmodelbase";
        L.Lods = 0x88, L.LodSize = 0x50, L.LodCount = LodCountAt, L.LodSurfs = 0x48;
        return L;
    }
    constexpr int64_t CompositeFlags = 0x20;
    // SoundAlias (sound, 0x20): its entries at 8, their count a u8 at 0x18; an entry (0x150)
    // points at its file spec (0x40, the type a u8 at 0: 1 loaded, 2 primed, 3 streamed) at 0x28
    constexpr int64_t AliasEntries = 8, AliasEntryCount = 0x18, AliasEntrySize = 0x150, EntrySpec = 0x28;
    constexpr uint32_t CompositeFlag = 0x200;

    // GfxImage (0x60): pixels at 0x20 (resident in the zone, or null when streamed), four mip
    // records {u16 width, u16 height, u32} from 0x28, the format at 0x48 (GameWorldWar2::DxgiFormat),
    // the resident size at 0x54, the map type at 0x58 (5: cube).
    constexpr int64_t ImagePixels = 0x20, ImageMips = 0x28, ImageFormatAt = 0x48, ImageMapType = 0x58;
    constexpr int Levels = 4;

    // The .ff header's two tables, before the compressed zone: a u32 count at 0x20 and that many
    // 16-byte entries {u64 key, u32, u32 flags}, then the same for sounds.
    // Images: four entries per streamed image (levels 0 to 3, smallest first; all zero when the
    // level is absent), in the order the zone loads its streamed images; the key is the first 8
    // bytes of the image's 16-byte hash in the imagefile*.toc.
    // Sounds: one entry {key, size, flags} per loaded sound and per primed or streamed file spec, in load
    // order; flags bit 0 is a localized sound (the language folder's toc); an all-zero entry is a
    // loaded sound whose bytes are in the zone. The zone leaves those buffers zeroed and the
    // spec's pack fields empty: the game fills them in from the soundfile*.toc.
    constexpr int64_t StreamTableAt = 0x20;
    struct HeaderEntry
    {
        uint64_t Key = 0;
        uint32_t Value = 0, Flags = 0;
    };

    std::vector<HeaderEntry> ReadHeaderTable(const std::string& FastFilePath, int Which)
    {
        std::vector<HeaderEntry> Out;
        std::ifstream F(FastFilePath, std::ios::binary);
        uint32_t Count = 0;
        F.seekg(StreamTableAt);
        for (int T = 0; T <= Which; T++)
        {
            F.read(reinterpret_cast<char*>(&Count), 4);
            if (!F || Count > (1u << 22)) return Out;
            if (T < Which) F.seekg((std::streamoff)Count * 16, std::ios::cur);
        }
        Out.resize(Count);
        static_assert(sizeof(HeaderEntry) == 16, "16-byte entries");
        F.read(reinterpret_cast<char*>(Out.data()), (std::streamsize)Count * 16);
        if (!F) Out.clear();
        return Out;
    }

    // imagefile*.toc: "S2ffi100", u32 version, u32 hash size, u32 count, then 72-byte entries:
    // the 16-byte hash and four levels {u64 offset, u32 size, u16 pak} into imagefile<pak>.pak.
    // The DLC tocs (imagefile4/5/6) repeat some images in their own paks.
    struct PakLevel
    {
        uint64_t Offset = 0;
        uint32_t Size = 0;
        uint16_t Pak = 0;
    };
    using ImageLevels = std::array<PakLevel, Levels>;

    // soundfile*.toc: "S2ffi200", u32 version, u32 count, then 30-byte entries: the 16-byte hash,
    // u64 offset, u32 size, u16 pak into soundfile<pak>.pak (raw FLAC); a language folder has its
    // own, prefixed (french/fra_soundfile.toc)
    struct SoundEntry
    {
        uint64_t Offset = 0;
        uint32_t Size = 0;
        uint16_t Pak = 0;
    };

    void ReadSoundToc(const fs::path& Path, std::unordered_map<uint64_t, SoundEntry>& Out)
    {
        std::ifstream F(Path, std::ios::binary);
        char Magic[8]{};
        uint32_t Head[2]{};
        F.read(Magic, 8);
        F.read(reinterpret_cast<char*>(Head), sizeof(Head));
        if (!F || std::memcmp(Magic, "S2ffi200", 8) != 0) return;
        std::vector<uint8_t> Raw((size_t)Head[1] * 30);
        F.read(reinterpret_cast<char*>(Raw.data()), (std::streamsize)Raw.size());
        if (!F) return;
        for (uint32_t K = 0; K < Head[1]; K++)
        {
            const uint8_t* E = Raw.data() + (size_t)K * 30;
            uint64_t Key = 0;
            SoundEntry S;
            std::memcpy(&Key, E, 8);
            std::memcpy(&S.Offset, E + 16, 8);
            std::memcpy(&S.Size, E + 24, 4);
            std::memcpy(&S.Pak, E + 28, 2);
            Out.emplace(Key, S);
        }
    }

    struct InstallPackages
    {
        std::string Root;
        std::unordered_multimap<uint64_t, ImageLevels> Images;
        std::unordered_map<uint64_t, SoundEntry> SoundFiles;
        // each language folder's (french), by its name in lower case
        std::map<std::string, std::unordered_map<uint64_t, SoundEntry>> LocalizedSoundFiles;
        std::unique_ptr<XPTOCCache> Meshes;
        std::unique_ptr<PAKCache> Sounds;
    };

    void ReadImageToc(const fs::path& Path, std::unordered_multimap<uint64_t, ImageLevels>& Out)
    {
        std::ifstream F(Path, std::ios::binary);
        char Magic[8]{};
        uint32_t Head[3]{};
        F.read(Magic, 8);
        F.read(reinterpret_cast<char*>(Head), sizeof(Head));
        if (!F || std::memcmp(Magic, "S2ffi100", 8) != 0 || Head[1] != 16) return;
        std::vector<uint8_t> Raw((size_t)Head[2] * 72);
        F.read(reinterpret_cast<char*>(Raw.data()), (std::streamsize)Raw.size());
        if (!F) return;
        for (uint32_t K = 0; K < Head[2]; K++)
        {
            const uint8_t* E = Raw.data() + (size_t)K * 72;
            uint64_t Key = 0;
            std::memcpy(&Key, E, 8);
            ImageLevels L;
            for (int J = 0; J < Levels; J++)
            {
                std::memcpy(&L[J].Offset, E + 16 + J * 14, 8);
                std::memcpy(&L[J].Size, E + 24 + J * 14, 4);
                std::memcpy(&L[J].Pak, E + 28 + J * 14, 2);
            }
            Out.emplace(Key, L);
        }
    }

    // The install a zone belongs to: its folder or a parent (localized zones are in a language
    // folder), the one holding xpakfile.toc
    std::string InstallOf(const std::string& FastFilePath)
    {
        for (fs::path P = fs::path(FastFilePath).parent_path(); !P.empty(); P = P.parent_path())
        {
            std::error_code Ec;
            if (fs::exists(P / "xpakfile.toc", Ec)) return P.string();
            if (P == P.root_path()) break;
        }
        return fs::path(FastFilePath).parent_path().string();
    }

    // The install's packages, read once per install for the run: the tocs are tens of MB
    InstallPackages& PackagesFor(const std::string& FastFilePath)
    {
        static std::mutex Lock;
        static std::unique_ptr<InstallPackages> Cached;
        std::lock_guard<std::mutex> Guard(Lock);
        const std::string Root = InstallOf(FastFilePath);
        if (!Cached || Cached->Root != Root)
        {
            auto P = std::make_unique<InstallPackages>();
            P->Root = Root;
            std::error_code Ec;
            for (const auto& E : fs::directory_iterator(Root, Ec))
            {
                const std::string N = Strings::ToLower(E.path().filename().string());
                if (N.rfind("imagefile", 0) == 0 && E.path().extension() == ".toc") ReadImageToc(E.path(), P->Images);
                if (N.rfind("soundfile", 0) == 0 && E.path().extension() == ".toc") ReadSoundToc(E.path(), P->SoundFiles);
                // a language folder: its <prefix>soundfile*.toc
                if (E.is_directory(Ec))
                    for (const auto& L : fs::directory_iterator(E.path(), Ec))
                    {
                        const std::string Ln = Strings::ToLower(L.path().filename().string());
                        if (Ln.find("soundfile") != std::string::npos && L.path().extension() == ".toc") ReadSoundToc(L.path(), P->LocalizedSoundFiles[N]);
                    }
            }
            P->Meshes = std::make_unique<XPTOCCache>();
            P->Meshes->LoadPackageCache(Root);   // xpakfile.toc
            for (const char* Dlc : { "xpakfile4.toc", "xpakfile5.toc", "xpakfile6.toc" })
                if (fs::exists(fs::path(Root) / Dlc, Ec)) P->Meshes->LoadPackage((fs::path(Root) / Dlc).string());
            P->Sounds = std::make_unique<PAKCache>();
            P->Sounds->LoadPackageCache(Root);
            Cached = std::move(P);
        }
        return *Cached;
    }

    // A name-only material header names a material of the install's common zones (common*,
    // code_pre_gfx*, code_post_gfx* and their patch_ zones), which the game loads first. Material
    // (0x168): image count u8 0xa2, image table at 0xc0; an image's name at 0.
    IWMaterialBooks::Layout MaterialLayout()
    {
        IWMaterialBooks::Layout L;
        L.Profile = "s2";
        L.ImageCount = 0xa2, L.Images = 0xc0, L.ImageName = 0;
        return L;
    }
    constexpr const char* CommonZones = "(patch_)?(common|code_pre_gfx|code_post_gfx)(_[a-z0-9_]+)?[.]ff";

    // The weapon fields (IwWeaponDefs.h): the weapon asset is the WeaponDef itself (0x1090). No community
    // layout exists for WWII: each field is read off the exe (s2_mp64_flat RVAs; the getters 0x3b66b0
    // (i32) and 0x3b6500 (f32) take the field's offset, as Advanced Warfare's 0x1649d0 and 0x164760 do)
    // or found by its values on common_mp's weapons; the README says which.
    const IwWeaponDefs::Layout& WeaponLayout()
    {
        static const IwWeaponDefs::Layout L = [] {
            IwWeaponDefs::Layout L;
            L.Name = 0, L.Display = 8;
            // the exe's name tables (0x7ff63c9a02e0: weapType from entry 0, weapClass from 8, fireType from 34)
            L.Types = { "none", "bullet", "grenade", "projectile", "gas", "riotshield", "energy", "melee" };
            L.Classes = { "rifle", "sniper", "mg", "smg", "spread", "pistol", "grenade", "rocketlauncher", "turret",
                          "throwingknife", "non-player", "gas", "item", "beam", "shield", "hover", "cloak", "ping",
                          "repulsor", "adrenaline", "health", "mute", "underwater", "ball", "grapplinghook", "knife" };
            L.FireTypes = { "Full Auto", "Single Shot", "2-Round Burst", "3-Round Burst", "4-Round Burst", "5-Round Burst", "Double Barrel" };
            L.Fields = { { "weaponType", 0x4d8 }, { "weaponClass", 0x4dc }, { "fireType", 0x4f4 }, { "clipSize", 0x608 },
                         { "shotCount", 0x60c }, { "fireTime", 0x63c }, { "reloadTime", 0x66c }, { "reloadEmptyTime", 0x688 },
                         { "reloadAddTime", 0x68c }, { "reloadEmptyAddTime", 0x690 },
                         { "damage", 0xdf4 }, { "maxDamageRange", 0xe1c }, { "damage2", 0xe20 }, { "damageRange2", 0xe28 },
                         { "damage3", 0xe2c }, { "damageRange3", 0xe3c }, { "minDamage", 0xe34 }, { "locationDamageMultipliers", 0x400 },
                         { "startAmmo", 0x5f4, true, 0x5f8 }, { "maxAmmo", 0x600, true, 0x604 },
                         { "linearFalloff1", 0xdf0, true, -1, true }, { "linearFalloff2", 0xdf1, true, -1, true },
                         { "adsTransInTime", 0x9d8 }, { "adsTransOutTime", 0x9e0 },
                         // the akimbo state's timer set (0x3bbf40, 0x3c1510, 0x3c0ce0, 0x3c0ad0), for the attachment table
                         { "fireTimeAkimbo", 0x77c }, { "reloadTimeAkimbo", 0x7ac }, { "reloadEmptyTimeAkimbo", 0x7c8 },
                         { "reloadAddTimeAkimbo", 0x7cc } };
            L.ZeroRangeExtends = true;
            // the attachments (WeaponFiles' s2 rules; the README says the RVAs): the list at 0x48 (u8 count
            // 0x4d0), the defaults 0x50-0x54, the animation overrides at 0x60 (u8 count 0x4d1; reload 0x16,
            // reload empty 0x17), the class editor's pair table (code_post_gfx_mp)
            L.Attachments.List = 0x48, L.Attachments.Count = 0x4d0, L.Attachments.Defaults = 0x50, L.Attachments.DefaultCount = 5;
            L.Attachments.Anims = 0x60, L.Attachments.AnimCount = 0x4d1, L.Attachments.AnimStates = { 0x16, 0x17 };
            L.Attachments.ComboTable = "mp/attachmentcombos_mtx12.csv";
            L.Notes = IwWeaponDefs::GameNotes("WWII", "french/fra_code_post_gfx_mp", "common_mp", "the WeaponDef (the weapon asset itself)", "");
            L.Notes.Extra = { { "linear_falloff_1", "linearFalloff1", WeaponFiles::Unit::Count },
                              { "linear_falloff_2", "linearFalloff2", WeaponFiles::Unit::Count } };
            L.Notes.ExtraNotes =
                "- linear_falloff_1, linear_falloff_2 (WWII): the damage code's two flags (bytes 0xdf0, 0xdf1; GrayWolf's\n"
                "  names). 1 in linear_falloff_1: from range_1_m to range_2_m the damage goes linearly from damage_1 to\n"
                "  damage_2 instead of being damage_2 (when the weapon has a middle range: else, either flag makes the\n"
                "  whole fall-off linear, damage_1 at range_1_m down to the last damage at range_2_m). 1 in\n"
                "  linear_falloff_2: from range_2_m to range_3_m, linearly from damage_2 to the last damage instead of\n"
                "  damage_3. 0, 0: steps. In common_mp only shotguns set one (Winchester 1897, Blunderbuss, Walther).\n";
            L.Notes.Source =
                "WWII keeps no weapon field table in its exe and no community layout exists: each field is read off\n"
                "the exe's code (s2_mp64_flat.exe RVAs) or found by its values on common_mp's weapons, under the\n"
                "weapon file's name for it. By the values (each the only offset matching every weapon checked):\n"
                "weaponType 0x4d8, weaponClass 0x4dc, fireType 0x4f4 (STG-44 rifle full auto, Kar98k sniper single\n"
                "shot, MG 42 mg, MP-40 smg, M30 spread, M1911 pistol, Bazooka rocketlauncher), clipSize 0x608\n"
                "(STG-44 30, M1 Garand 8, Kar98k 5, BAR 20, M1911 7), shotCount 0x60c (M30 8), the hit location\n"
                "multipliers (the 22 floats behind 0x400). By the code: fireTime 0x63c (its akimbo twin 0x77c,\n"
                "0x1569c9), reloadTime 0x66c (0x3c154c), reloadEmptyTime 0x688 (0x3c0d19), reloadAddTime 0x68c and\n"
                "reloadEmptyAddTime 0x690 (0x3c0b7a, 0x3c0c8a): the same functions as Advanced Warfare's, whose\n"
                "fields x64-zt names. The damage (0x3bb710, the damage at a distance): damage (0xdf4) up to\n"
                "maxDamageRange (0xe1c), then midDamage (0xe20) up to midDamageRange (0xe28), mid2Damage (0xe2c) up\n"
                "to minDamageRange (0xe3c), minDamage (0xe34) beyond (the names are the exe's own: its menu function\n"
                "GetWeaponDamage, 0x11c620, returns them under these names). When midDamageRange is 0, midDamage\n"
                "holds up to minDamageRange and mid2Damage is not used (written so: damage_2 at range_2_m, then the\n"
                "last damage). Two flags (bytes 0xdf0, 0xdf1) turn a step into a linear fall-off (linear_falloff_1,\n"
                "_2 below). start_ammo: the menu function GetWeaponStartAmmo (0x3bf9a0) reads 0x5f4, rounds, or\n"
                "when it is 0 the magazines at 0x5f8 times clipSize (STG-44 6 x 30 = 180). max_ammo: the same pair\n"
                "at 0x600 / 0x604 (0x3c3290), the cap the ammo pickup code (0x55a920) subtracts the held stock from\n"
                "(STG-44 4 x 30 = 120). ads_time_ms, ads_out_time_ms: 0x9d8, 0x9e0, the in and out times the ADS\n"
                "function (0x3badf0) turns into its rates (1 / time; STG-44 220, MP-40 190, M1919 550 / 350; the Fast\n"
                "ADS attachments scale both; 0x9dc replaces the in time under a player state flag, 0x5f4 & 0x80000, not\n"
                "identified). weaponType, weaponClass and fireType are named by the exe's own name\n"
                "tables. Times are milliseconds in the asset, distances inches. The other columns (raise, drop, sprint\n"
                "times, explosions...) stay empty: no code reading them as weapon fields was found (the field getter's\n"
                "call sites never take those offsets; the times of Advanced Warfare's slots read as nothing here).\n";
            return L;
        }();
        return L;
    }

    // The anim owners (IwZoneAnimOwners.h): the weapon's view model, the first of its models array at
    // 0x10, its hands at 0x20 (the weapons' struct dumps), each a composite's parts; the zones a zone
    // loads with it. The player's
    // characters are composites with no body model of their own: the multiplayer tree plays on the
    // infantry body of single player's common zone
    const IwZoneAnimOwners::Layout& AnimOwnerLayout()
    {
        static const IwZoneAnimOwners::Layout L = [] {
            IwZoneAnimOwners::Layout L;
            L.Gun = { 0x10, 0 }, L.Hands = { 0x20 };
            // the view model and hands are composites: their parts (ExportComposites)
            L.CompositeFlagAt = CompositeFlags, L.CompositeFlag = CompositeFlag;
            L.Trees["multiplayer"] = { { "germany_infantry_body" }, "the infantry body of single player's common zone (a stand-in)" };
            L.SharedMp = { "common_mp", "common_core_mp", "common_shared_core_mp" };
            L.SharedSp = { "common" };
            L.LevelTrees = { "generic_human" };
            L.SharedWhen = { { "zombie", { "common_zm_mp", "common_core_zm_mp" } } };
            return L;
        }();
        return L;
    }

    class WW2Zone final : public IWZone
    {
    public:
        WW2Zone(const std::string& Path, std::string CacheDir)
            : IWZone(Path), Books(MaterialLayout(), std::move(CacheDir), [Path] { return IWMaterialBooks::ZonesMatching(InstallOf(Path), CommonZones); })
        {
            using namespace DiskGame;
            // the live model and anim readers, with the install's packages
            for (Kind K : { Kind::Model, Kind::Anim })
                Exports(K, [this, K, Base = *ExporterOf(K)](const Context& C, Totals& T) {
                    const IWPakSounds::PackagesInUse Using(Packages().Meshes.get());
                    Base(C, T);
                    if (K == Kind::Model) ExportComposites(C, T);
                });
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) {
                const IWPakSounds::PackagesInUse Using(Packages().Meshes.get());
                ExportMap(C, T);
            });
            ExportsScriptFiles(Scripts::GscToolBin("s2"));
            ExportsIwRawfiles();
            ExportsIwStringTables();
            ExportsIwWeapons(WeaponLayout());
            // the material and fx kinds: FxEffectDef 0x48 (elements at 0x40), elements 0x168; Material's
            // techniqueSet at 0xb8 (the s2 tables f4ac960, f4acc20 and f4b7860)
            ExportsIwEffects(IwEffects::WorldWar2(), GameWorldWar2::ReadXMaterial, 0xb8);
        }
        ~WW2Zone() override
        {
            if (IsServed()) GameWorldWar2::ImagesElsewhere = nullptr;
        }

        // the material headers the common zones' book filled, and those it did not
        void Measures(DiskGame::Totals& T) const override
        {
            ZonekitZone::Measures(T);
            Books.Measure(T, "materials_from_common", "materials_in_no_common_zone");
        }

    protected:
        const char* Profile() const override { return "s2"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("S2"); }
        VirtualGameSession::Handlers Handlers() override { return { SlotString, DiskImage }; }
        // a back reference to an asset names its slot in the asset list, which the loader points
        // at the pooled asset (resolve_deref)
        bool DerefAssetPointers() const override { return true; }
        void Fixups() override;
        void Served(bool On) override
        {
            if (!On)
            {
                GameWorldWar2::ImagesElsewhere = nullptr;
                return;
            }
            GameWorldWar2::ImagesElsewhere = [this](const std::string& Name, IWMaterialBooks::ImageList& Out) { return Books.Find(Name, Out); };
        }
        void Defines(std::vector<DiskGame::AssetId>& Out) const override;
        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;
        // the string tables, then what the anim owners read (IwZoneAnimOwners.h)
        void Documents(std::vector<std::pair<std::string, std::string>>& Out) const override
        {
            ZonekitZone::Documents(Out);
            IwZoneAnimOwners::Documents(AnimOwnerLayout(), { &Image(), &Walk(), [this](uint64_t A) { return Off(A); },
                                                             [this](int64_t F) { return TextAt(F); },
                                                             [this](const std::string& T, int64_t P) { return NamedAsset(T, P); },
                                                             [this](int64_t P, std::string& N, std::vector<uint8_t>& B) {
                                                                 N = AssetName(DiskGame::Kind::Rawfile, P);
                                                                 return RawBytes(P, { 0xc, 0x10, 8 }, B);
                                                             },
                                                             [this](uint64_t A, uint64_t N, std::vector<uint8_t>& B) { return BytesAt(A, N, B); }, &Slots },
                                        Out);
        }
        std::function<uint32_t(int64_t, uint32_t, float*, uint32_t)> BlendLayers();
        // a model whose XModelBase is not in this zone, or is a name-only header (",name", no
        // LOD): another zone defines it whole
        bool IsReference(DiskGame::Kind K, const std::string& Nm, int64_t Pos) const override
        {
            if (K == DiskGame::Kind::Model)
            {
                const int64_t Base = Off(At<uint64_t>(Image(), Pos + ModelBaseAt));
                if (!Bases.count(Base) || At<uint8_t>(Image(), Base + LodCountAt) == 0) return true;
            }
            return ZonekitZone::IsReference(K, Nm, Pos);
        }
        // what Defines lists depends on this reader too, not only on the tables
        std::string IndexKey() const override { return ZonekitZone::IndexKey() + "+s2.2"; }
        // A composite model (flag 0x200 at 0x20) holds a list of its part models at 8 instead of
        // an XModelBase: the parts are models of their own, the composite is not written
        const char* Unwritable(DiskGame::Kind K, int64_t Pos) const override
        {
            return K == DiskGame::Kind::Model && (At<uint32_t>(Zone(), Pos + CompositeFlags) & CompositeFlag) ? "models_composite" : nullptr;
        }

    private:
        static std::unique_ptr<XImageDDS> DiskImage(const XImage_t& Image);

        // A composite model's parts (the live reader writes no composite): <name>/<name>.parts.json
        // beside the models, each entry of its list (count u8 0x2a, 0x30 bytes at 8, the xmodel loader
        // f4c6b20's: the part's xmodel at 0) as the part model's name and the entry's other fields. A
        // weapon's view and world models are composites: their parts are the weapon's default parts.
        void ExportComposites(const DiskGame::Context& C, DiskGame::Totals& T)
        {
            using DiskGame::Kind;
            const auto& Img = Image();
            // the merged models' textures, written by the image kind as ExportLive's are: an image held
            // here or in a zone served beside, renamed as the image export names it
            std::unordered_set<int64_t> Images;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, ImageType())) Images.insert(I.Pos);
            std::set<std::string> TextureNames;
            ExportWriters::ModelTextures Tex = Textures(C);
            if (ExporterOf(Kind::Image) != nullptr)
                Tex.Take = [&](XImage_t& X) {
                    if (X.ImagePtr == 0 || X.ImageName.empty()) return false;
                    if (X.ImagePtr != ExportWriters::ImageElsewhere && !Images.count((int64_t)(X.ImagePtr - Live().Origin())) &&
                        (!ImageBeside(X.ImagePtr) || X.ImageName[0] == '$'))
                        return false;
                    X.ImageName = Referenced(Kind::Image, X.ImageName);
                    TextureNames.insert(X.ImageName);
                    X.ImageName = ExportNaming::SafeFileName(X.ImageName);
                    return true;
                };
            for (const auto& I : Walk().Instances)
            {
                if (!OwnerIs(I, "xmodel") || (At<uint32_t>(Img, I.Pos + CompositeFlags) & CompositeFlag) == 0) continue;
                const std::string Model = AssetName(Kind::Model, I.Pos);
                if (ZonekitZone::IsReference(Kind::Model, Model, I.Pos) || !C.Wants(Kind::Model, Model)) continue;
                const int64_t List = Off(At<uint64_t>(Img, I.Pos + 8));
                const uint8_t Count = At<uint8_t>(Img, I.Pos + 0x2a);
                std::string Json = "{\n\t\"model\": \"" + Model + "\",\n\t\"parts\": [";
                for (uint8_t N = 0; List >= 0 && N < Count; N++)
                {
                    const int64_t E = List + 0x30 * (int64_t)N;
                    const int64_t Part = Off(At<uint64_t>(Img, E));
                    std::string PartName = Part >= 0 ? AssetName(Kind::Model, Part) : std::string();
                    if (Part >= 0 && IsReference(Kind::Model, PartName, Part)) PartName = Referenced(Kind::Model, PartName);
                    Json += std::string(N ? "," : "") + "\n\t\t{ \"model\": \"" + PartName + "\"";
                    // (read by their values, not traced in the exe: a quaternion, x y z w, the identity on
                    // every weapon's part read; then three floats, 0; a u32, 8; 8 zero bytes)
                    const auto F = [&](int64_t W) { return std::format("{}", At<float>(Img, E + W)); };
                    Json += ", \"rotation\": [" + F(8) + ", " + F(0xc) + ", " + F(0x10) + ", " + F(0x14) + "], \"offset\": [" + F(0x18) + ", " + F(0x1c) +
                            ", " + F(0x20) + "]" + std::format(", \"0x24\": {}, \"0x28\": \"0x{:016x}\" }}", At<uint32_t>(Img, E + 0x24), At<uint64_t>(Img, E + 0x28));
                }
                Json += "\n\t]\n}\n";
                const fs::path File = fs::path(C.KindDir(Kind::Model)) / ExportNaming::SafeFileName(Model) / (ExportNaming::SafeFileName(Model) + ".parts.json");
                if (ExportWriters::WriteBytes(File, (const uint8_t*)Json.data(), Json.size())) Counter("models_composite_parts_written")++;
                else T.Fail(Name(), Kind::Model, Model, "parts write failed");
                // the composite as one model: its parts held here, in place (the entries' rotations are
                // the identity and their offsets 0 on every composite read), bones joined by name
                std::vector<CoDModel_t> Parts;
                for (uint8_t N = 0; List >= 0 && N < Count; N++)
                {
                    const int64_t Part = Off(At<uint64_t>(Img, List + 0x30 * (int64_t)N));
                    if (Part < 0 || IsReference(Kind::Model, AssetName(Kind::Model, Part), Part) || Unwritable(Kind::Model, Part) != nullptr)
                    {
                        Counter("models_composite_parts_elsewhere")++;
                        continue;
                    }
                    CoDModel_t M;
                    M.AssetName = ExportNaming::SafeFileName(AssetName(Kind::Model, Part));
                    M.AssetPointer = Live().Addr((uint64_t)Part);
                    Parts.push_back(M);
                }
                if (Parts.empty()) continue;
                size_t Read = 0;
                std::string Crash;
                const int Code = ExportWriters::ExportComposite(ExportNaming::SafeFileName(Model), Parts, File.parent_path().string(), Tex, Read, Crash);
                if (Code == static_cast<int>(ExportGameResult::Success)) Counter("models_composite_written")++, Counter("models_composite_parts_merged") += Read;
                else T.Fail(Name(), Kind::Model, Model, "composite code " + std::to_string(Code) + (Crash.empty() ? "" : " " + Crash));
            }
            if (!TextureNames.empty()) ExportTextures(TextureNames, C, T);
        }

        InstallPackages& Packages() const { return PackagesFor(FastFilePath()); }
        bool Resident(int64_t Pos) const;
        // the image's largest level with data: its size and where it is
        bool Largest(int64_t Pos, uint32_t& Width, uint32_t& Height, PakLevel& Where, std::string& Pak) const;
        std::unique_ptr<XImageDDS> Decode(int64_t Pos, ImageUsageType Usage);
        void ExportImages(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T);
        // the sound header table's entry of each loaded sound and primed or streamed file spec, by position
        std::unordered_map<int64_t, HeaderEntry> SoundFiles();

        std::unordered_map<int64_t, uint64_t> Streams;   // streamed image position -> its key
        std::unordered_set<int64_t> Bases;               // the XModelBases walked here
        bool NoPixelsCounted = false;
        IWMaterialBooks::Lookup Books;
    };

    bool WW2Zone::Resident(int64_t Pos) const
    {
        const uint64_t Raw = At<uint64_t>(Zone(), Pos + ImagePixels);
        return Raw == Rules().Marker || Raw == Rules().Resident;
    }

    bool WW2Zone::Largest(int64_t Pos, uint32_t& Width, uint32_t& Height, PakLevel& Where, std::string& Pak) const
    {
        const auto It = Streams.find(Pos);
        if (It == Streams.end()) return false;
        const auto& P = Packages();
        const auto [First, Last] = P.Images.equal_range(It->second);
        for (int L = Levels - 1; L >= 0; L--)
        {
            Width = At<uint16_t>(Image(), Pos + ImageMips + L * 8);
            Height = At<uint16_t>(Image(), Pos + ImageMips + L * 8 + 2);
            if (Width == 0 || Height == 0) continue;
            // the first toc whose pak is here (the DLC tocs share images)
            for (auto E = First; E != Last; ++E)
            {
                const PakLevel& Lv = E->second[(size_t)L];
                if (Lv.Size == 0) continue;
                const std::string Path = (fs::path(P.Root) / std::format("imagefile{}.pak", (unsigned)Lv.Pak)).string();
                std::error_code Ec;
                if (!fs::exists(Path, Ec)) continue;
                Where = Lv;
                Pak = Path;
                return true;
            }
        }
        return false;
    }

    std::unique_ptr<XImageDDS> WW2Zone::Decode(int64_t Pos, ImageUsageType Usage)
    {
        const XImage_t Info(Usage, 0, Live().Addr((uint64_t)Pos), AssetName(DiskGame::Kind::Image, Pos));
        // pixels resident in the zone, not in a .pak
        if (Resident(Pos)) return GameWorldWar2::LoadResidentXImage(Info);
        uint32_t Width = 0, Height = 0;
        PakLevel Where;
        std::string Pak;
        if (!Largest(Pos, Width, Height, Where, Pak)) return nullptr;
        return GameWorldWar2::DecodePakImage(Pak, Where.Offset, Width, Height, At<uint8_t>(Image(), Pos + ImageFormatAt),
                                             At<uint8_t>(Image(), Pos + ImageMapType) == 5, Usage);
    }

    std::unique_ptr<XImageDDS> WW2Zone::DiskImage(const XImage_t& Image)
    {
        WW2Zone* Current = ServedZone<WW2Zone>();
        if (Current == nullptr || !Current->Live().Contains(Image.ImagePtr)) return nullptr;
        return Current->Decode((int64_t)(Image.ImagePtr - Current->Live().Origin()), Image.ImageUsage);
    }

    void WW2Zone::Fixups()
    {
        Served(true);   // opened: the zone the handlers reach
        // the script strings: count at 0, marker-tested slots from 0x28, then their text
        const auto& Z = Zone();
        const int32_t Count = At<int32_t>(Z, 0);
        if (At<uint64_t>(Z, 8) != 0 && Count > 0 && (uint64_t)Count <= Z.size() / 8)
            Slots = InlineStrings(Z, 0x28, (uint64_t)Count, 8, Rules().Marker);

        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, "xmodelbase")) Bases.insert(I.Pos);
        if (const size_t Unlinked = LinkLodSurfaces(ModelLods())) Counter("lod_surfaces_unresolved") += Unlinked;

        // streamed images (no pixels here, some mip described; a ",name" header has neither), in
        // walk order, take the stream table's keys four by four
        std::vector<int64_t> Streamed;
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "image") || At<uint64_t>(Z, I.Pos + ImagePixels) != 0) continue;
            bool Described = false;
            for (int64_t K = 0; K < Levels * 8; K += 8) Described = Described || At<uint64_t>(Z, I.Pos + ImageMips + K) != 0;
            if (Described) Streamed.push_back(I.Pos);
        }
        const std::vector<HeaderEntry> Keys = ReadHeaderTable(FastFilePath(), 0);
        Counter("images_streamed") += Streamed.size();
        if (Keys.size() == Streamed.size() * Levels)
        {
            for (size_t K = 0; K < Streamed.size(); K++)
                for (int L = 0; L < Levels; L++)
                    if (Keys[Levels * K + L].Key != 0)
                    {
                        Streams[Streamed[K]] = Keys[Levels * K + L].Key;
                        break;
                    }
        }
        else if (!Streamed.empty())
            Counter("image_tables_mismatched")++;
    }

    void WW2Zone::ExportImages(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        // the images with pixels: resident ones in walk order, then the streamed ones in pak order
        std::vector<int64_t> Order;
        std::vector<std::tuple<uint16_t, uint64_t, int64_t>> Paked;
        size_t NoPixels = 0;
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "image")) continue;
            if (Resident(I.Pos))
            {
                Order.push_back(I.Pos);
                continue;
            }
            uint32_t W = 0, H = 0;
            PakLevel Where;
            std::string Pak;
            if (Largest(I.Pos, W, H, Where, Pak)) Paked.emplace_back(Where.Pak, Where.Offset, I.Pos);
            else if (!IsReference(DiskGame::Kind::Image, AssetName(DiskGame::Kind::Image, I.Pos), I.Pos)) NoPixels++;
        }
        std::sort(Paked.begin(), Paked.end());
        for (const auto& P : Paked) Order.push_back(std::get<2>(P));
        if (!NoPixelsCounted) Counter("images_without_pixels") += NoPixels;   // once, the kind runs for textures too
        NoPixelsCounted = true;

        ImageList L;
        L.Order = std::move(Order);
        // what the output file is made of: the pak level or the zone's pixels, the format and the
        // extension
        L.KeyOf = [this](int64_t Pos, const std::string& Extension) {
            ExportWriters::ContentKey Key;
            uint32_t W = 0, Ht = 0;
            PakLevel Where;
            std::string Pak;
            if (Largest(Pos, W, Ht, Where, Pak))
                Key.Mix(Where.Pak).Mix(Where.Offset).Mix(Pak);
            else
            {
                // resident: the pixels themselves
                const uint64_t Pixels = At<uint64_t>(Image(), Pos + ImagePixels);
                const int64_t At0 = Off(Pixels);
                const uint32_t Size = At<uint32_t>(Image(), Pos + 0x54);
                if (At0 < 0 || At0 + (int64_t)Size > (int64_t)Image().size()) return (uint64_t)0;
                for (uint32_t K = 0; K + 8 <= Size; K += 8) Key.Mix(At<uint64_t>(Image(), At0 + K));
                Key.Mix(Size);
            }
            return Key.Mix(At<uint64_t>(Image(), Pos + ImageMips)).Mix(At<uint8_t>(Image(), Pos + ImageFormatAt)).Mix(Extension).Key();
        };
        // a normal map by its name; the sink's rebuilds (no name) as colour
        L.Decode = [this](int64_t Pos, const std::string& Name) {
            return Decode(Pos, (Name.find("_nml") != std::string::npos) ? ImageUsageType::NormalMap : ImageUsageType::DiffuseMap);
        };
        L.FailWhy = [this](int64_t Pos) { return std::format("format {}", (unsigned)At<uint8_t>(Image(), Pos + ImageFormatAt)); };
        L.OnePerName = true;
        ExportImageList(C, T, L);
    }

    void WW2Zone::Defines(std::vector<DiskGame::AssetId>& Out) const
    {
        ZonekitZone::Defines(Out);
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "image")) continue;
            if (!Resident(I.Pos) && !Streams.count(I.Pos)) continue;
            const std::string N = AssetName(DiskGame::Kind::Image, I.Pos);
            if (!IsReference(DiskGame::Kind::Image, N, I.Pos)) Out.push_back({ DiskGame::Kind::Image, N });
        }
    }

    // The level (the s2 tables): GfxWorld (gfx_map, 0x10c8) with GfxWorldDraw at 0xc0 (its u16
    // indices: count 0x308, array 0x310) and GfxWorldDpvsStatic at 0xca8 (0x38-byte surfaces at
    // +0x2f0: first vertex 4, vertex count u16 8, triangle count u16 0xa, first index 0x18, material
    // 0x20; static models 0x80 each at +0x310, the model at 0x48), 96-byte brush models at 0xb50
    // (count 0xb48); the vertices as streams in gfx_map_trzone (0x98: count 0xc, positions 0x10,
    // colours 0x20, uvs 0x28, normals 0x38); ComWorld's primary lights 0xa8 each (count 0x10, array
    // 0x18, the definition's name at 0xa0); col_map_mp's dynamic entities 0x78 each (counts 0x338,
    // lists 0x340). The surfaces' fields and the static model's placement (origin 0, axis 12,
    // scale 48) checked on mp_egypt_02_dogfight's records.
    Maps::ClipLayout ClipBrushes(int64_t InfoAt);
    Maps::TriggerLayout TriggerShapes();
    IWZone::MapLayout Level()
    {
        IWZone::MapLayout M;
        M.IndexCount = 0xc0 + 0x308, M.Indices = 0xc0 + 0x310;
        M.VertexAsset = "gfx_map_trzone", M.VertexCount = 0xc;
        M.Streams.Positions = 0x10, M.Streams.Colors = 0x20, M.Streams.Uvs = 0x28, M.Streams.Normals = 0x38;
        M.BrushCount = 0xb48, M.Brushes = 0xb50, M.BrushSize = 96, M.BrushFirstSurface = 84, M.BrushSurfaceCount = 88;
        M.SModelCount = 0xca8, M.Surfaces = 0xca8 + 0x2f0, M.SModels = 0xca8 + 0x310, M.SModelSize = 0x80, M.SModelModel = 0x48;
        M.SurfaceSize = 0x38, M.SurfaceMaterial = 0x20, M.SurfaceFirstVertex = 4, M.SurfaceTris = 0xa, M.SurfaceBaseIndex = 0x18;
        M.Clip = "col_map_mp", M.DynCounts = 0x338, M.DynLists = 0x340, M.DynSize = 0x78;
        M.Lights.Count = 0x10, M.Lights.Array = 0x18, M.Lights.Size = 0xa8;
        M.Lights.Color = 12, M.Lights.Dir = 28, M.Lights.Origin = 52, M.Lights.Radius = 88, M.Lights.CosHalfFov = 92, M.Lights.Def = 0xa0;
        M.ImageNameAt = 0;
        M.ClipBrushes = ClipBrushes(0x10);
        M.ClipBrushes.ModelCount = 0x2e0, M.ClipBrushes.Models = 0x2e8;
        M.Triggers = 0x18, M.TriggerShapes = TriggerShapes();
        return M;
    }

    // A blended surface's other layers' UVs: its material is named "*<a>_<b>_..." (a layer per
    // name, the first drawn by the vertex's own uv). gfx_map_trzone's layer data (size u64 0x80,
    // bytes 0x88) holds from the surface's offset (u32 at 0) a record per vertex of its range: the
    // other layers' uvs (two floats each), then a u32 per other layer (not read): 12 bytes a layer.
    // Checked on mp_egypt_02_dogfight: its six ranges (2 to 4 layers) fill the 221 520 bytes exactly.
    std::function<uint32_t(int64_t, uint32_t, float*, uint32_t)> WW2Zone::BlendLayers()
    {
        const int64_t Tr = Root("gfx_map_trzone");
        if (Tr < 0) return {};
        const auto& Img = Image();
        const int64_t Data = Off(At<uint64_t>(Img, Tr + 0x88));
        const uint64_t Size = At<uint64_t>(Img, Tr + 0x80);
        if (Data < 0 || Size == 0 || (uint64_t)Data + Size > Img.size()) return {};
        auto Layers = std::make_shared<std::unordered_map<uint64_t, uint32_t>>();   // by material
        return [this, &Img, Data, Size, Layers](int64_t Surface, uint32_t Local, float* Out, uint32_t Max) -> uint32_t {
            const uint64_t Material = At<uint64_t>(Img, Surface + 0x20);
            auto It = Layers->find(Material);
            if (It == Layers->end())
            {
                const std::string Name = Off(Material) >= 0 ? StringAt(At<uint64_t>(Img, Off(Material))) : std::string();
                const uint32_t N = !Name.empty() && Name[0] == '*' ? (uint32_t)std::count(Name.begin(), Name.end(), '_') : 0;
                It = Layers->emplace(Material, N).first;
                if (N) Counter("map_materials_blended")++;
            }
            const uint32_t N = It->second;
            if (N == 0) return 0;
            const uint64_t At0 = At<uint32_t>(Img, Surface) + (uint64_t)Local * N * 12;
            if (Local >= At<uint16_t>(Img, Surface + 8) || At0 + N * 12 > Size)
            {
                Counter("map_uv_layers_outside")++;
                return 0;
            }
            const uint32_t Read = std::min(N, Max);
            std::memcpy(Out, Img.data() + Data + At0, (size_t)Read * 8);
            return Read;
        };
    }

    // The collision (the s2 tables): a ClipInfo (0x2a8)
    // keeps planes, materials and leaf brush nodes; its first col_map_trzone pointer (0x228) the
    // brushes (a table at 0x10), the collision triangles and their partitions (a vertex segment of
    // 4096). cmodel_t 0x68: ClipInfo pointer 0x20, leafBrushNode 0x58.
    Maps::ClipLayout ClipBrushes(int64_t InfoAt)
    {
        Maps::ClipLayout C;
        C.InfoAt = InfoAt;
        C.Info.PlaneCount = 0, C.Info.Planes = 8, C.Info.MaterialCount = 0x10, C.Info.Materials = 0x18;
        C.Info.LeafBrushNodeCount = 0x20, C.Info.LeafBrushNodes = 0x28;
        C.Tables = 0x228, C.TablesAt = 0x10;
        C.Info.BrushCount = 0xc, C.Info.Brushes = 0x10, C.Info.BrushBounds = 0x18, C.Info.BrushContents = 0x20;
        C.BrushContentsSize = 8;
        C.MaterialSize = 0x18, C.MaterialName = 0, C.MaterialSurface = 8, C.MaterialContents = 0x10;
        C.BrushSize = 0x18, C.BrushSides = 0, C.BrushSideCount = 8, C.BrushSideCountBytes = 4, C.AxialMaterial = 0xc;
        C.SideSize = 8, C.SidePlane = 0, C.SidePlaneNumber = true, C.SideMaterial = 4, C.SideMaterialBytes = 4;
        C.ModelSize = 0x68, C.ModelInfo = 0x20, C.ModelLeafBrushNode = 0x58;
        C.NodeSize = 0x20, C.NodeCount = 4, C.NodeCountBytes = 4, C.NodeBrushes = 0x10, C.NodeBrushBytes = 4, C.NodeChildren = 0x18;
        C.VertexCount = 0x38, C.Vertices = 0x40, C.TriangleCount = 0x48, C.Triangles = 0x50;
        C.PartitionCount = 0x70, C.Partitions = 0x78, C.PartitionSize = 0x10, C.SegmentVertices = 4096;
        return C;
    }

    // MapTriggers' records, widened: models 0x10 (hull count 8, first 0xa), hulls 0x28 (slabs 0x20, 0x22)
    Maps::TriggerLayout TriggerShapes()
    {
        Maps::TriggerLayout T;
        T.ModelSize = 0x10, T.ModelHullCount = 8, T.ModelFirstHull = 0xa;
        T.HullSize = 0x28, T.HullContents = 0x18, T.HullSlabCount = 0x20, T.HullFirstSlab = 0x22;
        return T;
    }

    // "?N" values of an entity string moved by Offset (an addon's trigger numbers are its own)
    std::string ShiftTriggers(const std::string& Text, size_t Offset)
    {
        std::string Out;
        for (size_t K = 0; K < Text.size(); K++)
        {
            Out += Text[K];
            if (Text[K] != '"' || K + 2 >= Text.size() || Text[K + 1] != '?' || !std::isdigit((unsigned char)Text[K + 2])) continue;
            size_t End = K + 2;
            while (End < Text.size() && std::isdigit((unsigned char)Text[End])) End++;
            if (End >= Text.size() || Text[End] != '"') continue;
            Out += "?" + std::to_string(std::stoul(Text.substr(K + 2, End - K - 2)) + Offset);
            K = End - 1;
        }
        return Out;
    }

    bool WW2Zone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        IWZone::MapLayout M = Level();
        M.MoreUvs = BlendLayers();
        if (!ReadLevel(Scene, Models, M, GameWorldWar2::ReadXMaterial)) return false;
        // addon_map_ents (0x88): its entity string (8), MapTriggers (0x18, numbered from 0), a
        // ClipInfo pointer (0x48) and the brush models it owns (count 0x50, array 0x58, numbered
        // after the clip map's). Its triangles are in its models' space: counted, not placed.
        const int64_t Addon = Root("addon_map_ents");
        if (Addon < 0) return true;
        Maps::ClipLayout L = ClipBrushes(0x48);
        L.InfoPointer = true, L.ModelCount = 0x50, L.Models = 0x58, L.ModelNeedsInfo = true;
        L.VertexCount = L.TriangleCount = -1;
        Maps::Collision Own;
        std::map<std::string, size_t> Unread;
        const Maps::ClipSource Src = ClipSource();
        Maps::ReadCollision(Own, Src, Addon, L, Unread);
        for (const auto& [Key, N] : Unread) Counter("map_addon_" + Key) += N;
        Maps::ReadTriggers(Own, Src, Addon + 0x18, TriggerShapes());
        Maps::Collision& C = Scene.Collision;
        C.ClipBrushes += Own.ClipBrushes;
        for (auto& B : Own.Brushes) C.Brushes.push_back(std::move(B));
        if (Own.Models.size() > C.Models.size()) C.Models.resize(Own.Models.size());
        for (size_t M = 1; M < Own.Models.size(); M++)
        {
            if (Own.Models[M].empty()) continue;
            if (!C.Models[M].empty()) Counter("map_addon_models_taken")++;
            else C.Models[M] = std::move(Own.Models[M]);
        }
        for (auto& Mat : Own.Materials) C.Materials.push_back(std::move(Mat));
        const size_t First = C.Triggers.size();
        for (auto& T : Own.Triggers) C.Triggers.push_back(std::move(T));
        const std::string Ents = StringAt(PointerAt(Addon + 8));
        if (!Ents.empty()) Scene.MapEnts += "\n" + ShiftTriggers(Ents, First);
        const int64_t Info = Off(PointerAt(Addon + 0x48));
        const int64_t Trzone = Info >= 0 ? Off(PointerAt(Info + 0x228)) : -1;
        if (Trzone >= 0) Counter("map_addon_triangles_unplaced") += At<uint32_t>(Image(), Trzone + 0x48);
        return true;
    }

    std::unordered_map<int64_t, HeaderEntry> WW2Zone::SoundFiles()
    {
        // the zone's loaded sounds and primed or streamed file specs, in load (stream) order
        std::vector<int64_t> Order;
        for (const auto& I : Walk().Instances)
        {
            if (OwnerIs(I, "loaded_sound"))
            {
                Order.push_back(I.Pos);
                continue;
            }
            if (!OwnerIs(I, "sound")) continue;
            const int64_t Entries = Off(At<uint64_t>(Image(), I.Pos + AliasEntries));
            const uint32_t Count = At<uint8_t>(Image(), I.Pos + AliasEntryCount);
            for (uint32_t K = 0; Entries >= 0 && K < Count; K++)
            {
                const int64_t Spec = Off(At<uint64_t>(Image(), Entries + (int64_t)K * AliasEntrySize + EntrySpec));
                if (Spec < 0) continue;
                const uint8_t Type = At<uint8_t>(Image(), Spec);
                Counter(std::format("sound_specs_type{}", (unsigned)Type))++;
                if (Type == 2 || Type == 3) Order.push_back(Spec);
            }
        }
        std::sort(Order.begin(), Order.end());
        Order.erase(std::unique(Order.begin(), Order.end()), Order.end());
        std::unordered_map<int64_t, HeaderEntry> Out;
        const std::vector<HeaderEntry> Table = ReadHeaderTable(FastFilePath(), 1);
        if (Table.size() != Order.size())
        {
            if (!Order.empty()) Counter("sound_tables_mismatched")++;
            return Out;
        }
        for (size_t K = 0; K < Order.size(); K++) Out[Order[K]] = Table[K];
        return Out;
    }

    // snd_alias_t (0x150): strings at 0, 8, 0x10, 0x18, 0x20, 0x30 and the SoundFile at 0x28 (the s2
    // tables); the list's count a byte at 0x18. No public layout: the rest is measured on mp_airship's
    // 6617 variants (their values and ranges, in the order IW keeps them): volumes 0x48/0x4c (linear,
    // 0..2), pitches 0x54/0x58 (ratios 0.8..1.1), distances 0x5c/0x60 (10..1000, 350..3000), flags
    // 0x68 (bit 0 set on the 22 ambient beds only: looping)
    IWSoundAliases::Layout AliasLayout()
    {
        IWSoundAliases::Layout L;
        L.ListCount = 0x18, L.ListCountBytes = 1, L.Size = 0x150;
        L.Subtitle = 8, L.Secondary = 0x10, L.SoundFile = 0x28, L.VolMin = 0x48, L.VolMax = 0x4c, L.PitchMin = 0x54, L.PitchMax = 0x58;
        L.DistMin = 0x5c, L.DistMax = 0x60, L.Flags = 0x68;
        return L;
    }

    // How AddAliasSounds names a variant's file (GameWorldWar2.cpp), lower case (OnePerName): the
    // SoundFile {u8 type, u8 exists, then at 8} names a loaded sound by its LoadedSound's name (a
    // pointer at 8), a primed one by the name at 0x18, a streamed one by the name at 0x20
    std::string LiveFileName(const SoundAliasCsv::ZoneView& Z, int64_t File)
    {
        if (File < 0 || Z.At<uint8_t>(File + 1) == 0) return std::string();
        std::string S;
        switch (Z.At<uint8_t>(File))
        {
        case 1: {
            const int64_t Loaded = Z.Follow(File + 8);
            if (Loaded >= 0) S = FileSystems::GetFileName(Z.Text(Loaded));
            break;
        }
        case 2: S = Z.Text(File + 0x18); break;
        case 3: S = Z.Text(File + 0x20); break;
        default: break;
        }
        return Strings::ToLower(ExportNaming::SafeFileName(S));
    }

    void WW2Zone::ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        // every alias's file specs, through the live reader: loaded sounds are in the zone,
        // primed and streamed ones in soundfile<N>.pak (a language folder when localized)
        std::set<uint64_t> Unique;
        std::vector<CoDAsset_t*> Found;
        std::vector<uint64_t> Sources;
        std::vector<std::pair<int64_t, std::string>> Lists;
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "sound")) continue;
            const std::string AliasName = AssetName(DiskGame::Kind::Sound, I.Pos);
            if (IsReference(DiskGame::Kind::Sound, AliasName, I.Pos)) continue;
            const auto Alias = CoDAssets::GameInstance->Read<WWIISoundAlias>(Live().Addr((uint64_t)I.Pos));
            GameWorldWar2::AddAliasSounds(Alias, Unique, Found, &Sources);
            Lists.emplace_back(I.Pos, AliasName);
        }
        // where each sound's bytes are: the soundfile pak the header table names, else the zone
        const auto Files = SoundFiles();
        const auto& P = Packages();
        // a localized sound is in the zone's language folder (french/fra_paris.ff), else in the
        // install's first one
        const std::string Language = GameWorldWar2::LanguageOfZone(FastFilePath());
        static const std::unordered_map<uint64_t, SoundEntry> NoToc;
        const auto Localized = P.LocalizedSoundFiles.find(Language);
        const auto& LocalizedToc = Localized != P.LocalizedSoundFiles.end() ? Localized->second
            : P.LocalizedSoundFiles.empty() ? NoToc : P.LocalizedSoundFiles.begin()->second;
        for (size_t N = 0; N < Found.size(); N++)
        {
            const auto It = Files.find(Off(Sources[N]));
            if (It == Files.end() || It->second.Key == 0) continue;
            const auto& Toc = (It->second.Flags & 1) ? LocalizedToc : P.SoundFiles;
            const auto Entry = Toc.find(It->second.Key);
            if (Entry == Toc.end())
            {
                Counter("sounds_not_in_toc")++;
                continue;
            }
            auto* Sound = static_cast<CoDSound_t*>(Found[N]);
            Sound->IsFileEntry = true;
            Sound->IsLocalized = (It->second.Flags & 1) != 0;
            Sound->PackageIndex = Entry->second.Pak;
            Sound->AssetPointer = Entry->second.Offset;
            Sound->AssetSize = Entry->second.Size;
        }
        IWPakSounds::WriteOptions Options;
        Options.OnePerName = true;
        Options.Language = Localized != P.LocalizedSoundFiles.end() || P.LocalizedSoundFiles.empty() ? Language : P.LocalizedSoundFiles.begin()->first;
        IWPakSounds::Write(Found, Packages().Sounds.get(), C, T, Name(), [this](const std::string& N) -> size_t& { return Counter(N); }, Options);
        if (!SoundAliasCsv::Wanted(C)) return;
        const SoundAliasCsv::ZoneView Z = AliasView();
        const IWSoundAliases::Layout L = AliasLayout();
        SoundAliasCsv::Rows Rows;
        const std::string Extension = IWPakSounds::Extension();
        const auto File = [&](const std::string&, uint32_t, int64_t At) {
            const std::string S = LiveFileName(Z, At);
            return S.empty() ? S : S + Extension;
        };
        for (const auto& [Pos, AliasName] : Lists) IWSoundAliases::Read(Z, Pos, AliasName, L, File, Rows);
        SoundAliasCsv::Write(C, T, Name(), Rows);
    }

    class WW2Game final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "ww2"; }
        const char* Title() const override { return "Call of Duty: WWII"; }
        const char* Engine() const override { return "S2"; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<WW2Zone>(std::string(), std::string()); }
        // the rich presence strings, the language's own (fra_) after the install's: PRESENCE_MP_,
        // a DLC's or a store pack's (mp_wolfslair DLC1_ "Valkyrie", mp_house MTX5_), the campaign
        // by PRESENCE_SP_ (hill "Hill 493"); a map's version or variant suffix (mp_aachen_v2,
        // mp_carentan_s2, mp_wolfslair_free) is not in its key
        const LevelNaming* LevelNames() const override
        {
            static const LevelNaming Naming = [] {
                const auto Presence = [](const std::string& Map) {
                    std::vector<std::string> Keys;
                    for (const char* Pack : { "", "DLC1_", "DLC2_", "DLC3_", "DLC4_", "MTX1_", "MTX3_5_", "MTX5_", "MTX7_" })
                        Keys.push_back(std::string("PRESENCE_") + Pack + Map);
                    return Keys;
                };
                LevelNaming N;
                N.From = { "code_post_gfx", "code_post_gfx_mp", "code_post_gfx_zm_mp", "???_code_post_gfx", "???_code_post_gfx_mp",
                           "???_code_post_gfx_zm_mp" };
                N.Keys = { { "mp_*_s2_winter", Presence("MP_{REST}_WINTER") },
                           { "mp_*_v2", Presence("MP_{REST}") },
                           { "mp_*_s2", Presence("MP_{REST}") },
                           { "mp_*_free", Presence("MP_{REST}") },
                           { "mp_*_slim", Presence("MP_{REST}") },
                           { "mp_*", Presence("{NAME}") },
                           { "*", { "PRESENCE_SP_{NAME}" } } };
                N.Keys[5].second.push_back("MPUI_{REST}");
                return N;
            }();
            return &Naming;
        }

        // the anims' weapons, trees and animclasses (IwZoneAnimOwners.h)
        std::vector<std::string> AnimOwnerDocuments() const override { return IwZoneAnimOwners::Patterns(); }
        void AnimOwners(const DiskGame::ExportOptions& Options, const std::vector<AnimZone>& Zones, ::AnimOwners::Book& Out) const override
        {
            IwZoneAnimOwners::Fill(*this, AnimOwnerLayout(), Options, Zones, Out);
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions& Options, std::string& Error) const override
        {
            // the common zones' material book is kept under --cache, a temp folder by default
            const std::string Cache = !Options.Cache.empty() ? Options.Cache : (fs::temp_directory_path() / "greyhound_ww2").string();
            auto Zone = std::make_unique<WW2Zone>(Path, (fs::path(Cache) / "ww2_materials").string());
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const WW2Game Game;
    const DiskGame::Registration Registered(Game);
}
