#include "stdafx.h"

// Call of Duty: Black Ops (T5) on the disk template: a ZonekitZone with the t5 profile (32-bit:
// every pointer 4 bytes, the zone served below 4 GB), served to the live Black Ops readers
// (GameBlackOps.cpp's ReadXModel / ReadXAnim / ReadXMaterial). Without OpenAssetTools, whose path
// stays as --game bo1-oat. Images: .iwi in the install's .iwd archives (code images: pixels in the
// zone); sounds: SndBank aliases naming an .iwd .wav (MS-ADPCM) or a loaded RIFF in the zone.

#include "IWDSoundDecoder.h"
#include "CoDIWITranslator.h"
#include "Games/Treyarch/TreyarchSoundAliases.h"
#include "DiskGame.h"
#include "Games/Treyarch/TreyarchEffects.h"
#include "Games/Treyarch/TreyarchGscOwners.h"
#include "Games/Treyarch/TreyarchLevel.h"
#include "ExportWriters.h"
#include "GameBlackOps.h"
#include "Games/IW3/Iw3Install.h"
#include "Games/IW3/LoadDefImage.h"

#include "Image.h"
#include "IWDCache.h"
#include "MapLive.h"
#include <miniz/miniz.h>
#include "ZonekitZone.h"
#include "WmaSounds.h"
#include "Games/OAT/OatWeapons.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{
    using DiskGame::Context;
    using DiskGame::ExportOptions;
    using DiskGame::Kind;
    using DiskGame::Totals;

    // GfxImage (0x34, Load_GfxImage 0x281280): texture (its GfxImageLoadDef) at 0, width u16 at
    // 0x14, height at 0x16, the name at 0x2c. GfxImageLoadDef (0xc + resourceSize): levelCount u8,
    // flags u8, a D3DFORMAT at 4, resourceSize at 8, the pixels from 0xc, the largest level first.
    constexpr int64_t ImageNameAt = 0x2c, ImageWidth = 0x14, ImageHeight = 0x16;
    // SndBank (0x28, 0x422b0): alias lists at 8, count at 4; snd_alias_list_t (0x14): aliases at 8,
    // count at 0xc; snd_alias_t (0x54): SoundFile at 0x10; SoundFile (8): the sound at 0, type u8 at
    // 4 (1 loaded); LoadedSound (0x3c): name 0, data_size 0x34, data 0x38; StreamedSound: name 0.
    constexpr int64_t BankAliases = 8, BankCount = 4, ListHead = 8, ListCount = 0xc, AliasSize = 0x54,
                      AliasFile = 0x10, FileType = 4, LoadedDataSize = 0x34, LoadedData = 0x38;
    // RawFile (0xc): name 0, len 4, buffer 8 (len + 1 bytes)
    constexpr int64_t RawLen = 4, RawBuffer = 8;
    // GfxWorld (0x43c, Load_GfxWorld 0x142360): name 0, baseName 4, surfaceCount 0x10, skyImage 0x2c,
    // skyBoxModel 0x34, draw 0x158 (vertexCount +0x9c, 44-byte vertices +0xa0, indexCount +0xb8,
    // u16 indices +0xbc), models 0x250/0x254 (60 bytes: surfaceCount 52, startSurfIndex 56),
    // dpvs 0x344 (smodelCount +0, surfaces +0x58 (0x50 each: srfTriangles_t firstVertex 0x1c,
    // triCount u16 0x22, baseIndex 0x24; the material 0x30), smodelDrawInsts +0x60 (0x4c each: the
    // placement 0, the model 0x38)). ComWorld (0x40): primaryLightCount 8, primaryLights 0xc (0xdc each).
    TreyarchLevelZone::Level32Layout Level()
    {
        constexpr int64_t Draw = 0x158, Dpvs = 0x344;
        TreyarchLevelZone::Level32Layout L;
        L.BaseName = 4, L.SurfaceCount = 0x10, L.SkyImage = 0x2c, L.SkyModel = 0x34;
        L.VertexCount = Draw + 0x9c, L.Vertices = Draw + 0xa0, L.IndexCount = Draw + 0xb8, L.Indices = Draw + 0xbc;
        L.Format = TreyarchLevelZone::VertexFormat::Records44;
        L.BrushCount = 0x250, L.Brushes = 0x254, L.BrushSize = 60, L.BrushSurfaceCount = 52, L.BrushFirstSurface = 56;
        L.SModelCount = Dpvs, L.Surfaces = Dpvs + 0x58, L.SModels = Dpvs + 0x60, L.SModelSize = 0x4c, L.SModelModel = 0x38;
        L.SurfaceSize = 0x50, L.SurfaceFirstVertex = 0x1c, L.SurfaceTris = 0x22, L.SurfaceBaseIndex = 0x24, L.SurfaceMaterial = 0x30;
        L.LightCount = 8, L.Lights = 0xc, L.LightSize = 0xdc;
        // clipMap_t (0x14c, OAT's T5 struct, the t5 tables' col_map): dmaterial_t 0x48 (char[64],
        // surface, contents); cbrush_t 0x60 (mins, contents 0xc, maxs 0x10, numsides 0x1c, sides
        // 0x20, axial cflags 0x24 and sflags 0x3c); cbrushside_t 0xc (plane, cflags, sflags);
        // cmodel_t 0x48, its leaf's leafBrushNode at 0x40; cLeafBrushNode_s 0x14
        Maps::ClipLayout& C = L.Clip;
        C.Info.MaterialCount = 0x18, C.Info.Materials = 0x1c, C.Info.BrushCount = 0x94, C.Info.Brushes = 0x98;
        C.Info.LeafBrushNodeCount = 0x38, C.Info.LeafBrushNodes = 0x3c;
        C.MaterialSize = 0x48, C.MaterialName = 0, C.MaterialNameInline = true, C.MaterialSurface = 0x40, C.MaterialContents = 0x44;
        C.BrushSize = 0x60, C.BrushMins = 0, C.BrushContents = 0xc, C.BrushMaxs = 0x10, C.BrushSideCount = 0x1c, C.BrushSides = 0x20;
        C.AxialContents = 0x24, C.AxialSurface = 0x3c;
        C.SideSize = 0xc, C.SidePlane = 0, C.SideContents = 4, C.SideSurface = 8;
        C.ModelCount = 0x8c, C.Models = 0x90, C.ModelSize = 0x48, C.ModelLeafBrushNode = 0x40;
        C.NodeSize = 0x14;
        C.VertexCount = 0x50, C.Vertices = 0x54, C.TriangleCount = 0x68, C.Triangles = 0x6c;   // u16 vertex numbers
        return L;
    }

    // Every file of the install's .iwd archives by its lowercased path, the first archive in the
    // order the OAT path's FindIwdEntry walks them (a recursive directory walk) holding it, each
    // archive opened once: a sound is then one extraction, not a walk over every archive
    class IwdFiles
    {
    public:
        explicit IwdFiles(const std::string& Install)
        {
            std::error_code Ec;
            for (auto It = fs::recursive_directory_iterator(Install, Ec); It != fs::recursive_directory_iterator(); It.increment(Ec))
            {
                if (Ec) break;
                if (!It->is_regular_file(Ec) || ExportNaming::Lower(It->path().extension().string()) != ".iwd") continue;
                auto Zip = std::make_unique<mz_zip_archive>();
                std::memset(Zip.get(), 0, sizeof(mz_zip_archive));
                if (!mz_zip_reader_init_file(Zip.get(), It->path().string().c_str(), 0)) continue;
                const mz_uint N = mz_zip_reader_get_num_files(Zip.get());
                for (mz_uint K = 0; K < N; K++)
                {
                    char Name[512];
                    if (mz_zip_reader_get_filename(Zip.get(), K, Name, sizeof(Name)) == 0) continue;
                    Files.emplace(ExportNaming::ForwardSlashes(ExportNaming::Lower(Name)), std::make_pair(Archives.size(), K));
                }
                Archives.push_back(std::move(Zip));
            }
        }
        ~IwdFiles()
        {
            for (auto& Z : Archives) mz_zip_reader_end(Z.get());
        }
        IwdFiles(const IwdFiles&) = delete;
        IwdFiles& operator=(const IwdFiles&) = delete;

        bool Read(const std::string& Path, std::vector<uint8_t>& Out)
        {
            const auto It = Files.find(ExportNaming::ForwardSlashes(ExportNaming::Lower(Path)));
            if (It == Files.end()) return false;
            std::lock_guard<std::mutex> G(Lock);
            mz_zip_archive* Zip = Archives[It->second.first].get();
            mz_zip_archive_file_stat Stat;
            if (!mz_zip_reader_file_stat(Zip, It->second.second, &Stat) || Stat.m_uncomp_size == 0) return false;
            Out.resize((size_t)Stat.m_uncomp_size);
            return mz_zip_reader_extract_to_mem(Zip, It->second.second, Out.data(), Out.size(), 0) != 0;
        }

    private:
        std::vector<std::unique_ptr<mz_zip_archive>> Archives;
        std::unordered_map<std::string, std::pair<size_t, mz_uint>> Files;
        std::mutex Lock;
    };

    std::mutex IwdFilesLock;
    std::map<std::string, std::shared_ptr<IwdFiles>> IwdFilesByInstall;

    std::shared_ptr<IwdFiles> IwdFilesOf(const std::string& Install)
    {
        std::lock_guard<std::mutex> G(IwdFilesLock);
        auto& P = IwdFilesByInstall[Install];
        if (!P) P = std::make_shared<IwdFiles>(Install);
        return P;
    }

    class Bo1Zone final : public TreyarchLevelZone
    {
    public:
        Bo1Zone(const std::string& Path, std::string InstallDir) : TreyarchLevelZone(Path), Install(std::move(InstallDir)), Weapons(Path)
        {
            // the weapon files and their stats table, through OpenAssetTools (Games/OAT/OatWeapons.h)
            Exports(Kind::Weapons, [this](const Context& C, Totals& T) { OatWeapons::ExportWeapons(Weapons, Name(), C, T); });
            Exports(Kind::WeaponStats, [this](const Context& C, Totals& T) { OatWeapons::ExportWeaponStats(Weapons, Name(), C, T, "Call of Duty: Black Ops"); });
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            // the rawfile kind's loop is ZonekitZone's, its bytes RawfileBytes'
            Exports(Kind::Rawfile, [this](const Context& C, Totals& T) {
                ExportRawfiles(C, T, { { "rawfile", [this](int64_t Pos, std::string& Nm, std::vector<uint8_t>& Bytes) { return RawfileBytes(Pos, Nm, Bytes); } } });
            });
            // stringtable (0x14, OpenAssetTools' layout) {name, i32 columns 4, rows 8, cells 0xc of 8
            // bytes {string, hash}, cell index 0x10}: CSV
            Exports(Kind::Data, [this](const Context& C, Totals& T) {
                ExportStringTables(C, T, "stringtable", [this](int64_t Pos, uint32_t& Columns, uint32_t& Rows, std::vector<std::string>& Cells) {
                    return StringTableCells(Pos, { 4, 8, 0xc, 8 }, Columns, Rows, Cells);
                });
            });
            // the .gsc/.csc rawfiles: source
            Exports(Kind::Script, [this](const Context& C, Totals& T) {
                ExportScripts(C, T, {}, { { "rawfile", [this](int64_t Pos, Scripts::Script& S) {
                                  if (!Scripts::IsSourceName(S.Name)) S.Name.clear();
                                  return S.Name.empty() || RawfileBytes(Pos, S.Name, S.Bytes);
                              } } });
            });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
            // Material: its techset, images and blend factors as JSON, its images by the image kind;
            // FxEffectDef (0x3c, elements 0x124) as JSON
            ExportsEffects(GameBlackOps::ReadXMaterial, Treyarch::EffectLayout::Bo1);
        }
        ~Bo1Zone() override
        {
            Serve(false);
        }

        // its string tables, its rawfiles as text (the scripts, AI types, characters and animtrees
        // the anim owners are read from) and its weapons' models and xanims (TreyarchGscOwners.h).
        // WeaponVariantDef (0xe4): WeaponDef at 8, szXAnims at 0x10 (66); WeaponDef: gunXModel 4 (16), handXModel 8
        void Documents(std::vector<std::pair<std::string, std::string>>& Out) const override
        {
            ZonekitZone::Documents(Out);
            for (const auto& I : Walk().Instances)
            {
                if (I.Parent >= 0 || !OwnerIs(I, "rawfile")) continue;
                const std::string Nm = AssetName(Kind::Rawfile, I.Pos);
                std::vector<uint8_t> Bytes;
                bool Inflated = false;
                if (!IsReference(Kind::Rawfile, Nm, I.Pos) && RawfileData(I.Pos, Nm, Bytes, Inflated))
                    Out.emplace_back(Nm, std::string(Bytes.begin(), Bytes.end()));
            }
            WeaponLayout W;
            W.Anims = 0x10, W.AnimCount = 66;
            WeaponDocuments(Out, W);
        }

    protected:
        const char* Profile() const override { return "t5"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("T5"); }
        // an asset pointer's back reference is resolved through the slot it names (the Ptr
        // loaders call DB_ConvertOffsetToAlias 0x19bf20: *p = *(block + offset)); a data one is
        // the place itself (DB_ConvertOffsetToPointer 0x1daca0)
        bool DerefAssetPointers() const override { return true; }
        VirtualGameSession::Handlers Handlers() override
        {
            Slots = ZoneStrings(Zone());
            return { SlotString, GameBlackOps::LoadXImage };
        }
        void Served(bool On) override { Serve(On); }
        void Fixups() override { Serve(true); }
        std::string AssetName(Kind K, int64_t Pos) const override
        {
            return StringAt(PointerAt(K == Kind::Image ? Pos + ImageNameAt : Pos));
        }

        // The level (TreyarchLevel.h, Black Ops' places below)
        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override
        {
            return ReadLevel(Scene, Models, Level(), GameBlackOps::ReadXMaterial);
        }

    private:
        std::string Install;
        OatWeapons::Source Weapons;
        std::shared_ptr<IWDCache> Iwd;

        // the live image handler reads the .iwd through CoDAssets::GamePackageCache while the
        // zone is served; the previous cache comes back when it is not
        void Serve(bool On)
        {
            if (Install.empty()) return;
            ServePackages(On, [this] {
                if (!Iwd) Iwd = Iw3Family::IwdPackagesOf(Install);
                return SharedPackages(Iwd);
            });
        }

        // The zone's script strings: count at 0x24, the list pointer at 0x28 (any
        // non-null value: present), u32 slots from 0x34, then the text of every -1 slot
        static std::vector<std::string> ZoneStrings(const std::vector<uint8_t>& Z)
        {
            const uint32_t Count = At<uint32_t>(Z, 0x24);
            if (At<uint32_t>(Z, 0x28) == 0 || Count == 0 || 0x34 + (size_t)Count * 4 > Z.size()) return {};
            return InlineStrings(Z, 0x34, Count, 4, 0xFFFFFFFFu);
        }

        // Every image the zone names (its own and its materials'): from the .iwd by name, or from
        // the pixels the zone carries (a loadDef: the code images). A "," name is a reference, read
        // from the .iwd by name too: a localized archive's image (localized_<language>_iw*.iwd: the
        // waypoint_* icons common_mp names) is held whole by no zone.
        void ExportImages(const Context& C, Totals& T)
        {
            ExportNamedImages(C, T, [this](const std::string& N, int64_t Pos) { return Decode(N, Pos); },
                              "neither in the .iwd archives nor in the zone", true);
        }

        std::string IndexKey() const override { return ZonekitZone::IndexKey() + "_images1"; }
        // the images this zone holds whole, for the install index (a texture another zone names)
        void Defines(std::vector<DiskGame::AssetId>& Out) const override
        {
            ZonekitZone::Defines(Out);
            DefineImages(Out);
        }

        std::unique_ptr<XImageDDS> Decode(const std::string& ImageName, int64_t Pos)
        {
            if (!Install.empty())
            {
                if (!Iwd) Iwd = Iw3Family::IwdPackagesOf(Install);
                uint32_t Size = 0;
                auto Data = Iwd->ExtractPackageObject(Iwd->HashPackageID(ImageName), Size);
                if (Data)
                {
                    Counter("images_iwd")++;
                    return CoDIWITranslator::TranslateIWI(Data, Size);
                }
            }
            auto Inline = LoadDef(Pos);
            if (Inline) Counter("images_in_zone")++;
            return Inline;
        }

        std::unique_ptr<XImageDDS> LoadDef(int64_t Pos)
        {
            const int64_t Def = Off(PointerAt(Pos));
            if (Def < 0) return nullptr;
            const auto& Z = Image();
            const uint32_t W = At<uint16_t>(Z, Pos + ImageWidth), H = At<uint16_t>(Z, Pos + ImageHeight);
            const uint32_t Format = At<uint32_t>(Z, Def + 4), Size = At<uint32_t>(Z, Def + 8);
            ImageFormat Fmt = ImageFormat::DDS_BC1_UNORM;
            uint64_t Level = 0;
            bool Swap = false;
            const bool Packed = Format == 23 || Format == 34;   // 23 R5G6B5, 34 G16R16 (the lightmaps): widened to 8-bit RGBA
            if (Packed)
                Fmt = ImageFormat::DDS_Standard_A8R8G8B8, Level = (uint64_t)W * H * (Format == 23 ? 2 : 4);
            else if (!LoadDefImage::D3dFormat(Format, W, H, Fmt, Level, Swap))
            {
                Counter("images_in_zone_unknown_format")++;
                return nullptr;
            }
            if (W == 0 || H == 0 || Level > Size || Def + 0xc + (int64_t)Level > (int64_t)Z.size()) return nullptr;
            const uint8_t* Src = Z.data() + Def + 0xc;
            if (!Packed) return LoadDefImage::Dds(W, H, Fmt, Src, Level, Swap);
            std::vector<uint8_t> Dst((size_t)W * H * 4);
            if (Format == 23)
                for (uint64_t P = 0; P < (uint64_t)W * H; P++)
                {
                    const uint16_t V = (uint16_t)(Src[2 * P] | (Src[2 * P + 1] << 8));
                    Dst[4 * P] = (uint8_t)(((V >> 11) & 31) * 255 / 31);
                    Dst[4 * P + 1] = (uint8_t)(((V >> 5) & 63) * 255 / 63);
                    Dst[4 * P + 2] = (uint8_t)((V & 31) * 255 / 31);
                    Dst[4 * P + 3] = 255;
                }
            else
                for (uint64_t P = 0; P < (uint64_t)W * H; P++)
                {
                    Dst[4 * P] = Src[4 * P + 1];       // R16's high byte
                    Dst[4 * P + 1] = Src[4 * P + 3];   // G16's high byte
                    Dst[4 * P + 2] = 0;
                    Dst[4 * P + 3] = 255;
                }
            return LoadDefImage::Dds(W, H, Fmt, Dst.data(), Dst.size());
        }

        // Every alias of every SndBank: a loaded sound is a whole RIFF/WAVE in the zone, written as
        // it is; a streamed one names a .wav of the .iwd archives (block MS-ADPCM), decoded to PCM.
        void ExportSounds(const Context& C, Totals& T)
        {
            ExportSoundFiles(C, T);
            if (!SoundAliasCsv::Wanted(C)) return;
            // a SoundFile's file as ExportSoundFiles writes it: its sound's name, .wav added
            const auto File = [&](int64_t Spec) -> std::string {
                const int64_t Snd = Spec < 0 ? -1 : Off(PointerAt(Spec));
                std::string Nm = Snd < 0 ? std::string() : ExportNaming::ForwardSlashes(StringAt(PointerAt(Snd)));
                if (!Nm.empty() && (Nm.size() < 4 || ExportNaming::Lower(Nm.substr(Nm.size() - 4)) != ".wav")) Nm += ".wav";
                return Nm.empty() ? Nm : ExportWriters::SoundFileName(Nm, ExportWriters::SoundFormatSetting());
            };
            SoundAliasCsv::Rows Rows;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "sound") && I.Parent < 0) Treyarch::ReadBo1SoundBank(AliasView(), I.Pos, File, Rows);
            SoundAliasCsv::Write(C, T, Name(), Rows);
        }

        // A WMA loaded sound (format 7) as the RIFF XWMA World at War keeps the same packets in (fmt
        // 0x161), for ffmpeg to decode (WmaSounds.h; none: false, the sound left out as before). The
        // packets are fixed-size (mp_nuked: 2230 bytes at 44.1 kHz mono, 4096 at 48 kHz stereo), one
        // a seek table entry (snd_asset seek_table_count 0x2c, seek_table 0x30), each entry the bytes
        // decoded at the end of its packet: xWMA's dpds (its last frames x 2 x channels). The bit rate
        // xWMA's fmt carries is not kept: World at War's 12000 bytes a second for mono, 20000 for
        // stereo (the one ffmpeg decodes the stereo ones with), else WmaSounds tries the others.
        bool WrapWma(int64_t Snd, const uint8_t* Data, uint32_t Size, std::vector<uint8_t>& Out)
        {
            const auto& Z = Image();
            if (!WmaSounds::Available())
            {
                Counter(WmaSounds::NoFfmpegCounter)++;
                return false;
            }
            const uint32_t Frames = At<uint32_t>(Z, Snd + 8), Rate = At<uint32_t>(Z, Snd + 0xc), Channels = At<uint32_t>(Z, Snd + 0x10);
            const uint32_t Packets = At<uint32_t>(Z, Snd + 0x2c);
            const int64_t Seek = Off(PointerAt(Snd + 0x30));
            if (Channels == 0 || Channels > 2 || Rate == 0 || Packets == 0 || Seek < 0 || Size % Packets != 0 ||
                (uint64_t)Seek + Packets * 4ull > Z.size() || Size / Packets > 0xffff)
            {
                Counter("sounds_loaded_wma_unread")++;
                return false;
            }
            std::vector<uint32_t> Dpds(Packets);
            std::memcpy(Dpds.data(), Z.data() + Seek, Packets * 4ull);
            if (!std::is_sorted(Dpds.begin(), Dpds.end()) || Dpds.back() != (uint64_t)Frames * 2 * Channels)
            {
                Counter("sounds_loaded_wma_seek_table_odd")++;
                return false;
            }
            Out = WmaSounds::Xwma((uint16_t)Channels, Rate, Channels == 1 ? 12000 : 20000, (uint16_t)(Size / Packets), Dpds.data(),
                                  Dpds.size(), Data, Size);
            Counter("sounds_loaded_wma")++;
            return true;
        }

        // A loaded sound without a container (LoadedSound: its snd_asset at 4, OAT's T5 layout: frame
        // count 8, rate 0xc, channels 0x10, block size 0x18, format 0x20): MS-ADPCM (6) as a WAV of
        // that format, every block's predictors checked (below 7); WMA (7) as an xWMA (WrapWma);
        // false for any other
        bool WrapLoaded(int64_t Snd, const uint8_t* Data, uint32_t Size, std::vector<uint8_t>& Out)
        {
            const auto& Z = Image();
            const uint32_t Frames = At<uint32_t>(Z, Snd + 8), Rate = At<uint32_t>(Z, Snd + 0xc), Channels = At<uint32_t>(Z, Snd + 0x10);
            const uint32_t Format = At<uint32_t>(Z, Snd + 0x20);
            if (Format == 7) return WrapWma(Snd, Data, Size, Out);
            // the block size field (67072) is the streaming chunk, as in the .iwd files: the blocks are
            // the standard 512-sample ones (IWDSoundDecoder), 262 bytes a channel (mp_nuked: 16 of 16
            // sizes are a whole number of them, frames / 512 x 262)
            const uint32_t Block = 262 * Channels;
            if (Format != 6 || Channels == 0 || Channels > 2 || Rate == 0) return false;
            for (uint32_t B = 0; B + Block <= Size; B += Block)
                for (uint32_t Ch = 0; Ch < Channels; Ch++)
                    if (Data[B + Ch] >= 7)
                    {
                        Counter("sounds_loaded_adpcm_bad_block")++;
                        return false;
                    }
            if (Size % Block != 0)
            {
                Counter("sounds_loaded_adpcm_partial_block")++;
                return false;
            }
            const uint16_t PerBlock = 512;
            static constexpr int16_t Coefs[7][2] = { { 256, 0 }, { 512, -256 }, { 0, 0 }, { 192, 64 }, { 240, 0 }, { 460, -208 }, { 392, -232 } };
            Out.clear();
            const auto U16 = [&](uint32_t V) { Out.push_back((uint8_t)V), Out.push_back((uint8_t)(V >> 8)); };
            const auto U32 = [&](uint32_t V) { U16(V & 0xffff), U16(V >> 16); };
            const auto Tag = [&](const char* T4) { Out.insert(Out.end(), T4, T4 + 4); };
            Tag("RIFF"), U32(0), Tag("WAVE");
            Tag("fmt "), U32(50);
            U16(2), U16(Channels), U32(Rate), U32((uint32_t)((uint64_t)Rate * Block / PerBlock)), U16(Block), U16(4), U16(32), U16(PerBlock), U16(7);
            for (const auto& C2 : Coefs) U16((uint16_t)C2[0]), U16((uint16_t)C2[1]);
            Tag("fact"), U32(4), U32(Frames);
            Tag("data"), U32(Size);
            Out.insert(Out.end(), Data, Data + Size);
            if (Size & 1) Out.push_back(0);
            const uint32_t Riff = (uint32_t)Out.size() - 8;
            std::memcpy(Out.data() + 4, &Riff, 4);
            return true;
        }

        void ExportSoundFiles(const Context& C, Totals& T)
        {
            auto& Kt = T.Kinds[Kind::Sound];
            const auto& Z = Image();
            std::map<std::string, int64_t> Loaded;   // file -> its LoadedSound
            std::set<std::string> Streamed;
            for (const auto& I : Walk().Instances)
            {
                if (!OwnerIs(I, "sound") || I.Parent >= 0) continue;
                const int64_t Lists = Off(PointerAt(I.Pos + BankAliases));
                const uint32_t Lcount = At<uint32_t>(Z, I.Pos + BankCount);
                for (uint32_t L = 0; Lists >= 0 && L < Lcount; L++)
                {
                    const int64_t List = Lists + (int64_t)L * 0x14;
                    const int64_t Head = Off(PointerAt(List + ListHead));
                    const uint32_t Count = At<uint32_t>(Z, List + ListCount);
                    for (uint32_t K = 0; Head >= 0 && K < Count && K < 4096; K++)
                    {
                        const int64_t File = Off(PointerAt(Head + (int64_t)K * AliasSize + AliasFile));
                        if (File < 0) continue;
                        const int64_t Snd = Off(PointerAt(File));
                        if (Snd < 0) continue;
                        std::string Nm = ExportNaming::ForwardSlashes(StringAt(PointerAt(Snd)));
                        if (Nm.empty()) continue;
                        if (Nm.size() < 4 || ExportNaming::Lower(Nm.substr(Nm.size() - 4)) != ".wav") Nm += ".wav";
                        if (At<uint8_t>(Z, File + FileType) == 1) Loaded.emplace(Nm, Snd);
                        else Streamed.insert(Nm);
                    }
                }
            }
            const fs::path Root = C.KindDir(Kind::Sound);
            const std::string Format = ExportWriters::SoundFormatSetting();
            const auto Write = [&](const uint8_t* Data, size_t Size, const std::string& F, std::string& Why) {
                return ExportWriters::WriteSound(ExportWriters::SoundSource::OfFile(Data, Size, F),
                                                 Root / fs::path(ExportWriters::SoundFileName(F, Format)).make_preferred(), Format, &Why);
            };
            size_t N = 0;
            const size_t All = Loaded.size() + Streamed.size();
            for (const auto& [F, Snd] : Loaded)
            {
                if (!C.Advance(N++, All, F)) return;
                if (!C.Wants(Kind::Sound, F)) continue;
                const uint32_t Size = At<uint32_t>(Z, Snd + LoadedDataSize);
                const int64_t Data = Off(PointerAt(Snd + LoadedData));
                std::vector<uint8_t> Wrapped;
                if (Size >= 12 && Data >= 0 && (uint64_t)Data + Size <= Z.size() && std::memcmp(Z.data() + Data, "RIFF", 4) != 0 &&
                    WrapLoaded(Snd, Z.data() + Data, Size, Wrapped))
                {
                    Kt.Seen++;
                    std::string Why;
                    if (Write(Wrapped.data(), Wrapped.size(), F, Why))
                    {
                        Kt.Written++;
                        Counter("sounds_loaded_wrapped")++;
                    }
                    else
                        T.Fail(Name(), Kind::Sound, F, Why);
                    continue;
                }
                if (Size < 12 || Data < 0 || (uint64_t)Data + Size > Z.size() || std::memcmp(Z.data() + Data, "RIFF", 4) != 0 ||
                    std::memcmp(Z.data() + Data + 8, "WAVE", 4) != 0)
                {
                    Counter("sounds_loaded_not_riff")++;
                    Counter("sounds_loaded_format_" + std::to_string(At<uint32_t>(Z, Snd + 4 + 0x1c)) + "_size_" +
                            (Size == 0 ? "0" : "n"))++;
                    continue;
                }
                Kt.Seen++;
                std::string Why;
                if (Write(Z.data() + Data, Size, F, Why))
                {
                    Kt.Written++;
                    Counter("sounds_loaded")++;
                }
                else
                    T.Fail(Name(), Kind::Sound, F, Why);
            }
            for (const auto& F : Streamed)
            {
                if (!C.Advance(N++, All, F)) return;
                if (Loaded.count(F) || !C.Wants(Kind::Sound, F) || Install.empty()) continue;
                Kt.Seen++;
                // the IWD's block MS-ADPCM decoded to PCM16, or an entry that is a RIFF already
                std::vector<uint8_t> Raw, Wav;
                const bool Found = IwdFilesOf(Install)->Read(F, Raw);
                const bool Riff = Raw.size() >= 12 && std::memcmp(Raw.data(), "RIFF", 4) == 0 && std::memcmp(Raw.data() + 8, "WAVE", 4) == 0;
                const std::vector<uint8_t>* Payload = !Found ? nullptr : DecodeIWDSound(Raw, Wav) ? &Wav : Riff ? &Raw : nullptr;
                std::string Why;
                if (Payload == nullptr)
                {
                    Counter("sounds_iwd_missing")++;
                    T.Fail(Name(), Kind::Sound, F, "not in the .iwd archives");
                }
                else if (Write(Payload->data(), Payload->size(), F, Why))
                {
                    Kt.Written++;
                    Counter("sounds_iwd")++;
                }
                else
                    T.Fail(Name(), Kind::Sound, F, Why);
            }
        }

        bool RawfileBytes(int64_t Pos, const std::string& Nm, std::vector<uint8_t>& Bytes)
        {
            bool Inflated = false;
            const bool Ok = RawfileData(Pos, Nm, Bytes, Inflated);
            if (Inflated) Counter("rawfiles_inflated")++;
            return Ok;
        }

        // RawFile: a .gsc/.csc buffer is {u32 decompressed size, u32 compressed size, zlib}; any
        // other file is its bytes
        bool RawfileData(int64_t Pos, const std::string& Nm, std::vector<uint8_t>& Bytes, bool& Inflated) const
        {
            const auto& Z = Image();
            const uint32_t Len = At<uint32_t>(Z, Pos + RawLen);
            const int64_t Data = Off(PointerAt(Pos + RawBuffer));
            bool Ok = Len == 0 || (Data >= 0 && (uint64_t)Data + Len <= Z.size());
            if (Ok && Len)
            {
                const std::string Ext = ExportNaming::Lower(fs::path(Nm).extension().string());
                const uint32_t Full = At<uint32_t>(Z, Data), Packed = At<uint32_t>(Z, Data + 4);
                if ((Ext == ".gsc" || Ext == ".csc") && Len >= 8 && (uint64_t)Packed + 8 <= Len && Z[Data + 8] == 0x78)
                {
                    Bytes.resize(Full);
                    mz_ulong Got = Full;
                    Ok = mz_uncompress(Bytes.data(), &Got, Z.data() + Data + 8, Packed) == MZ_OK;
                    Bytes.resize(Got);
                    if (!Bytes.empty() && Bytes.back() == 0) Bytes.pop_back();   // the size counts the NUL
                    Inflated = Ok;
                }
                else
                    Bytes.assign(Z.data() + Data, Z.data() + Data + Len);
            }
            return Ok;
        }

    };


    class Bo1Game final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "bo1"; }
        const char* Title() const override { return "Call of Duty: Black Ops"; }
        const char* Engine() const override { return "T5"; }
        std::vector<std::string> Aliases() const override { return { "t5" }; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<Bo1Zone>(std::string(), std::string()); }
        // multiplayer maps by MPUI_ (mp_nuked "Nuketown"), zombies by MENU_ (zombie_moon "Moon"),
        // World at War's remakes by WAW_MENU_, the campaign by MENU_LEVEL_<level>_CAPS
        const LevelNaming* LevelNames() const override
        {
            static const LevelNaming Naming{ { "??_code_post_gfx_mp", "??_code_post_gfx", "??_patch", "??_patch_mp" },
                                             { { "mp_*", { "MPUI_{REST}" } },
                                               { "zombie_cod5_*", { "WAW_MENU_ZOMBIE_{REST}_CAPS" } },
                                               { "zombie_*", { "MENU_{NAME}", "MENU_{NAME}_CAPS" } },
                                               { "*", { "MENU_LEVEL_{NAME}_CAPS" } } } };
            return &Naming;
        }

        // zone\Common\<zone>.ff: the zone folder above, its language's zones beside (zone\French\
        // fr_zombie_theater.ff holds zombie_theater's lobby doors), all in the install index
        std::string InstallOfZone(const std::string& Zone) const override
        {
            const fs::path Dir = fs::path(Zone).parent_path();
            if (ExportNaming::Lower(Dir.filename().string()) == "common" && ExportNaming::Lower(Dir.parent_path().filename().string()) == "zone")
                return Dir.parent_path().string();
            return IDiskGame::InstallOfZone(Zone);
        }

        // the anims' AI types, players and weapons, from the zones' scripts, animtrees and weapons
        // (TreyarchGscOwners.h): a zombie level's zones loaded with it (<zone>_patch,
        // common_zombie_patch, common_zombie, patch), multiplayer's (patch_mp, common_mp; the shared
        // ones the first level's, for its create-a-class players), the campaign's (<zone>_patch,
        // patch, common). The multiplayer types (mptype) are left out: no zone holds their bodies.
        std::vector<std::string> AnimOwnerDocuments() const override { return GscAnimOwners::Documents(); }
        void AnimOwners(const ExportOptions& Options, const std::vector<AnimZone>& Zones, ::AnimOwners::Book& Out) const override
        {
            TreyarchGscOwners::Setup S;
            S.Rules.PlayerTypes = false;
            S.Options.ByModelPrefix = S.Options.ByNameTokens = true;
            S.Shared = [](const fs::path& Zone) {
                const std::string Stem = ExportNaming::Lower(Zone.stem().string());
                const fs::path Dir = Zone.parent_path();
                const auto In = [&](std::initializer_list<std::string> Names) {
                    std::vector<fs::path> Out;
                    for (const auto& N : Names) Out.push_back(Dir / (N + ".ff"));
                    return Out;
                };
                if (Stem.rfind("zombie", 0) == 0) return In({ Stem + "_patch", "common_zombie_patch", "common_zombie", "patch" });
                if (Stem.rfind("mp_", 0) == 0) return In({ "patch_mp", "common_mp" });
                if (Stem.size() > 3 && Stem.compare(Stem.size() - 3, 3, "_mp") == 0)
                {
                    auto Out = In({ "patch_mp", "common_mp" });
                    Out.push_back(TreyarchGscOwners::FirstZone(Dir, "mp_*"));
                    return Out;
                }
                return In({ Stem + "_patch", "patch", "common" });
            };
            TreyarchGscOwners::Fill(*this, Options, Zones, Out, S);
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const ExportOptions& Options, std::string& Error) const override
        {
            auto Zone = std::make_unique<Bo1Zone>(Path, Iw3Family::InstallOf(Path, Options.Install));
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const Bo1Game Game;
    const DiskGame::Registration Registered(Game);
}
