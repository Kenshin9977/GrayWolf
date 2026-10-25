#include "stdafx.h"

#include "ExportWriters.h"

// World at War (T4) on the disk template: an IW3-family zone (Games/IW3/IwdZone.h) with the t4
// profile (32-bit, CoD4's layout with 7 blocks), served to the live World at War readers
// (GameWorldAtWar.cpp's ReadXModel / ReadXAnim / ReadXMaterial). Without OpenAssetTools. What is
// World at War's own: its XFile is 0x24 bytes, its aliases 0xb8 with the SoundFile at +0x14, a
// streamed file's folder and name at +8 and +0xc of it; the sounds are .wav files of the .iwd
// archives' sound folder, or whole .wav files a zone holds (loaded sounds).

#include "DiskGame.h"
#include "GameWorldAtWar.h"
#include "Games/IW3/IwdZone.h"
#include "Games/Treyarch/TreyarchEffects.h"
#include "Games/OAT/OatWeapons.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace
{
    using DiskGame::Context;
    using DiskGame::ExportOptions;
    using DiskGame::Kind;
    using DiskGame::Totals;

    // LoadedSound (0xc): name at 0, its data at +4 (read in place when non-null), its size at +8
    constexpr int64_t LoadedSize = 8;

    class WawZone final : public Iw3Family::IwdZone
    {
    public:
        WawZone(const std::string& Path, std::string InstallDir) : IwdZone(Path, std::move(InstallDir)), Weapons(Path)
        {
            // the weapon files and their stats table, through OpenAssetTools (Games/OAT/OatWeapons.h)
            Exports(Kind::Weapons, [this](const Context& C, Totals& T) { OatWeapons::ExportWeapons(Weapons, Name(), C, T); });
            Exports(Kind::WeaponStats, [this](const Context& C, Totals& T) { OatWeapons::ExportWeaponStats(Weapons, Name(), C, T, "Call of Duty: World at War"); });
            // Material: its techset, images and blend factors as JSON, its images by the image kind;
            // FxEffectDef (0x24, elements 0x100) as JSON, CoD4's with a wind influence
            ExportsEffects(GameWorldAtWar::ReadXMaterial, Treyarch::EffectLayout::T4);
        }

    protected:
        const char* Profile() const override { return "t4"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("T4"); }
        int64_t XFileSize() const override { return 0x24; }
        LoadXImageHandler ImageHandler() const override { return GameWorldAtWar::LoadXImage; }
        // snd_alias_t (0xb8): its SoundFile at +0x14; SoundFile (0x14): type u8 at 0, streamed:
        // StreamFileName {hash +4, folder +8, name +0xc}, a primed start at +0x10
        SoundLayout Sounds() const override { return { 0xb8, 0x14, 8, 0xc, LoadedSize }; }
        // snd_alias_t (0xb8, OAT's T4_Assets.h with codresearch.dev's names for its gaps): subtitle 8,
        // secondary 0xc, SoundFile 0x14, volumes 0x1c/0x20, pitches 0x24/0x28, distances 0x2c/0x30, the
        // reverb's 0x34, probability 0x3c, center 0x44, envelope 0x48/0x4c/0x50, priorities 0x54/0x58,
        // their thresholds 0x5c/0x60, reverb send 0x64, occlusion 0x70, start delay 0x7c, flags 0x84
        // (looping 0), limit type 0x9c, entity limit count 0xa0, randomize type 0xa8
        IWSoundAliases::Layout AliasLayout() const override
        {
            IWSoundAliases::Layout L;
            L.ListCount = 8, L.ListCountBytes = 4, L.Size = 0xb8;
            L.Subtitle = 8, L.Secondary = 0xc, L.SoundFile = 0x14, L.VolMin = 0x1c, L.VolMax = 0x20, L.PitchMin = 0x24, L.PitchMax = 0x28;
            L.DistMin = 0x2c, L.DistMax = 0x30, L.DistReverbMax = 0x34, L.Probability = 0x3c, L.CenterPercentage = 0x44;
            L.EnvelopMin = 0x48, L.EnvelopMax = 0x4c, L.EnvelopPercentage = 0x50, L.MinPriority = 0x54, L.MaxPriority = 0x58;
            L.MinPriorityThreshold = 0x5c, L.MaxPriorityThreshold = 0x60, L.ReverbSend = 0x64, L.OcclusionLevel = 0x70;
            L.StartDelay = 0x7c, L.Flags = 0x84, L.LimitType = 0x9c, L.EntityLimitCount = 0xa0, L.RandomizeType = 0xa8;
            return L;
        }
        // ExportLoadedSounds: the whole file under its folder, named for the sound format
        std::string LoadedSoundFile(const std::string& Name) const override
        {
            const std::string File = ExportWriters::SoundFileName(fs::path(Name).filename().string(), ExportWriters::SoundFormatSetting());
            return (fs::path(Name).parent_path() / ExportNaming::SafeFileName(File)).generic_string();
        }
        // The level (the t4 tables): GfxWorld (0x320) indices 0x10/0x14, surface count 0x18, sky
        // image 0x28, sky model name 0x30, vertices 0x34/0x38 (44 bytes), brush models
        // 0x168/0x16c (0x38: u32 surface count at 48, first surface at 52), GfxWorldDpvsStatic at
        // 0x25c (surfaces 0x2a8, 0x34 each, material at 0x14; static models 0x2b0, 0x5c each);
        // clipMap_t (0x14c) dynamic entities u16 0x106/0x108, lists 0x110/0x114, 0x54 each;
        // ComPrimaryLight 0x48, colour at 8
        MapLayout Map() const override
        {
            return { 0x10, 0x14, 0x18, 0x28, 0x30, 0x34, 0x38, 0x168, 0x16c, 0x38, 48, 52, 4,
                     0x25c, 0x2a8, 0x34, 0x14, 0x2b0, 0x5c, 0x106, 0x110, 0x54, 0x48, 8 };
        }
        XMaterial_t Material(uint64_t Address) const override { return GameWorldAtWar::ReadXMaterial(Address); }
        // clipMap_t (OAT's T4 struct, the t4 tables' col_map): dmaterial_t 0x48 (char[64], surface,
        // contents); cbrush_t 0x50 (mins, contents 0xc, maxs 0x10, numsides 0x1c, sides 0x20, axial
        // material numbers 0x24); cbrushside_t 0xc (plane, material number); cmodel_t 0x48, its leaf's
        // leafBrushNode at 0x40; cLeafBrushNode_s 0x14
        Maps::ClipLayout ClipBrushes() const override
        {
            Maps::ClipLayout C;
            C.Info.MaterialCount = 0x18, C.Info.Materials = 0x1c, C.Info.BrushCount = 0x9c, C.Info.Brushes = 0xa0;
            C.Info.LeafBrushNodeCount = 0x40, C.Info.LeafBrushNodes = 0x44;
            C.MaterialSize = 0x48, C.MaterialNameInline = true, C.MaterialSurface = 0x40, C.MaterialContents = 0x44;
            C.BrushSize = 0x50, C.BrushMins = 0, C.BrushContents = 0xc, C.BrushMaxs = 0x10, C.BrushSideCount = 0x1c, C.BrushSides = 0x20;
            C.AxialMaterial = 0x24;
            C.SideSize = 0xc, C.SidePlane = 0, C.SideMaterial = 4;
            C.ModelCount = 0x94, C.Models = 0x98, C.ModelSize = 0x48, C.ModelLeafBrushNode = 0x40;
            C.VertexCount = 0x58, C.Vertices = 0x5c, C.TriangleCount = 0x70, C.Triangles = 0x74;   // u16 vertex numbers
            return C;
        }

        // Loaded sounds (an alias's SoundFile of type 1): a whole .wav file in the zone (RIFF, the
        // archives' own format, MS ADPCM or PCM), u32@8 bytes read right after the inline name
        // (Load_LoadedSound: `if (data) read(size)`, a test the layout does not link), written as
        // it is under the alias' path
        void ExportLoadedSounds(const Context& C, Totals& T) override
        {
            auto& Kt = T.Kinds[Kind::Sound];
            const fs::path Root = C.KindDir(Kind::Sound);
            const auto& Z = Image();
            std::vector<int64_t> Found;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "loaded_sound") && At<uint32_t>(Z, I.Pos + LoadedSize) != 0) Found.push_back(I.Pos);
            for (size_t N = 0; N < Found.size(); N++)
            {
                const int64_t Pos = Found[N];
                const std::string Full = AssetName(Kind::Sound, Pos);
                if (!C.Advance(N, Found.size(), Full)) break;
                if (Full.empty() || !C.Wants(Kind::Sound, Full)) continue;
                Kt.Seen++;
                const uint32_t Size = At<uint32_t>(Z, Pos + LoadedSize);
                const int64_t NameAt = Off(PointerAt(Pos));
                const int64_t Data = NameAt < 0 ? -1 : NameAt + (int64_t)Full.size() + 1;
                if (Data < 0 || Data + (int64_t)Size > (int64_t)Z.size() || std::memcmp(Z.data() + Data, "RIFF", 4) != 0)
                {
                    Counter("sounds_loaded_unread")++;
                    T.Fail(Name(), Kind::Sound, Full, "no RIFF data after its name");
                    continue;
                }
                const fs::path Out = Root / fs::path(LoadedSoundFile(Full)).make_preferred();
                std::string Why;
                if (ExportWriters::WriteSound(ExportWriters::SoundSource::OfFile(Z.data() + Data, Size, Full), Out.string(),
                                              ExportWriters::SoundFormatSetting(), &Why))
                {
                    Kt.Written++;
                    Counter("sounds_loaded")++;
                }
                else
                    T.Fail(Name(), Kind::Sound, Full, Why);
            }
        }

    private:
        OatWeapons::Source Weapons;
    };

    class WawGame final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "waw"; }
        const char* Title() const override { return "Call of Duty: World at War"; }
        const char* Engine() const override { return "T4"; }
        std::vector<std::string> Aliases() const override { return { "t4" }; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<WawZone>(std::string(), std::string()); }
        // multiplayer maps by MPUI_, else the patch's PATCH_ (mp_drum "Battery"), zombies by
        // PATCH_LEVEL_ZOMBIE_ (nazi_zombie_factory "Der Riese"), the campaign by MENU_LEVEL_
        const LevelNaming* LevelNames() const override
        {
            static const LevelNaming Naming{ { "localized_code_post_gfx_mp", "localized_common_mp", "code_post_gfx", "patch" },
                                             { { "mp_*", { "MPUI_{REST}", "PATCH_{REST}" } },
                                               { "nazi_zombie_*", { "PATCH_LEVEL_ZOMBIE_{REST}", "MENU_LEVEL_ZOMBIE_{REST}" } },
                                               { "*", { "MENU_LEVEL_{NAME}" } } } };
            return &Naming;
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const ExportOptions& Options, std::string& Error) const override
        {
            auto Zone = std::make_unique<WawZone>(Path, Iw3Family::InstallOf(Path, Options.Install));
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const WawGame Game;
    const DiskGame::Registration Registered(Game);
}
