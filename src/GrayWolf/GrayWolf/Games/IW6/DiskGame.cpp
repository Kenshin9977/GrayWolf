#include "stdafx.h"

// Ghosts (IW6) on the disk template: a ZonekitZone with the iw6 profile, served to the live Ghosts
// readers. What is Ghosts' own: the streamed-image table in the .ff header, the LOD surfaces the
// game fills in at load time, and sounds through the aliases.

#include "CoDAssets.h"
#include "DiskGame.h"
#include "ExportWriters.h"
#include "GameGhosts.h"
#include "GameReaders.h"
#include "GameWorldWar2.h"
#include "Games/IW/IWPakSounds.h"
#include "Games/IW/IWSoundAliases.h"
#include "Games/IW/IWZone.h"
#include "Games/IW/IwWeaponDefs.h"
#include "Games/IW6/EntityKeys.h"
#include "MapLive.h"
#include "PAKCache.h"

#include <filesystem>
#include <format>
#include <set>

namespace fs = std::filesystem;

namespace
{
    // XModel (0x258 on disk, the same layout as GhostsXModel): six 0x40 LOD records from 0x70, the
    // LOD count at 0x1f1, a LOD's surfaces at +0x30
    IWZone::LodLayout ModelLods()
    {
        IWZone::LodLayout L;
        L.Lods = 0x70, L.LodSize = 0x40, L.LodCount = 0x1f1, L.LodSurfs = 0x30;
        return L;
    }

    // GfxImage: format 0x18, streamed 0x35, name 0x60. A stream entry's pak index is its word >> 16;
    // index 0 is the .ff itself: "_exclude" zones append their pixels after the compressed zone.
    constexpr int64_t ImageFormatAt = 0x18, ImageStreamedAt = 0x35, ImageNameAt = 0x60;

    // snd_alias_t (200 bytes, zonetool's IW6 Structs.hpp): subtitle 8, secondary 0x10, SoundFile 0x28,
    // volumes 0x34/0x38, pitches 0x40/0x44, distances 0x48/0x4c, flags 0x54 (looping 0; spatialize, 6,
    // set on no variant of mp_prisonbreak: no 2D/3D),
    // probability 0x64, center 0x6c, start delay 0x70, envelope 0x90/0x94/0x98, allowDoppler 0xb8;
    // a primed SoundFile's LoadedSound at 0x20
    IWSoundAliases::Layout AliasLayout()
    {
        IWSoundAliases::Layout L;
        L.ListCount = 0x10, L.ListCountBytes = 4, L.Size = 0xc8;
        L.Subtitle = 8, L.Secondary = 0x10, L.SoundFile = 0x28, L.VolMin = 0x34, L.VolMax = 0x38, L.PitchMin = 0x40, L.PitchMax = 0x44;
        L.DistMin = 0x48, L.DistMax = 0x4c, L.Flags = 0x54, L.Probability = 0x64, L.CenterPercentage = 0x6c;
        L.StartDelay = 0x70, L.EnvelopMin = 0x90, L.EnvelopMax = 0x94, L.EnvelopPercentage = 0x98, L.AllowDoppler = 0xb8;
        L.PrimedLoaded = 0x20;
        return L;
    }

    // The weapon fields (IwWeaponDefs.h): WeaponCompleteDef (0x128, the weapon asset) and its WeaponDef
    // (0xa98, behind 8), at x64-zt's IW6 structs.hpp offsets (zonetool rebuilds these zones), checked
    // on common_core_mp's weapons (README). weapType, weapClass, fireType and the hit locations are
    // named by the exe's tables (iw6sp64: 0x140a0f5e0, 0x140a0f610, 0x1407e01e0).
    const IwWeaponDefs::Layout& WeaponLayout()
    {
        static const IwWeaponDefs::Layout L = [] {
            IwWeaponDefs::Layout L;
            L.Name = 0, L.Def = 8, L.Display = 0x10;
            L.Types = { "none", "bullet", "grenade", "projectile", "riotshield" };
            L.Classes = { "rifle", "sniper", "mg", "smg", "spread", "pistol", "grenade", "rocketlauncher", "turret", "throwingknife", "non-player", "item" };
            L.FireTypes = { "Full Auto", "Single Shot", "2-Round Burst", "3-Round Burst", "4-Round Burst", "Double Barrel" };
            constexpr int64_t Timers = 0x3f0;   // WeaponDef stateTimers (StateTimers, 0xcc)
            L.Fields = { { "adsTransInTime", 0x94, false }, { "adsTransOutTime", 0x98, false }, { "clipSize", 0x9c, false },
                         { "fireTime", 0xa4, false }, { "firstRaiseTime", 0xf0, false },
                         { "weaponType", 0x94 }, { "weaponClass", 0x98 }, { "fireType", 0xa4 }, { "startAmmo", 0x3a0 }, { "maxAmmo", 0x3c4 },
                         { "shotCount", 0x3c8 }, { "damage", 0x3e0 }, { "meleeDamage", 0x3e8 },
                         { "reloadTime", Timers + 0x30 }, { "reloadEmptyTime", Timers + 0x38 }, { "reloadAddTime", Timers + 0x3c },
                         { "reloadEmptyAddTime", Timers + 0x40 }, { "reloadStartTime", Timers + 0x44 }, { "reloadEndTime", Timers + 0x4c },
                         { "dropTime", Timers + 0x50 }, { "raiseTime", Timers + 0x54 }, { "quickRaiseTime", Timers + 0x60 },
                         { "sprintOutTime", Timers + 0x78 },
                         { "explosionRadius", 0x6d4 }, { "explosionRadiusMin", 0x6d8 }, { "explosionInnerDamage", 0x6dc },
                         { "explosionOuterDamage", 0x6e0 }, { "projectileSpeed", 0x6f0 },
                         { "minDamage", 0x908 }, { "maxDamageRange", 0x910 }, { "minDamageRange", 0x914 }, { "locationDamageMultipliers", 0x928 } };
            L.Notes = IwWeaponDefs::GameNotes("Ghosts", "french/fra_code_post_gfx_mp", "common_core_mp",
                                              "the WeaponCompleteDef (the weapon asset: ADS times, clipSize, fireTime,\n"
                                              "firstRaiseTime) and its WeaponDef (behind 8), at the offsets of x64-zt's IW6 structs.hpp",
                                              "Honey Badger 800 rpm, 30 rounds, 42-20 damage; AK-12 690 rpm, 48-25; MTS-255 8 pellets");
            L.Notes.Source += "The damage fields are read off the exe too (iw6sp64_dumped, the definitions' table 0x5a5fb20):\n"
                              "damage 0x3e0 (0x4c7daa) and minDamage 0x908 (0x4c9c8a), each overridden by an attachment's block\n"
                              "0x70, maxDamageRange 0x910 and minDamageRange 0x914 (0x4c7e82, scaled by the attachments' 0xe4, 0xe8).\n";
            return L;
        }();
        return L;
    }

    class GhostsZone final : public IWZone
    {
    public:
        explicit GhostsZone(const std::string& Path) : IWZone(Path)
        {
            using namespace DiskGame;
            ExportsIwWeapons(WeaponLayout());
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
            ExportsScriptFiles(Scripts::GscToolBin("iw6"));
            ExportsIwRawfiles();
            ExportsIwStringTables();
        }

    protected:
        const char* Profile() const override { return "iw6"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("IW6"); }
        VirtualGameSession::Handlers Handlers() override { return { SlotString, DiskImage }; }
        bool DerefAssetPointers() const override { return true; }
        void Fixups() override;
        // with the streamed images
        void Defines(std::vector<DiskGame::AssetId>& Out) const override
        {
            ZonekitZone::Defines(Out);
            DefineStreamed(Out, ImageNameAt);
        }
        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;

    private:
        static std::unique_ptr<XImageDDS> DiskImage(const XImage_t& Image);

        std::unique_ptr<XImageDDS> Decode(int64_t Pos);
        void ExportImages(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T);
    };

    std::unique_ptr<XImageDDS> GhostsZone::DiskImage(const XImage_t& Image)
    {
        GhostsZone* Current = ServedZone<GhostsZone>();
        if (Current == nullptr) return nullptr;
        const auto It = Current->Streams.find((int64_t)(Image.ImagePtr - VirtualGameSession::Base));
        if (It == Current->Streams.end()) return nullptr;
        const auto Info = CoDAssets::GameInstance->Read<GhostsGfxImage>(Image.ImagePtr);
        uint32_t Width = 0, Height = 0;
        const uint32_t Level = GameReaders::LargestLevel(Info, Width, Height);
        const PakEntry& E = It->second[Level];
        if (E.End <= E.Start) return nullptr;
        const std::string& Ff = Current->FastFilePath();
        const std::string Pak = (E.Pak >> 16) == 0 ? Ff
            : (fs::path(Ff).parent_path() / std::format("imagefile{}.pak", (unsigned long long)(E.Pak >> 16))).string();
        return GameGhosts::DecodePakImage(Info, Pak, E.Start, E.End, Width, Height, Image.ImageUsage);
    }

    void GhostsZone::Fixups()
    {
        // the script strings: count at 0x48, marker-tested slots from 0x70, then their text
        const auto& Z = Zone();
        const uint64_t Count = At<uint64_t>(Z, 0x48);
        if (At<uint64_t>(Z, 0x50) == Rules().Marker && Count != 0 && Count <= Z.size() / 8)
            Slots = InlineStrings(Z, 0x70, Count, 8, Rules().Marker);
        LinkLodSurfaces(ModelLods());
        ReadStreams(ImageStreamedAt);
    }

    std::unique_ptr<XImageDDS> GhostsZone::Decode(int64_t Pos)
    {
        const auto Usage = At<uint8_t>(Zone(), Pos + ImageFormatAt) == 84 ? ImageUsageType::NormalMap : ImageUsageType::DiffuseMap;
        return DiskImage(XImage_t(Usage, 0, VirtualGameSession::Address((uint64_t)Pos), AssetName(DiskGame::Kind::Image, Pos + ImageNameAt)));
    }

    void GhostsZone::ExportImages(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        ImageList L;
        L.Order = StreamOrder();
        L.NameAt = ImageNameAt;
        // what the output file is made of: the four pak entries (and for index 0, which .ff), the
        // format and the extension
        L.KeyOf = [this](int64_t Pos, const std::string& Extension) {
            ExportWriters::ContentKey Key;
            MixStreams(Pos, Key);
            if ((Streams.at(Pos)[0].Pak >> 16) == 0) Key.Mix(FastFilePath());
            return Key.Mix(At<uint8_t>(Zone(), Pos + ImageFormatAt)).Mix(Extension).Key();
        };
        L.Decode = [this](int64_t Pos, const std::string&) { return Decode(Pos); };
        ExportImageList(C, T, L);
    }

    void GhostsZone::ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        // every alias's file specs, through the live reader: loaded sounds are PCM in the zone,
        // streamed and primed ones FLAC in soundfile<N>.pak (a language folder when localized)
        std::set<uint64_t> Unique;
        std::vector<CoDAsset_t*> Found;
        std::vector<std::pair<int64_t, std::string>> Lists;
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "sound")) continue;
            const std::string AliasName = AssetName(DiskGame::Kind::Sound, I.Pos);
            if (IsReference(DiskGame::Kind::Sound, AliasName, I.Pos)) continue;
            const auto Alias = CoDAssets::GameInstance->Read<GhostsSoundAlias>(VirtualGameSession::Address((uint64_t)I.Pos));
            GameGhosts::AddAliasSounds(Alias, AliasName, Unique, Found);
            Lists.emplace_back(I.Pos, AliasName);
        }
        PAKCache Packages;
        Packages.LoadPackageCache(GameWorldWar2::InstallOfZone(FastFilePath()));
        IWPakSounds::WriteOptions Options;
        Options.Language = GameWorldWar2::LanguageOfZone(FastFilePath());
        IWPakSounds::Write(Found, &Packages, C, T, Name(), [this](const std::string& N) -> size_t& { return Counter(N); }, Options);

        if (!SoundAliasCsv::Wanted(C)) return;
        SoundAliasCsv::Rows Rows;
        IWSoundAliases::ReadLive(AliasView(), Lists, AliasLayout(), Rows);
        SoundAliasCsv::Write(C, T, Name(), Rows);
    }

    // The level (the iw6 tables): GfxWorld (gfx_map, 0xa30) with GfxWorldDraw at 0x88 and
    // GfxWorldDpvsStatic at 0x8a0 (static model instances 0xb0 each), 60-byte brush models;
    // ComWorld's primary lights 0x78 each; col_map_sp's dynamic entities 0x78 each, the destroyed
    // model at 56
    IWZone::MapLayout Level()
    {
        IWZone::MapLayout M;
        M.VertexCount = 0xf4, M.Vertices = 0xf8, M.IndexCount = 0x120, M.Indices = 0x128;
        M.BrushCount = 0x758, M.Brushes = 0x760, M.BrushSize = 0x3c, M.BrushFirstSurface = 52, M.BrushSurfaceCount = 56;
        M.SModelCount = 0x8a0, M.Surfaces = 0x968, M.SModels = 0x978, M.SModelSize = 0xb0;
        M.SurfaceTris = 14, M.SurfaceBaseIndex = 16;
        M.Clip = "col_map_sp", M.DynSize = 0x78, M.DynDestroyed = 56;
        M.Lights.Size = 0x78, M.Lights.Color = 4, M.Lights.Dir = 16, M.Lights.Origin = 40, M.Lights.Radius = 52, M.Lights.CosHalfFov = 56;
        M.ImageNameAt = ImageNameAt;
        M.EntityKeys = IW6Keys::EntityKeys;
        // clipMap_t's brushes (the iw6 tables' col_map_sp, MW3's 64-bit shapes): a ClipInfo at 0x10
        // (0x80: ClipMaterial 0x10 at 0x10/0x18, name, surface 8, contents 0xc; leaf brush nodes 0x40/0x48,
        // 0x18 each; brushes u16 0x60, records 0x68, Bounds 0x70, contents 0x78); cbrush_t 0x30 (u16
        // numsides, sides 8, axial material numbers 0x18); cbrushside_t 0x10 (plane, u16 material 8);
        // brush models 0x120/0x128, cmodel_t 0x50 (a ClipInfo pointer at 0x20, leafBrushNode at 0x4c);
        // triangles: vertices 0xc8/0xd0, u16 vertex numbers 0xd8/0xe0. MapEnts' MapTriggers at 0x18
        Maps::ClipLayout& C = M.ClipBrushes;
        C.InfoAt = 0x10;
        C.Info.MaterialCount = 0x10, C.Info.Materials = 0x18, C.Info.BrushCount = 0x60, C.Info.Brushes = 0x68;
        C.Info.BrushBounds = 0x70, C.Info.BrushContents = 0x78, C.Info.LeafBrushNodeCount = 0x40, C.Info.LeafBrushNodes = 0x48;
        C.MaterialSize = 0x10, C.MaterialName = 0, C.MaterialSurface = 8, C.MaterialContents = 0xc;
        C.BrushSize = 0x30, C.BrushSideCount = 0, C.BrushSideCountBytes = 2, C.BrushSides = 8, C.AxialMaterial = 0x18;
        C.SideSize = 0x10, C.SidePlane = 0, C.SideMaterial = 8, C.SideMaterialBytes = 2;
        C.ModelCount = 0x120, C.Models = 0x128, C.ModelSize = 0x50, C.ModelInfo = 0x20, C.ModelLeafBrushNode = 0x4c;
        C.NodeSize = 0x18;
        C.VertexCount = 0xc8, C.Vertices = 0xd0, C.TriangleCount = 0xd8, C.Triangles = 0xe0;
        M.Triggers = 0x18;
        return M;
    }

    bool GhostsZone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        return ReadLevel(Scene, Models, Level(), GameGhosts::ReadXMaterial);
    }

    class GhostsGame final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "ghosts"; }
        const char* Title() const override { return "Call of Duty: Ghosts"; }
        const char* Engine() const override { return "IW6"; }
        std::vector<std::string> Aliases() const override { return { "ghosts-disk-export" }; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<GhostsZone>(std::string()); }
        // the rich presence strings of the patch: PRESENCE_MP_PRISONBREAK, PRESENCE_SP_FLOOD; a
        // language's own patch (fra_) after the install's
        const LevelNaming* LevelNames() const override
        {
            static const LevelNaming Naming{ { "patch_code_post_gfx_mp", "???_patch_code_post_gfx_mp" },
                                             { { "*", { "PRESENCE_{NAME}", "PRESENCE_SP_{NAME}", "MPUI_{NAME}" } } } };
            return &Naming;
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions&, std::string& Error) const override
        {
            auto Zone = std::make_unique<GhostsZone>(Path);
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const GhostsGame Game;
    const DiskGame::Registration Registered(Game);
}
