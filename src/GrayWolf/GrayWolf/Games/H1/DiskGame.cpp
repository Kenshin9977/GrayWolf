#include "stdafx.h"

// Modern Warfare Remastered (H1) on the disk template: a ZonekitZone with the h1 profile, served to
// the live MWR readers (GameModernWarfareRM.cpp). What is MWR's own: the streamed-image table in the
// .ff header, the LOD surfaces the game fills in at load time, the pixels of loaded images inside
// the zone, and sounds through the aliases.

#include "CoDAssets.h"
#include "DiskGame.h"
#include "ExportWriters.h"
#include "GameModernWarfareRM.h"
#include "GameReaders.h"
#include "Games/IW/IWMaterialBooks.h"
#include "Games/IW/IWSoundAliases.h"
#include "Games/IW/IWZone.h"
#include "Games/IW/IwEffects.h"
#include "Games/IW/IwWeaponDefs.h"
#include "Games/H1/EntityKeys.h"
#include "MapLive.h"
#include "PAKCache.h"
#include "PAKSupport.h"

#include <filesystem>
#include <format>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace
{
    // XModel (0x2a8, MWRXModel): the bone count at 8, six 0x40 LOD records from 0x80, the LOD count
    // at 0x200, a LOD's surfaces at +0x30
    constexpr int64_t ModelBoneCountAt = 8, LodCountAt = 0x200;
    IWZone::LodLayout ModelLods()
    {
        IWZone::LodLayout L;
        L.Lods = 0x80, L.LodSize = 0x40, L.LodCount = LodCountAt, L.LodSurfs = 0x30;
        return L;
    }

    // GfxImage (0x68): format at 0x18, streamed flag at 0x35, name at 0x60. A loaded image keeps
    // its pixels in the zone (0x38), their size at 0x24 and its size at 0x2c/0x2e; a streamed one
    // its levels' sizes at 0x40 (MWRGfxImage) and its pixels in imagefile<N>.pak.
    constexpr int64_t ImageFormatAt = 0x18, ImageDataSizeAt = 0x24, ImageWidthAt = 0x2c, ImageHeightAt = 0x2e;
    constexpr int64_t ImageStreamedAt = 0x35, ImageDataAt = 0x38, ImageNameAt = 0x60;

    // The install folder: a localized zone (french/fra_*.ff) reads the root's image paks, and the
    // sound reader finds its language folder from there.
    fs::path InstallRoot(const std::string& FastFilePath)
    {
        return IWMaterialBooks::InstallDir(FastFilePath, "imagefile1.pak");
    }

    // Material (0x250): name at 0, image count at 0x120, its slots at 0x130. A level's models and
    // the cosmetic zones name their materials by header only (",name", no slot): the material is
    // defined in the level's techsets_<level> zone or a common one.
    IWMaterialBooks::Layout MaterialLayout()
    {
        IWMaterialBooks::Layout L;
        L.Profile = "h1";
        L.ImageCount = 0x120, L.Images = 0x130, L.ImageName = ImageNameAt;
        L.WithSlotsOnly = true;
        return L;
    }

    // Where a zone's material headers are defined, in the order the game loads them: the level's
    // techsets zone, then the mode's common zones (MP: *_mp).
    std::vector<fs::path> BookZones(const std::string& FastFilePath)
    {
        const fs::path Dir = fs::path(FastFilePath).parent_path();
        const std::string Zone = fs::path(FastFilePath).stem().string();
        const bool Mp = Zone.rfind("mp_", 0) == 0 || Zone.find("_mp") != std::string::npos;
        std::vector<std::string> Names = { "techsets_" + Zone };
        // a level's patch zone (patch_<level>) takes its materials from the level's
        if (Zone.rfind("patch_", 0) == 0) Names.insert(Names.end(), { "techsets_" + Zone.substr(6), Zone.substr(6) });
        if (Mp) Names.insert(Names.end(), { "techsets_common_mp", "patch_common_mp", "common_mp", "techsets_common_core_mp", "common_core_mp" });
        else Names.insert(Names.end(), { "techsets_common", "patch_common", "common" });
        std::vector<fs::path> Out;
        std::error_code Ec;
        for (const auto& N : Names)
            if (N != Zone && fs::exists(Dir / (N + ".ff"), Ec)) Out.push_back(Dir / (N + ".ff"));
        return Out;
    }

    // The weapon fields (IwWeaponDefs.h): the weapon asset is the WeaponDef itself (0xe20), at x64-zt's
    // H1 structs.hpp offsets (zonetool rebuilds these zones), checked on common_mp's weapons (README).
    // weapType, weapClass, fireType and the hit locations are named by the exe's tables (h1_mp64_flat
    // RVAs: 0x10b2460 entries 0, 6 and 37; 0x10b5370).
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
            constexpr int64_t Timers = 0x668;   // stateTimers (StateTimers, 0x12c)
            L.Fields = { { "weaponType", 0x53c }, { "weaponClass", 0x540 }, { "fireType", 0x554 }, { "startAmmo", 0x62c }, { "maxAmmo", 0x640 },
                         { "clipSize", 0x648 }, { "shotCount", 0x64c }, { "damage", 0x658 }, { "meleeDamage", 0x660 },
                         { "fireTime", Timers + 0x10 }, { "reloadTime", Timers + 0x34 }, { "reloadEmptyTime", Timers + 0x3c },
                         { "reloadAddTime", Timers + 0x40 }, { "reloadEmptyAddTime", Timers + 0x44 }, { "reloadStartTime", Timers + 0x48 },
                         { "reloadEndTime", Timers + 0x50 }, { "dropTime", Timers + 0x6c }, { "raiseTime", Timers + 0x70 },
                         { "quickRaiseTime", Timers + 0x80 }, { "firstRaiseTime", Timers + 0x84 }, { "sprintOutTime", Timers + 0x9c },
                         { "adsTransInTime", 0x9a0 }, { "adsTransOutTime", 0x9a8 },
                         { "explosionRadius", 0xa7c }, { "explosionRadiusMin", 0xa80 }, { "explosionInnerDamage", 0xa84 },
                         { "explosionOuterDamage", 0xa88 }, { "projectileSpeed", 0xa98 },
                         { "minDamage", 0xca4 }, { "maxDamageRange", 0xcb0 }, { "minDamageRange", 0xcb4 }, { "locationDamageMultipliers", 0x470 } };
            L.Notes = IwWeaponDefs::GameNotes("MW Remastered", "french/fra_code_post_gfx_mp", "common_mp",
                                              "the WeaponDef (the weapon asset itself) at the offsets of x64-zt's H1 structs.hpp",
                                              "M4 857 rpm, 30 rounds, 30-20 damage; AK-47 706 rpm, 40-30; MP5 800 rpm; M1014 8 pellets");
            L.Notes.Source += "The damage falls linearly from damage (0x658) at maxDamageRange (0xcb0) to minDamage at\n"
                              "minDamageRange (0xcb4) (h1_mp64_flat 0x2e5c50, through the getters 0x2e19c0 and 0x2e17d0 that take\n"
                              "a field's offset); that minDamage is the i32 at 0xca4, the one x64-zt calls midDamage (its 0xca0 is\n"
                              "read but not used there).\n";
            return L;
        }();
        return L;
    }

    class MwrZone final : public IWZone
    {
    public:
        MwrZone(const std::string& Path, std::string BookDir)
            : IWZone(Path), Books(MaterialLayout(), std::move(BookDir), [Path] { return BookZones(Path); })
        {
            using namespace DiskGame;
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
            ExportsScriptFiles(Scripts::GscToolBin("h1"));
            ExportsIwRawfiles();
            ExportsIwStringTables();
            ExportsIwWeapons(WeaponLayout());
            // the material and fx kinds: FxEffectDef 0x40, elements 0x140; Material's techniqueSet at
            // 0x128 (the h1 tables f36c7d0 and f375ba0, x64-zt's H1 structs.hpp)
            ExportsIwEffects(IwEffects::ModernWarfareRemastered(), GameModernWarfareRM::ReadXMaterial, 0x128);
        }
        ~MwrZone() override
        {
            if (IsServed()) GameModernWarfareRM::ImagesElsewhere = nullptr;
        }

        // the material headers the books filled, and those they did not
        void Measures(DiskGame::Totals& T) const override
        {
            ZonekitZone::Measures(T);
            Books.Measure(T, "materials_from_books", "materials_in_no_book");
        }

    protected:
        const char* Profile() const override { return "h1"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("H1"); }
        VirtualGameSession::Handlers Handlers() override { return { SlotString, DiskImage }; }
        bool DerefAssetPointers() const override { return true; }
        void Fixups() override;
        void Served(bool On) override;
        // with the images
        void Defines(std::vector<DiskGame::AssetId>& Out) const override
        {
            ZonekitZone::Defines(Out);
            DefineStreamed(Out, ImageNameAt, Loaded);
        }
        // a stream entry's word: {u16 locale, u16 pak index, u32 checksum}
        uint64_t PakOf(const PakEntry& E) const override { return (E.Pak >> 16) & 0xffff; }
        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;
        // what Defines lists beyond the profile's walk: images, names by reference, model headers
        std::string IndexKey() const override { return ZonekitZone::IndexKey() + "_images_names1"; }
        // a model with no LOD and no bone, named in place (a weapon's view model): another zone
        // defines it
        bool IsReference(DiskGame::Kind K, const std::string& Name, int64_t Pos) const override
        {
            if (ZonekitZone::IsReference(K, Name, Pos)) return true;
            return K == DiskGame::Kind::Model && At<uint8_t>(Zone(), Pos + LodCountAt) == 0 && At<uint8_t>(Zone(), Pos + ModelBoneCountAt) == 0;
        }

    private:
        static std::unique_ptr<XImageDDS> DiskImage(const XImage_t& Image);

        std::unique_ptr<XImageDDS> Decode(int64_t Pos);
        std::unique_ptr<XImageDDS> DecodeAt(int64_t Pos, ImageUsageType Usage);
        bool HasPixels(int64_t Pos) const;
        void ExportImages(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T);
        std::vector<int64_t> Loaded;   // images whose pixels are in the zone
        IWMaterialBooks::Lookup Books;
    };

    std::unique_ptr<XImageDDS> MwrZone::DiskImage(const XImage_t& Image)
    {
        MwrZone* Current = ServedZone<MwrZone>();
        if (Current == nullptr) return nullptr;
        return Current->DecodeAt((int64_t)(Image.ImagePtr - VirtualGameSession::Base), Image.ImageUsage);
    }

    bool MwrZone::HasPixels(int64_t Pos) const
    {
        return Off(PointerAt(Pos + ImageDataAt)) >= 0 && At<uint32_t>(Image(), Pos + ImageDataSizeAt) != 0
            && At<uint16_t>(Image(), Pos + ImageWidthAt) != 0 && At<uint16_t>(Image(), Pos + ImageHeightAt) != 0;
    }

    std::unique_ptr<XImageDDS> MwrZone::DecodeAt(int64_t Pos, ImageUsageType Usage)
    {
        if (Pos < 0 || Pos + ImageNameAt + 8 > (int64_t)Image().size()) return nullptr;
        const auto Info = CoDAssets::GameInstance->Read<MWRGfxImage>(VirtualGameSession::Address((uint64_t)Pos));
        if (const auto It = Streams.find(Pos); It != Streams.end())
        {
            uint32_t Width = 0, Height = 0;
            const PakEntry& E = It->second[GameReaders::LargestLevel(Info, Width, Height)];
            if (E.End <= E.Start) return nullptr;
            const std::string Pak = (InstallRoot(FastFilePath()) / std::format("imagefile{}.pak", (unsigned)PakOf(E))).string();
            uint32_t Size = 0;
            auto Pixels = PAKSupport::AWExtractImagePackage(Pak, E.Start, E.End - E.Start, Size);
            return GameReaders::TranslatePakImage(std::move(Pixels), Size, Info.ImageFormat, Info.MapType, Width, Height, Usage);
        }
        if (!HasPixels(Pos)) return nullptr;
        const int64_t Data = Off(PointerAt(Pos + ImageDataAt));
        const uint32_t Size = At<uint32_t>(Image(), Pos + ImageDataSizeAt);
        if (Data + (int64_t)Size > (int64_t)Image().size()) return nullptr;
        auto Pixels = std::make_unique<uint8_t[]>(Size);
        std::memcpy(Pixels.get(), Image().data() + Data, Size);
        return GameReaders::TranslatePakImage(std::move(Pixels), Size, Info.ImageFormat, Info.MapType, At<uint16_t>(Image(), Pos + ImageWidthAt),
                                                 At<uint16_t>(Image(), Pos + ImageHeightAt), Usage);
    }

    void MwrZone::Served(bool On)
    {
        if (!On)
        {
            GameModernWarfareRM::ImagesElsewhere = nullptr;
            return;
        }
        GameModernWarfareRM::ImagesElsewhere = [this](const std::string& Header, std::vector<std::pair<uint32_t, std::string>>& Out) {
            return Books.Find(!Header.empty() && Header[0] == ',' ? Header.substr(1) : Header, Out);
        };
    }

    void MwrZone::Fixups()
    {
        Served(true);   // opened: the zone the handlers reach
        Slots = HeaderScriptStrings();
        const auto& Z = Zone();
        LinkLodSurfaces(ModelLods());
        ReadStreams(ImageStreamedAt);
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, "image") && At<uint8_t>(Z, I.Pos + ImageStreamedAt) == 0 && HasPixels(I.Pos)) Loaded.push_back(I.Pos);
        Counter("images_loaded") += Loaded.size();
    }

    std::unique_ptr<XImageDDS> MwrZone::Decode(int64_t Pos)
    {
        const auto Usage = At<uint8_t>(Zone(), Pos + ImageFormatAt) == 84 ? ImageUsageType::NormalMap : ImageUsageType::DiffuseMap;
        return DecodeAt(Pos, Usage);
    }

    void MwrZone::ExportImages(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        ImageList L;
        // the streamed ones in pak order, then the loaded ones in zone order
        L.Order = StreamOrder();
        L.Order.insert(L.Order.end(), Loaded.begin(), Loaded.end());
        L.NameAt = ImageNameAt;
        // what the output file is made of: the four pak entries (or the zone's pixels), the format
        // and the extension
        L.KeyOf = [this](int64_t Pos, const std::string& Extension) {
            ExportWriters::ContentKey Key;
            if (Streams.count(Pos))
                MixStreams(Pos, Key);
            else
            {
                const int64_t Data = Off(PointerAt(Pos + ImageDataAt));
                const uint32_t Size = At<uint32_t>(Image(), Pos + ImageDataSizeAt);
                for (uint32_t K = 0; Data >= 0 && K < Size && Data + K < (int64_t)Image().size(); K++) Key.Mix(Image()[(size_t)(Data + K)]);
                Key.Mix(At<uint32_t>(Image(), Pos + ImageWidthAt));
            }
            return Key.Mix(At<uint8_t>(Zone(), Pos + ImageFormatAt)).Mix(Extension).Key();
        };
        L.Decode = [this](int64_t Pos, const std::string&) { return Decode(Pos); };
        ExportImageList(C, T, L);
    }

    // snd_alias_t (0xf8, zonetool's H1 Structs.hpp): subtitle 8, secondary 0x10, SoundFile 0x20,
    // volumes 0x44/0x48, pitches 0x50/0x54, distances 0x58/0x5c, flags 0x64 (looping 0, spatializedIs3D 7),
    // probability 0x78, center 0x90, start delay 0x94, envelope 0xa0/0xa4, allowDoppler 0xe4; the list's
    // count a byte at 0x18; a primed SoundFile's LoadedSound at 8
    IWSoundAliases::Layout AliasLayout()
    {
        IWSoundAliases::Layout L;
        L.ListCount = 0x18, L.ListCountBytes = 1, L.Size = 0xf8;
        L.Subtitle = 8, L.Secondary = 0x10, L.SoundFile = 0x20, L.VolMin = 0x44, L.VolMax = 0x48, L.PitchMin = 0x50, L.PitchMax = 0x54;
        L.DistMin = 0x58, L.DistMax = 0x5c, L.Flags = 0x64, L.Is3DBit = 7, L.Probability = 0x78, L.CenterPercentage = 0x90;
        L.StartDelay = 0x94, L.EnvelopMin = 0xa0, L.EnvelopMax = 0xa4, L.AllowDoppler = 0xe4;
        L.PrimedLoaded = 8;
        return L;
    }

    void MwrZone::ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        // loaded sounds are in the zone, streamed and primed ones FLAC in soundfile<N>.pak
        // (french/fra_soundfile<N>.pak when localized); an empty file is a failure
        ExportPakSounds(C, T, InstallRoot(FastFilePath()).string(), true, AliasLayout(),
                        [](uint64_t Address, const std::string& Name, std::set<uint64_t>& Unique, std::vector<CoDAsset_t*>& Found) {
                            GameModernWarfareRM::AddAliasSounds(CoDAssets::GameInstance->Read<MWRSoundAlias>(Address), Name, Unique, Found);
                        });
    }

    // The level (the h1 tables): GfxWorld (gfx_map, 0xb30) with GfxWorldDraw at 0xb0 and
    // GfxWorldDpvsStatic at 0x748 (surfaces at +0x298, static models 0x50 each at +0x2a8), 96-byte
    // brush models, surfaces whose indices tile the index array (triangle count 18, first index
    // 20); ComWorld's primary lights 0x90 each; col_map_mp's dynamic entities 0x88 each
    IWZone::MapLayout Level()
    {
        IWZone::MapLayout M;
        M.VertexCount = 0x130, M.Vertices = 0x138, M.IndexCount = 0x170, M.Indices = 0x178;
        M.BrushCount = 0x5e8, M.Brushes = 0x5f0, M.BrushSize = 96, M.BrushFirstSurface = 84, M.BrushSurfaceCount = 88;
        M.SModelCount = 0x748, M.Surfaces = 0x9e0, M.SModels = 0x9f0, M.SModelSize = 0x50;
        M.SurfaceTris = 18, M.SurfaceBaseIndex = 20;
        M.Clip = "col_map_mp", M.DynSize = 0x88;
        M.Lights.Size = 0x90, M.Lights.Color = 8, M.Lights.Dir = 20, M.Lights.Origin = 44, M.Lights.Radius = 80, M.Lights.CosHalfFov = 84;
        M.ImageNameAt = ImageNameAt;
        M.EntityKeys = H1Keys::EntityKeys;
        M.ClipBrushes = IWZone::AwClipBrushes(), M.Triggers = 0x18;   // Advanced Warfare's shapes; MapEnts' MapTriggers at 0x18
        return M;
    }

    bool MwrZone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        return ReadLevel(Scene, Models, Level(), GameModernWarfareRM::ReadXMaterial);
    }

    class MwrGame final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "mwr"; }
        const char* Title() const override { return "Call of Duty: Modern Warfare Remastered"; }
        const char* Engine() const override { return "H1"; }
        std::vector<std::string> Aliases() const override { return { "h1" }; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<MwrZone>(std::string(), std::string()); }
        // the patch's strings, the language's own (fra_patch_) after the install's: multiplayer
        // maps by MPUI_ (mp_crash_snow "Winter Crash"), the campaign by its rich presence
        // PRESENCE_SP_ (ac130 "Death from Above")
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
            const fs::path Cache = Options.Cache.empty() ? fs::temp_directory_path() / "greyhound_mwr" : fs::path(Options.Cache);
            auto Zone = std::make_unique<MwrZone>(Path, (Cache / "material_books").string());
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const MwrGame Game;
    const DiskGame::Registration Registered(Game);
}
