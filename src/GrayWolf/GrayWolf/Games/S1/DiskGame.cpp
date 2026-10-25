#include "stdafx.h"

// Advanced Warfare (S1) on the disk template: a ZonekitZone with the s1 profile, served to the
// live Advanced Warfare readers (GameAdvancedWarfare.cpp). What is AW's own: the streamed-image
// table in the .ff header, the LOD surfaces the game fills in at load time, the materials the
// common zones define, sounds through the aliases (samples from soundfile<N>.pak) and the level.

#include "CoDAssets.h"
#include "DiskGame.h"
#include "ExportWriters.h"
#include "GameAdvancedWarfare.h"
#include "GameReaders.h"
#include "GameWorldWar2.h"
#include "Games/IW/IWMaterialBooks.h"
#include "Games/IW/IWSoundAliases.h"
#include "Games/IW/IWZone.h"
#include "Games/IW/IwEffects.h"
#include "Games/IW/IwWeaponDefs.h"
#include "Games/S1/EntityKeys.h"
#include "MapLive.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>

namespace fs = std::filesystem;

namespace
{
    // XModel (0x2a8, AWXModel): six 0x40 LOD records from 0x80, the LOD count at 0x200, a LOD's
    // surfaces at +0x30
    IWZone::LodLayout ModelLods()
    {
        IWZone::LodLayout L;
        L.Lods = 0x80, L.LodSize = 0x40, L.LodCount = 0x200, L.LodSurfs = 0x30;
        return L;
    }

    // A stream entry's pak index is its word >> 16; index 0 is the .ff itself.
    // GfxImage (0x68, AWGfxImage): DXGI format 0x18, map type 0x1c, streamed 0x35, name 0x60. A
    // resident image's pixels are in the zone: every level from the largest (size u32 0x24) at
    // the pointer at 0x38, its size u16 0x2c/0x2e and level count u8 0x34.
    constexpr int64_t ImageFormatAt = 0x18, ImageMapTypeAt = 0x1c, ImageStreamedAt = 0x35, ImageNameAt = 0x60;
    constexpr int64_t ImageDataSize = 0x24, ImageWidth = 0x2c, ImageHeight = 0x2e, ImageLevels = 0x34, ImagePixels = 0x38;

    // A name-only material header names a material of the common zones (common, common_mp,
    // code_pre_gfx, code_post_gfx and their patch_ zones), which the game loads first. Material
    // (0x1e0): image count u8 0xe4, image table at 0xf8.
    IWMaterialBooks::Layout MaterialLayout()
    {
        IWMaterialBooks::Layout L;
        L.Profile = "s1";
        L.ImageCount = 0xe4, L.Images = 0xf8, L.ImageName = ImageNameAt;
        return L;
    }
    constexpr const char* CommonZones = "(patch_)?(common|code_pre_gfx|code_post_gfx)(_[a-z0-9_]+)?[.]ff";

    // The weapon fields (IwWeaponDefs.h): the weapon asset is the WeaponDef itself (0xde8), at x64-zt's
    // S1 structs.hpp offsets (zonetool rebuilds these zones), checked on common_mp's weapons (README).
    // weapType, weapClass, fireType and the hit locations are named by the exe's tables (s1_mp64:
    // 0x1409b0930, 0x1409b0960, 0x1409b0a50, 0x1409b5400).
    const IwWeaponDefs::Layout& WeaponLayout()
    {
        static const IwWeaponDefs::Layout L = [] {
            IwWeaponDefs::Layout L;
            L.Name = 0, L.Display = 8;
            L.Types = { "none", "bullet", "grenade", "projectile", "riotshield", "energy" };
            L.Classes = { "rifle", "sniper", "mg", "smg", "spread", "pistol", "grenade", "rocketlauncher",
                          "turret", "throwingknife", "non-player", "item", "cone", "beam", "shield", "hover",
                          "cloak", "ping", "repulsor", "adrenaline", "health", "mute", "underwater", "ball" };
            L.FireTypes = { "Full Auto", "Single Shot", "2-Round Burst", "3-Round Burst", "4-Round Burst", "5-Round Burst", "Double Barrel" };
            constexpr int64_t Timers = 0x658;   // stateTimers (StateTimers, 0x12c)
            L.Fields = { { "weaponType", 0x52c }, { "weaponClass", 0x530 }, { "fireType", 0x544 }, { "startAmmo", 0x61c }, { "maxAmmo", 0x630 },
                         { "clipSize", 0x638 }, { "shotCount", 0x63c }, { "damage", 0x648 }, { "meleeDamage", 0x650 },
                         { "fireTime", Timers + 0x10 }, { "reloadTime", Timers + 0x34 }, { "reloadEmptyTime", Timers + 0x3c },
                         { "reloadAddTime", Timers + 0x40 }, { "reloadEmptyAddTime", Timers + 0x44 }, { "reloadStartTime", Timers + 0x48 },
                         { "reloadEndTime", Timers + 0x50 }, { "dropTime", Timers + 0x6c }, { "raiseTime", Timers + 0x70 },
                         { "quickRaiseTime", Timers + 0x80 }, { "firstRaiseTime", Timers + 0x84 }, { "sprintOutTime", Timers + 0x9c },
                         { "adsTransInTime", 0x988 }, { "adsTransOutTime", 0x990 },
                         { "explosionRadius", 0xa60 }, { "explosionRadiusMin", 0xa64 }, { "explosionInnerDamage", 0xa68 },
                         { "explosionOuterDamage", 0xa6c }, { "projectileSpeed", 0xa7c },
                         { "maxDamageRange", 0xc90 }, { "damage2", 0xc80 }, { "damageRange2", 0xc94 }, { "minDamage", 0xc84 }, { "locationDamageMultipliers", 0x460 } };
            L.Notes = IwWeaponDefs::GameNotes("Advanced Warfare", "french/fra_code_post_gfx_mp", "common_mp",
                                              "the WeaponDef (the weapon asset itself) at the offsets of x64-zt's S1 structs.hpp",
                                              "Bal-27 706 rpm, 32 rounds; AK-12 750 rpm, 30 rounds; KF5 870 rpm, 40 rounds; MORS bolt action");
            L.Notes.Source += "The damage is three steps, no fall-off (s1_mp64_flat 0x169ab0, through the getter 0x1649d0 and\n"
                              "0x164760 that take a field's offset): damage (0x648) up to maxDamageRange (0xc90), then damage_2\n"
                              "(0xc80, x64-zt's minDamage) up to range_2_m (0xc94, its minDamageRange), then the last one (0xc84,\n"
                              "x64-zt's midDamage) beyond: its range is empty.\n";
            return L;
        }();
        return L;
    }

    class AwZone final : public IWZone
    {
    public:
        AwZone(const std::string& Path, std::string CacheDir)
            : IWZone(Path), Books(MaterialLayout(), std::move(CacheDir), [Path] {
                  return IWMaterialBooks::ZonesMatching(IWMaterialBooks::InstallDir(Path, "common.ff"), CommonZones);
              })
        {
            using namespace DiskGame;
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
            ExportsScriptFiles(Scripts::GscToolBin("s1"));
            ExportsIwRawfiles();
            ExportsIwStringTables();
            ExportsIwWeapons(WeaponLayout());
            // the material and fx kinds: FxEffectDef 0x40, elements 0x140; Material's techniqueSet at
            // 0xf0 (the s1 tables f246690 and f24f5a0, x64-zt's S1 structs.hpp)
            ExportsIwEffects(IwEffects::AdvancedWarfare(), GameAdvancedWarfare::ReadXMaterial, 0xf0);
        }
        ~AwZone() override
        {
            if (IsServed()) GameAdvancedWarfare::ImagesElsewhere = nullptr;
        }

        // the material headers the common zones' book filled, and those it did not
        void Measures(DiskGame::Totals& T) const override
        {
            ZonekitZone::Measures(T);
            Books.Measure(T, "materials_from_common", "materials_in_no_common_zone");
        }

    protected:
        const char* Profile() const override { return "s1"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("S1"); }
        VirtualGameSession::Handlers Handlers() override { return { SlotString, DiskImage }; }
        bool DerefAssetPointers() const override { return true; }
        void Fixups() override;
        void Served(bool On) override
        {
            if (!On)
            {
                GameAdvancedWarfare::ImagesElsewhere = nullptr;
                return;
            }
            GameAdvancedWarfare::ImagesElsewhere = [this](const std::string& Name, IWMaterialBooks::ImageList& Out) { return Books.Find(Name, Out); };
        }
        // with the images
        void Defines(std::vector<DiskGame::AssetId>& Out) const override
        {
            ZonekitZone::Defines(Out);
            DefineStreamed(Out, ImageNameAt, Resident);
        }
        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;

    private:
        static std::unique_ptr<XImageDDS> DiskImage(const XImage_t& Image);

        std::unique_ptr<XImageDDS> Decode(int64_t Pos);
        std::unique_ptr<XImageDDS> DecodeResident(int64_t Pos, ImageUsageType Usage) const;
        int64_t ResidentPixels(int64_t Pos) const;   // where its pixels are in the zone, -1 when not there
        void ExportImages(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T);
        std::vector<int64_t> Resident;                                  // images whose pixels are in the zone
        IWMaterialBooks::Lookup Books;
    };

    std::unique_ptr<XImageDDS> AwZone::DiskImage(const XImage_t& Image)
    {
        AwZone* Current = ServedZone<AwZone>();
        if (Current == nullptr) return nullptr;
        const int64_t Pos = (int64_t)(Image.ImagePtr - VirtualGameSession::Base);
        const auto It = Current->Streams.find(Pos);
        if (It == Current->Streams.end()) return Current->DecodeResident(Pos, Image.ImageUsage);
        const auto Info = CoDAssets::GameInstance->Read<AWGfxImage>(Image.ImagePtr);
        uint32_t Width = 0, Height = 0;
        const uint32_t Level = GameReaders::LargestLevel(Info, Width, Height);
        const PakEntry& E = It->second[Level];
        if (E.End <= E.Start) return nullptr;
        return GameAdvancedWarfare::DecodePakImage(Info, Current->StreamPak(E), E.Start, E.End, Width, Height, Image.ImageUsage);
    }

    void AwZone::Fixups()
    {
        Served(true);   // opened: the zone the handlers reach
        Slots = HeaderScriptStrings();
        const auto& Z = Zone();
        LinkLodSurfaces(ModelLods());
        ReadStreams(ImageStreamedAt);
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, "image") && At<uint8_t>(Z, I.Pos + ImageStreamedAt) == 0 && ResidentPixels(I.Pos) >= 0) Resident.push_back(I.Pos);
        Counter("images_resident") += Resident.size();
    }

    int64_t AwZone::ResidentPixels(int64_t Pos) const
    {
        const int64_t Pixels = Off(PointerAt(Pos + ImagePixels));
        const uint32_t Size = At<uint32_t>(Zone(), Pos + ImageDataSize);
        return Pixels >= 0 && Size > 0 && Pixels + (int64_t)Size <= (int64_t)Zone().size() ? Pixels : -1;
    }

    std::unique_ptr<XImageDDS> AwZone::DecodeResident(int64_t Pos, ImageUsageType Usage) const
    {
        const int64_t Pixels = ResidentPixels(Pos);
        if (Pixels < 0) return nullptr;
        const auto& Z = Zone();
        const uint32_t Size = At<uint32_t>(Z, Pos + ImageDataSize);
        auto Data = std::make_unique<uint8_t[]>(Size);
        std::memcpy(Data.get(), Z.data() + Pixels, Size);
        return GameAdvancedWarfare::TranslateImage(std::move(Data), Size, At<uint8_t>(Z, Pos + ImageFormatAt), At<uint8_t>(Z, Pos + ImageMapTypeAt),
                                                   At<uint16_t>(Z, Pos + ImageWidth), At<uint16_t>(Z, Pos + ImageHeight), Usage,
                                                   std::max<uint8_t>(At<uint8_t>(Z, Pos + ImageLevels), 1));
    }

    std::unique_ptr<XImageDDS> AwZone::Decode(int64_t Pos)
    {
        const auto Usage = At<uint8_t>(Zone(), Pos + ImageFormatAt) == 84 ? ImageUsageType::NormalMap : ImageUsageType::DiffuseMap;
        return DiskImage(XImage_t(Usage, 0, VirtualGameSession::Address((uint64_t)Pos), AssetName(DiskGame::Kind::Image, Pos + ImageNameAt)));
    }

    void AwZone::ExportImages(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        ImageList L;
        // the resident ones, then the streamed in pak order
        L.Order = Resident;
        const std::vector<int64_t> Streamed = StreamOrder();
        L.Order.insert(L.Order.end(), Streamed.begin(), Streamed.end());
        L.NameAt = ImageNameAt;
        // what the output file is made of: the four pak entries (and for index 0, which .ff) or
        // the resident pixels and their shape, the format and the extension
        L.KeyOf = [this](int64_t Pos, const std::string& Extension) {
            ExportWriters::ContentKey Key;
            if (const auto It = Streams.find(Pos); It == Streams.end())
            {
                const auto& Z = Zone();
                const int64_t Pixels = ResidentPixels(Pos);
                const uint32_t Size = At<uint32_t>(Z, Pos + ImageDataSize);
                for (uint32_t K = 0; K + 8 <= Size; K += 8) Key.Mix(At<uint64_t>(Z, Pixels + K));
                Key.Mix(Size).Mix(At<uint32_t>(Z, Pos + ImageWidth)).Mix(At<uint8_t>(Z, Pos + ImageMapTypeAt));
                Key.Mix(At<uint8_t>(Z, Pos + ImageLevels));
            }
            else
            {
                MixStreams(Pos, Key);
                if ((It->second[0].Pak >> 16) == 0) Key.Mix(FastFilePath());
            }
            return Key.Mix(At<uint8_t>(Zone(), Pos + ImageFormatAt)).Mix(Extension).Key();
        };
        L.Decode = [this](int64_t Pos, const std::string&) { return Decode(Pos); };
        ExportImageList(C, T, L);
    }

    // snd_alias_t (0xf0, zonetool's S1 Structs.hpp): subtitle 8, secondary 0x10, SoundFile 0x20,
    // volumes 0x3c/0x40, pitches 0x48/0x4c, distances 0x50/0x54, flags 0x5c (looping 0, spatializedIs3D 7),
    // probability 0x70, center 0x88, start delay 0x8c, envelope 0x98/0x9c, allowDoppler 0xdc; the list's
    // count a byte at 0x18; a primed SoundFile's LoadedSound at 8
    IWSoundAliases::Layout AliasLayout()
    {
        IWSoundAliases::Layout L;
        L.ListCount = 0x18, L.ListCountBytes = 1, L.Size = 0xf0;
        L.Subtitle = 8, L.Secondary = 0x10, L.SoundFile = 0x20, L.VolMin = 0x3c, L.VolMax = 0x40, L.PitchMin = 0x48, L.PitchMax = 0x4c;
        L.DistMin = 0x50, L.DistMax = 0x54, L.Flags = 0x5c, L.Is3DBit = 7, L.Probability = 0x70, L.CenterPercentage = 0x88;
        L.StartDelay = 0x8c, L.EnvelopMin = 0x98, L.EnvelopMax = 0x9c, L.AllowDoppler = 0xdc;
        L.PrimedLoaded = 8;
        return L;
    }

    void AwZone::ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        // FLAC or PCM in soundfile<N>.pak, a loaded sound's in the zone when no package has it
        ExportPakSounds(C, T, GameWorldWar2::InstallOfZone(FastFilePath()), false, AliasLayout(),
                        [](uint64_t Address, const std::string& Name, std::set<uint64_t>& Unique, std::vector<CoDAsset_t*>& Found) {
                            GameAdvancedWarfare::AddAliasSounds(CoDAssets::GameInstance->Read<AWSoundAlias>(Address), Name, Unique, Found, true);
                        });
    }

    // The level (the s1 tables): GfxWorld (gfx_map, 0xaf0) with GfxWorldDraw at 0xb0 and
    // GfxWorldDpvsStatic at 0x710 (surfaces at +0x290, static models 0x50 each at +0x2a0), 96-byte
    // brush models; ComWorld's primary lights 0x90 each; col_map_mp's dynamic entities 0x88 each
    IWZone::MapLayout Level()
    {
        IWZone::MapLayout M;
        M.VertexCount = 0x130, M.Vertices = 0x138, M.IndexCount = 0x160, M.Indices = 0x168;
        M.BrushCount = 0x5b0, M.Brushes = 0x5b8, M.BrushSize = 96, M.BrushFirstSurface = 84, M.BrushSurfaceCount = 88;
        M.SModelCount = 0x710, M.Surfaces = 0x710 + 0x290, M.SModels = 0x710 + 0x2a0, M.SModelSize = 0x50;
        M.SurfaceTris = 14, M.SurfaceBaseIndex = 16;
        M.Clip = "col_map_mp", M.DynSize = 0x88;
        M.Lights.Size = 0x90, M.Lights.Color = 8, M.Lights.Dir = 20, M.Lights.Origin = 44, M.Lights.Radius = 80, M.Lights.CosHalfFov = 84;
        M.ImageNameAt = ImageNameAt;
        M.EntityKeys = S1Keys::EntityKeys;
        M.ClipBrushes = IWZone::AwClipBrushes(), M.Triggers = 0x18;   // MapEnts' MapTriggers at 0x18
        return M;
    }

    bool AwZone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        return ReadLevel(Scene, Models, Level(), GameAdvancedWarfare::ReadXMaterial);
    }

    class AwGame final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "aw"; }
        const char* Title() const override { return "Call of Duty: Advanced Warfare"; }
        const char* Engine() const override { return "S1"; }
        std::vector<std::string> Aliases() const override { return {}; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<AwZone>(std::string(), std::string()); }
        // the patch's strings, the language's own (fra_patch_) after the install's: multiplayer
        // and Exo Zombies maps by MPUI_ (mp_seoul2 "Swarm", mp_zombie_ark "Carrier"), the campaign
        // by its rich presence PRESENCE_SP_ (irons_estate "Sentinel")
        const LevelNaming* LevelNames() const override
        {
            static const LevelNaming Naming{ { "patch_code_post_gfx", "patch_code_post_gfx_mp", "???_patch_code_post_gfx",
                                               "???_patch_code_post_gfx_mp" },
                                             { { "mp_*", { "MPUI_{REST}", "PRESENCE_{NAME}" } },
                                               { "*", { "PRESENCE_SP_{NAME}", "LUA_MENU_MAPNAME_{NAME}" } } } };
            return &Naming;
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions& Options, std::string& Error) const override
        {
            const std::string Cache = !Options.Cache.empty() ? Options.Cache : (fs::temp_directory_path() / "greyhound_aw").string();
            auto Zone = std::make_unique<AwZone>(Path, (fs::path(Cache) / "aw_materials").string());
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const AwGame Game;
    const DiskGame::Registration Registered(Game);
}
