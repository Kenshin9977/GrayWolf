#include "stdafx.h"

// Call of Duty 4 (IW3) on the disk template: an IW3-family zone (Games/IW3/IwdZone.h) with the
// iw3 profile (32-bit: every pointer 4 bytes, the zone served below 4 GB), served to the live CoD4
// readers (GameModernWarfare.cpp's ReadXModel / ReadXAnim / ReadXMaterial). Without
// OpenAssetTools. What is CoD4's own: loaded sounds are PCM in the zone (LoadedSound: data at
// +0x28, its size at +0xc), its XFile is 0x2c bytes, its aliases 0x5c.

#include "DiskGame.h"
#include "ExportWriters.h"
#include "GameModernWarfare.h"
#include "Games/IW3/IwdZone.h"
#include "Games/OAT/OatWeapons.h"
#include "SettingsManager.h"
#include "Strings.h"

#include <filesystem>
#include <format>

namespace fs = std::filesystem;

namespace
{
    using DiskGame::Context;
    using DiskGame::ExportOptions;
    using DiskGame::Kind;
    using DiskGame::Totals;

    // LoadedSound (0x2c): name at 0, MssSound's _AILSOUNDINFO from +4 (format, data_ptr, data_len
    // at +0xc, rate +0x10, bits +0x14, channels +0x18, samples +0x1c), its data pointer at +0x28
    // (the loader's BYTES*|0x24 of +4)
    constexpr int64_t SoundFormat = 4, SoundSize = 0xc, SoundRate = 0x10, SoundBits = 0x14, SoundChannels = 0x18,
                      SoundSamples = 0x1c, SoundData = 0x28;

    class Cod4Zone final : public Iw3Family::IwdZone
    {
    public:
        Cod4Zone(const std::string& Path, std::string InstallDir) : IwdZone(Path, std::move(InstallDir)), Weapons(Path)
        {
            // the weapon files and their stats table, through OpenAssetTools (Games/OAT/OatWeapons.h)
            Exports(Kind::Weapons, [this](const Context& C, Totals& T) { OatWeapons::ExportWeapons(Weapons, Name(), C, T); });
            Exports(Kind::WeaponStats, [this](const Context& C, Totals& T) { OatWeapons::ExportWeaponStats(Weapons, Name(), C, T, "Call of Duty 4: Modern Warfare"); });
        }

    protected:
        const char* Profile() const override { return "iw3"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("IW3"); }
        int64_t XFileSize() const override { return 0x2c; }
        LoadXImageHandler ImageHandler() const override { return GameModernWarfare::LoadXImage; }
        // snd_alias_t (0x5c): its SoundFile at +0x10; SoundFile (0xc): streamed, the folder at +4
        // and the file at +8
        SoundLayout Sounds() const override { return { 0x5c, 0x10, 4, 8, SoundSize }; }
        // snd_alias_t (0x5c, OAT's IW3_Assets.h): subtitle 4, secondary 8, SoundFile 0x10, volumes
        // 0x18/0x1c, pitches 0x20/0x24, distances 0x28/0x2c, flags 0x30 (looping 0; no 3D bit),
        // probability 0x38, center 0x40, start delay 0x44, envelope 0x4c/0x50/0x54
        IWSoundAliases::Layout AliasLayout() const override
        {
            IWSoundAliases::Layout L;
            L.ListCount = 8, L.ListCountBytes = 4, L.Size = 0x5c;
            L.Subtitle = 4, L.Secondary = 8, L.SoundFile = 0x10, L.VolMin = 0x18, L.VolMax = 0x1c, L.PitchMin = 0x20, L.PitchMax = 0x24;
            L.DistMin = 0x28, L.DistMax = 0x2c, L.Flags = 0x30, L.Probability = 0x38, L.CenterPercentage = 0x40, L.StartDelay = 0x44;
            L.EnvelopMin = 0x4c, L.EnvelopMax = 0x50, L.EnvelopPercentage = 0x54;
            return L;
        }
        // ExportLoadedSounds: flat, its stem, the sound format's extension
        std::string LoadedSoundFile(const std::string& Name) const override
        {
            return ExportNaming::SafeFileName(fs::path(Name).stem().string()) + ExportWriters::SoundExtension(ExportWriters::SoundFormatSetting());
        }
        // The level (the iw3 tables): GfxWorld (0x2dc) indices 0x10/0x14, surface count 0x18, sky
        // image 0x28, vertices 0x30/0x34 (44 bytes), brush models 0x150/0x154 (0x38: u16 surface
        // count at 48, first surface at 50), GfxWorldDpvsStatic at 0x244 (surfaces 0x294, 0x30
        // each, material at 0x10; static models 0x29c, 0x4c each); clipMap_t (0x11c) dynamic
        // entities u16 0xf4/0xf6, lists 0xf8/0xfc, 0x60 each; ComPrimaryLight 0x44, colour at 4
        MapLayout Map() const override
        {
            return { 0x10, 0x14, 0x18, 0x28, -1, 0x30, 0x34, 0x150, 0x154, 0x38, 48, 50, 2,
                     0x244, 0x294, 0x30, 0x10, 0x29c, 0x4c, 0xf4, 0xf8, 0x60, 0x44, 4 };
        }
        XMaterial_t Material(uint64_t Address) const override { return GameModernWarfare::ReadXMaterial(Address); }
        // clipMap_t (OAT's IW3 struct, the iw3 tables' col_map): World at War's records (dmaterial_t
        // 0x48, cbrush_t 0x50, cbrushside_t 0xc, cmodel_t 0x48), its fields from 0x58 on 8 bytes
        // nearer (no brush vertices): brushes u16 0x8c / 0x90, brush models 0x84 / 0x88
        Maps::ClipLayout ClipBrushes() const override
        {
            Maps::ClipLayout C;
            C.Info.MaterialCount = 0x18, C.Info.Materials = 0x1c, C.Info.BrushCount = 0x8c, C.Info.Brushes = 0x90;
            C.Info.LeafBrushNodeCount = 0x40, C.Info.LeafBrushNodes = 0x44;
            C.MaterialSize = 0x48, C.MaterialNameInline = true, C.MaterialSurface = 0x40, C.MaterialContents = 0x44;
            C.BrushSize = 0x50, C.BrushMins = 0, C.BrushContents = 0xc, C.BrushMaxs = 0x10, C.BrushSideCount = 0x1c, C.BrushSides = 0x20;
            C.AxialMaterial = 0x24;
            C.SideSize = 0xc, C.SidePlane = 0, C.SideMaterial = 4;
            C.ModelCount = 0x84, C.Models = 0x88, C.ModelSize = 0x48, C.ModelLeafBrushNode = 0x40;
            C.VertexCount = 0x58, C.Vertices = 0x5c, C.TriangleCount = 0x60, C.Triangles = 0x64;   // u16 vertex numbers
            return C;
        }

        // Loaded sounds: PCM in the zone, written as WAV through the live sound writer (the others,
        // whose header names a file of the archives, are written by the family's file export)
        void ExportLoadedSounds(const Context& C, Totals& T) override
        {
            auto& Kt = T.Kinds[Kind::Sound];
            const std::string Extension = ExportWriters::SoundExtension(ExportWriters::SoundFormatSetting());
            const std::string Dir = C.KindDir(Kind::Sound);
            std::vector<int64_t> Found;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "loaded_sound")) Found.push_back(I.Pos);
            for (size_t N = 0; N < Found.size(); N++)
            {
                const int64_t Pos = Found[N];
                const std::string Full = AssetName(Kind::Sound, Pos);
                if (!C.Advance(N, Found.size(), Full)) break;
                if (Full.empty() || !C.Wants(Kind::Sound, Full)) continue;
                const uint32_t Size = At<uint32_t>(Image(), Pos + SoundSize);
                const uint64_t Data = PointerAt(Pos + SoundData);
                if (Size == 0 || !Live().Contains(Data)) continue;   // a file of the archives
                Kt.Seen++;
                const uint32_t Format = At<uint32_t>(Image(), Pos + SoundFormat), Bits = At<uint32_t>(Image(), Pos + SoundBits);
                if (Format != 1 || Bits != 16)
                {
                    // not 16-bit PCM (an ADPCM sound): written behind a PCM header it would be noise
                    Counter("sounds_not_pcm")++;
                    T.Fail(Name(), Kind::Sound, Full, std::format("format {}, {} bits", Format, Bits));
                    continue;
                }
                CoDSound_t Sound;
                Sound.AssetName = ExportNaming::SafeFileName(fs::path(Full).stem().string());
                Sound.FullPath = fs::path(Full).parent_path().string();
                Sound.AssetPointer = Data;
                Sound.AssetSize = Size;
                Sound.FrameRate = At<uint32_t>(Image(), Pos + SoundRate);
                Sound.ChannelsCount = At<uint32_t>(Image(), Pos + SoundChannels);
                Sound.FrameCount = At<uint32_t>(Image(), Pos + SoundSamples);
                Sound.IsFileEntry = false;
                Sound.DataType = SoundDataTypes::WAV_NeedsHeader;
                std::string Crash;
                const int Code = ExportWriters::ExportSound(Sound, Dir, Extension, Crash);
                if (Code == static_cast<int>(ExportGameResult::Success)) Kt.Written++;
                else T.Fail(Name(), Kind::Sound, Full, "code " + std::to_string(Code) + " " + Crash);
            }
        }

    private:
        OatWeapons::Source Weapons;
    };

    class Cod4Game final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "cod4"; }
        const char* Title() const override { return "Call of Duty 4: Modern Warfare"; }
        const char* Engine() const override { return "IW3"; }
        std::vector<std::string> Aliases() const override { return { "iw3" }; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<Cod4Zone>(std::string(), std::string()); }
        // multiplayer maps by MPUI_ (mp_crossfire "Crossfire"), the campaign by MENU_SP_
        const LevelNaming* LevelNames() const override
        {
            static const LevelNaming Naming{ { "localized_code_post_gfx_mp", "code_post_gfx" },
                                             { { "mp_*", { "MPUI_{REST}" } }, { "*", { "MENU_SP_{NAME}" } } } };
            return &Naming;
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const ExportOptions& Options, std::string& Error) const override
        {
            auto Zone = std::make_unique<Cod4Zone>(Path, Iw3Family::InstallOf(Path, Options.Install));
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const Cod4Game Game;
    const DiskGame::Registration Registered(Game);
}
