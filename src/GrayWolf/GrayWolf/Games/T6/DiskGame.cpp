#include "stdafx.h"

// Call of Duty: Black Ops II (T6) on the disk template: a ZonekitZone with the t6 profile (32-bit:
// every pointer 4 bytes, the zone served below 4 GB), served to the live Black Ops II readers
// (GameBlackOps2.cpp's ReadXModel / ReadXAnim / ReadXMaterial). Without OpenAssetTools, whose path
// stays as --game bo2-oat. Images: streamed from the install's .ipak packages by their key, or the
// pixels the zone carries (a GfxImageLoadDef, DXGI formats). Sounds: each SndBank alias's asset by
// its id, in the zone's loaded assets or in the install's sound banks (sound\*.sabl / .sabs).

#include "CoDIWITranslator.h"
#include "Games/Treyarch/TreyarchSoundAliases.h"
#include "DiskGame.h"
#include "Games/Treyarch/TreyarchEffects.h"
#include "Games/Treyarch/TreyarchLevel.h"
#include "ExportWriters.h"
#include "GameBlackOps2.h"
#include "Games/IW3/Iw3Install.h"
#include "Games/IW3/LoadDefImage.h"
#include "IPAKCache.h"

#include "Image.h"
#include <miniz/miniz.h>
#include "ZonekitZone.h"
#include "Games/OAT/OatWeapons.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

namespace fs = std::filesystem;

namespace
{
    using DiskGame::Context;
    using DiskGame::ExportOptions;
    using DiskGame::Kind;
    using DiskGame::Totals;

    // GfxImage (0x50, Load_GfxImage 0x189c90): texture (its GfxImageLoadDef) at 0, width u16 at
    // 0x14, height 0x16, streamedParts[0].hash 0x28, streamedPartCount u8 0x3c, name 0x48, hash
    // 0x4c. A streamed image's .ipak key is hash << 32 | part hash. GfxImageLoadDef (0xc +
    // resourceSize): levelCount u8, flags u8, a DXGI_FORMAT at 4, resourceSize at 8, the pixels
    // from 0xc, the largest level first.
    constexpr int64_t ImageNameAt = 0x48, ImageWidth = 0x14, ImageHeight = 0x16, ImagePartHash = 0x28,
                      ImagePartCount = 0x3c, ImageHash = 0x4c;
    // RawFile (0xc): name 0, len 4, buffer 8 (len + 1 bytes); an .atr is {u32 size, raw deflate}
    constexpr int64_t RawLen = 4, RawBuffer = 8;
    // SndBank (0x1294, Load_SndBank 0x1830c0): aliasCount 4, alias lists 8 (SndAliasList 0x14:
    // head 8, count 0xc; SndAlias 0x60: assetId 0x10, assetFileName 0x14), streamAssetBank 0x20
    // and loadAssetBank 0x942 (zone 0, language 4: the bank file <zone>.<language>.sabs / .sabl).
    // Its loadedAssets (0x1264) entries are blank on disk: the game fills them from the .sabl.
    constexpr int64_t BankAliasCount = 4, BankAliases = 8, ListHead = 8, ListCount = 0xc, ListSize = 0x14,
                      AliasSize = 0x60, AliasAssetId = 0x10, AliasFile = 0x14, StreamBank = 0x20, LoadBank = 0x942;
    // SndAssetBankEntry (0x14): id, size, offset, frameCount, frameRateIndex u8, channelCount u8,
    // looping u8, format u8 (0 PCM s16, 8 FLAC)
    constexpr uint32_t FrameRates[] = { 8000, 12000, 16000, 24000, 32000, 44100, 48000, 96000, 192000 };

    struct SoundEntry
    {
        uint32_t Size = 0, Offset = 0;
        uint8_t Rate = 0, Channels = 0, Format = 0;
        std::string File;   // the bank file
    };

    // A sound bank file of the install (2UX#, version 14): its entries by id and its dependencies
    bool ReadBank(const fs::path& P, std::map<uint32_t, SoundEntry>& Out, std::vector<std::string>& Depends)
    {
        std::ifstream F(P, std::ios::binary);
        if (!F) return false;
        std::vector<uint8_t> H(0x248);
        F.read(reinterpret_cast<char*>(H.data()), (std::streamsize)H.size());
        if (F.gcount() != (std::streamsize)H.size() || ExportNaming::At<uint32_t>(H, 0) != 0x23585532 ||
            ExportNaming::At<uint32_t>(H, 8) != 0x14)
            return false;
        const uint32_t Count = ExportNaming::At<uint32_t>(H, 0x14), DepCount = ExportNaming::At<uint32_t>(H, 0x18),
                       DepSize = ExportNaming::At<uint32_t>(H, 0x10);
        for (uint32_t K = 0; DepSize && K < DepCount && (K + 1) * DepSize <= 512; K++)
        {
            const char* D = reinterpret_cast<const char*>(H.data() + 0x48 + K * DepSize);
            std::string Name(D, strnlen(D, DepSize));
            if (!Name.empty()) Depends.push_back(Name);
        }
        std::vector<uint8_t> E((size_t)Count * 0x14);
        F.seekg((std::streamoff)ExportNaming::At<int64_t>(H, 0x28));
        F.read(reinterpret_cast<char*>(E.data()), (std::streamsize)E.size());
        if (F.gcount() != (std::streamsize)E.size()) return false;
        for (uint32_t K = 0; K < Count; K++)
        {
            SoundEntry S;
            S.Size = ExportNaming::At<uint32_t>(E, K * 0x14 + 4);
            S.Offset = ExportNaming::At<uint32_t>(E, K * 0x14 + 8);
            S.Rate = E[K * 0x14 + 0x10], S.Channels = E[K * 0x14 + 0x11], S.Format = E[K * 0x14 + 0x13];
            S.File = P.string();
            Out.emplace(ExportNaming::At<uint32_t>(E, K * 0x14), S);
        }
        return true;
    }

    // Every bank of the install's sound folder, entries by id: a sound the zone's own banks do not
    // hold is in another zone's (a loaded asset the zone's blank entries name), read once per install
    std::mutex AllBanksLock;
    std::map<std::string, std::shared_ptr<const std::map<uint32_t, SoundEntry>>> AllBanks;

    std::shared_ptr<const std::map<uint32_t, SoundEntry>> AllBanksOf(const std::string& Install)
    {
        std::lock_guard<std::mutex> G(AllBanksLock);
        auto& P = AllBanks[Install];
        if (!P)
        {
            auto M = std::make_shared<std::map<uint32_t, SoundEntry>>();
            std::error_code Ec;
            std::vector<fs::path> Files;
            for (const auto& E : fs::directory_iterator(fs::path(Install) / "sound", Ec))
            {
                const std::string Ext = ExportNaming::Lower(E.path().extension().string());
                if (Ext == ".sabl" || Ext == ".sabs") Files.push_back(E.path());
            }
            std::sort(Files.begin(), Files.end());
            for (const auto& F : Files)
            {
                std::vector<std::string> Unused;
                ReadBank(F, *M, Unused);
            }
            P = M;
        }
        return P;
    }

    std::vector<uint8_t> WavHeader(uint32_t Channels, uint32_t Rate, uint32_t Bytes)
    {
        std::vector<uint8_t> H(44);
        const auto Put = [&H](size_t At, uint32_t V, int N) { for (int K = 0; K < N; K++) H[At + K] = (uint8_t)(V >> (8 * K)); };
        std::memcpy(H.data(), "RIFF", 4), Put(4, 36 + Bytes, 4), std::memcpy(H.data() + 8, "WAVEfmt ", 8);
        Put(16, 16, 4), Put(20, 1, 2), Put(22, Channels, 2), Put(24, Rate, 4), Put(28, Rate * Channels * 2, 4);
        Put(32, Channels * 2, 2), Put(34, 16, 2), std::memcpy(H.data() + 36, "data", 4), Put(40, Bytes, 4);
        return H;
    }

    // The install's .ipak packages, opened once per install and zone folder: a language folder's
    // own packages (zone\french\fr_base.ipak) first, then zone\all's (the first package holding a
    // key serves it)
    std::mutex PackagesLock;
    std::map<std::string, std::shared_ptr<IPAKCache>> Packages;

    std::shared_ptr<IPAKCache> PackagesOf(const std::string& Install, const std::string& Folder)
    {
        std::lock_guard<std::mutex> G(PackagesLock);
        auto& P = Packages[Install + "|" + Folder];
        if (!P)
        {
            P = std::make_shared<IPAKCache>();
            std::vector<fs::path> Files;
            std::error_code Ec;
            for (const std::string& Dir : { Folder, std::string("all") })
            {
                std::vector<fs::path> In;
                for (const auto& E : fs::directory_iterator(fs::path(Install) / "zone" / Dir, Ec))
                    if (ExportNaming::Lower(E.path().extension().string()) == ".ipak") In.push_back(E.path());
                std::sort(In.begin(), In.end());
                Files.insert(Files.end(), In.begin(), In.end());
                if (ExportNaming::Lower(Folder) == "all") break;
            }
            for (const auto& F : Files) P->LoadPackage(F.string());
            P->SetLoadedState();
        }
        return P;
    }

    // An alias's asset file without its raw/ or devraw/ root, as the sound kind writes it
    std::string WithoutRawRoot(std::string Rel)
    {
        for (const char* Prefix : { "raw/", "devraw/" })
            if (Rel.rfind(Prefix, 0) == 0) Rel = Rel.substr(std::strlen(Prefix));
        return Rel;
    }

    // A DXGI format's DDS format and the bytes of a W x H level (0: not one we write)
    uint64_t LevelOf(uint32_t Dxgi, uint32_t W, uint32_t H, ImageFormat& Fmt)
    {
        const uint64_t Blocks = (uint64_t)std::max(1u, (W + 3) / 4) * std::max(1u, (H + 3) / 4), Pixels = (uint64_t)W * H;
        switch (Dxgi)
        {
        case 71: Fmt = ImageFormat::DDS_BC1_UNORM; return Blocks * 8;
        case 72: Fmt = ImageFormat::DDS_BC1_SRGB; return Blocks * 8;
        case 74: Fmt = ImageFormat::DDS_BC2_UNORM; return Blocks * 16;
        case 75: Fmt = ImageFormat::DDS_BC2_SRGB; return Blocks * 16;
        case 77: Fmt = ImageFormat::DDS_BC3_UNORM; return Blocks * 16;
        case 78: Fmt = ImageFormat::DDS_BC3_SRGB; return Blocks * 16;
        case 80: Fmt = ImageFormat::DDS_BC4_UNORM; return Blocks * 8;
        case 81: Fmt = ImageFormat::DDS_BC4_SNORM; return Blocks * 8;
        case 83: Fmt = ImageFormat::DDS_BC5_UNORM; return Blocks * 16;
        case 84: Fmt = ImageFormat::DDS_BC5_SNORM; return Blocks * 16;
        case 28: Fmt = ImageFormat::DDS_Standard_R8G8B8A8; return Pixels * 4;
        case 29: Fmt = ImageFormat::DDS_Standard_R8G8B8A8_SRGB; return Pixels * 4;
        case 87: Fmt = ImageFormat::DDS_Standard_A8R8G8B8; return Pixels * 4;     // B8G8R8A8
        case 10: Fmt = ImageFormat::DDS_Standard_R16G16B16A16_FLOAT; return Pixels * 8;
        case 24: Fmt = ImageFormat::DDS_Standard_R10G10B10A2_UNORM; return Pixels * 4;
        case 61: Fmt = ImageFormat::DDS_Standard_R8_UNORM; return Pixels;
        case 65: Fmt = ImageFormat::DDS_Standard_A8_UNORM; return Pixels;
        default: return 0;
        }
    }

    // GfxWorld (0x404, the t6 tables): name 0, baseName 4, surfaceCount 0x10, skyBoxModel 0x24,
    // draw 0x18c (vertexCount +0x1c, vertexDataSize0 +0x20, vd0.data +0x24, indexCount +0x38,
    // u16 indices +0x3c), models 0x218/0x21c (64 bytes: surfaceCount 56, startSurfIndex 60), dpvs
    // 0x310 (smodelCount +0, surfaces +0x58 (0x50 each: srfTriangles_t vertexDataOffset0 0xc, a
    // byte offset into vd0, triCount u16 0x2a, baseIndex 0x2c; the material 0x30), smodelDrawInsts
    // +0x5c (0x98 each: cullDist, the placement 4, the model 0x38)). ComWorld (0x10):
    // primaryLightCount 8, primaryLights 0xc (0xc4 each). vd0's vertices: the T6 packed-vertex format
    // (OatInProcess.cpp, ExportMapGlbT6).
    TreyarchLevelZone::Level32Layout Level()
    {
        constexpr int64_t Draw = 0x18c, Dpvs = 0x310;
        TreyarchLevelZone::Level32Layout L;
        L.BaseName = 4, L.SurfaceCount = 0x10, L.SkyModel = 0x24;
        L.VertexCount = Draw + 0x1c, L.VertexBytes = Draw + 0x20, L.Vertices = Draw + 0x24;
        L.IndexCount = Draw + 0x38, L.Indices = Draw + 0x3c;
        L.Format = TreyarchLevelZone::VertexFormat::Packed;
        L.BrushCount = 0x218, L.Brushes = 0x21c, L.BrushSize = 64, L.BrushSurfaceCount = 56, L.BrushFirstSurface = 60;
        L.SModelCount = Dpvs, L.Surfaces = Dpvs + 0x58, L.SModels = Dpvs + 0x5c, L.SModelSize = 0x98, L.SModelPlacement = 4,
        L.SModelModel = 0x38;
        L.SurfaceSize = 0x50, L.SurfaceFirstVertex = 0xc, L.SurfaceTris = 0x2a, L.SurfaceBaseIndex = 0x2c, L.SurfaceMaterial = 0x30;
        L.LightCount = 8, L.Lights = 0xc, L.LightSize = 0xc4;
        // clipMap_t (0x14c, OAT's T6 struct, the t6 tables' col_map): the world's ClipInfo at 8
        // (ClipMaterial 0xc: name, surface, contents; brushes, a u16 count at 0x38); cbrush_t 0x60
        // as Black Ops'; cbrushside_t 0xc (plane, cflags, sflags); cmodel_t 0x4c: a ClipInfo pointer at
        // 0x1c (null in the zones seen: its brushes are the world table's), its leaf's leafBrushNode at 0x44
        Maps::ClipLayout& C = L.Clip;
        C.InfoAt = 8;
        C.Info.MaterialCount = 8, C.Info.Materials = 0xc, C.Info.BrushCount = 0x38, C.Info.Brushes = 0x3c;
        C.Info.LeafBrushNodeCount = 0x18, C.Info.LeafBrushNodes = 0x1c;
        C.MaterialSize = 0xc, C.MaterialName = 0, C.MaterialSurface = 4, C.MaterialContents = 8;
        C.BrushSize = 0x60, C.BrushMins = 0, C.BrushContents = 0xc, C.BrushMaxs = 0x10, C.BrushSideCount = 0x1c, C.BrushSides = 0x20;
        C.AxialContents = 0x24, C.AxialSurface = 0x3c;
        C.SideSize = 0xc, C.SidePlane = 0, C.SideContents = 4, C.SideSurface = 8;
        C.ModelCount = 0x90, C.Models = 0x94, C.ModelSize = 0x4c, C.ModelInfo = 0x1c, C.ModelLeafBrushNode = 0x44;
        C.VertexCount = 0x6c, C.Vertices = 0x70, C.TriangleCount = 0x74, C.Triangles = 0x78;   // u16 vertex numbers
        return L;
    }

    class Bo2Zone final : public TreyarchLevelZone
    {
    public:
        Bo2Zone(const std::string& Path, std::string InstallDir) : TreyarchLevelZone(Path), Install(std::move(InstallDir)), Weapons(Path)
        {
            // the weapon files and their stats table, through OpenAssetTools (Games/OAT/OatWeapons.h)
            Exports(Kind::Weapons, [this](const Context& C, Totals& T) { OatWeapons::ExportWeapons(Weapons, Name(), C, T); });
            Exports(Kind::WeaponStats, [this](const Context& C, Totals& T) { OatWeapons::ExportWeaponStats(Weapons, Name(), C, T, "Call of Duty: Black Ops II"); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
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
            // scriptparsetree {name, u32 len, buffer}: a GSCOBJ of the T6 VM, decompiled by gsc-tool
            Exports(Kind::Script, [this](const Context& C, Totals& T) {
                ExportScripts(C, T, { Scripts::Tool::GscTool, "t6", "pc", "c" },
                              { { "scriptparsetree", [this](int64_t Pos, Scripts::Script& S) {
                                     S.Compiled = true;
                                     return BytesAt(PointerAt(Pos + 8), At<uint32_t>(Image(), Pos + 4), S.Bytes);
                                 } } });
            });
            // Material: its techset, images and blend factors as JSON, its images by the image kind;
            // FxEffectDef (0x4c, elements 0x124) as JSON
            ExportsEffects(GameBlackOps2::ReadXMaterial, Treyarch::EffectLayout::Bo2);
        }
        ~Bo2Zone() override
        {
            Serve(false);
        }

    protected:
        const char* Profile() const override { return "t6"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("T6"); }
        // an asset pointer's back reference is resolved through the slot it names (the Ptr
        // loaders call DB_ConvertOffsetToAlias 0x19b7d0: *p = *(block + offset)); a data one is
        // the place itself (DB_ConvertOffsetToPointer 0x19b800)
        bool DerefAssetPointers() const override { return true; }
        VirtualGameSession::Handlers Handlers() override
        {
            Slots = ZoneStrings(Zone());
            return { SlotString, GameBlackOps2::LoadXImage };
        }
        void Served(bool On) override { Serve(On); }
        void Fixups() override { Serve(true); }
        std::string AssetName(Kind K, int64_t Pos) const override
        {
            return StringAt(PointerAt(K == Kind::Image ? Pos + ImageNameAt : Pos));
        }
        // The level (TreyarchLevel.h, Black Ops II's places above). A world surface's material
        // may be a composite of two layers, "*<id>(<path>:<path>)": its name is kept whole, as the
        // file part of the second path would name different composites alike
        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override
        {
            return ReadLevel(Scene, Models, Level(), [this](uint64_t Address) {
                XMaterial_t M = GameBlackOps2::ReadXMaterial(Address);
                const std::string Raw = StringAt(PointerAt(Off(Address)));
                if (!Raw.empty() && Raw[0] == '*') M.MaterialName = ExportNaming::SafeFileName(Raw);
                return M;
            });
        }

    private:
        std::string Install;
        OatWeapons::Source Weapons;
        std::shared_ptr<IPAKCache> Ipak;

        // the live image handler reads the .ipak through CoDAssets::GamePackageCache while the
        // zone is served; the previous cache comes back when it is not
        void Serve(bool On)
        {
            if (Install.empty()) return;
            ServePackages(On, [this] {
                if (!Ipak) Ipak = PackagesOf(Install, fs::path(FastFilePath()).parent_path().filename().string());
                return SharedPackages(Ipak);
            });
        }

        // The zone's script strings: count at 0x28, the list pointer at 0x2c (any
        // non-null value: present), u32 slots from 0x40, then the text of every -1 slot
        static std::vector<std::string> ZoneStrings(const std::vector<uint8_t>& Z)
        {
            const uint32_t Count = At<uint32_t>(Z, 0x28);
            if (At<uint32_t>(Z, 0x2c) == 0 || Count == 0 || 0x40 + (size_t)Count * 4 > Z.size()) return {};
            return InlineStrings(Z, 0x40, Count, 4, 0xFFFFFFFFu);
        }

        // Every image the zone names (its own and its materials'): streamed ones from the .ipak
        // packages, the others from the pixels the zone carries. A "," name is a reference.
        void ExportImages(const Context& C, Totals& T)
        {
            ExportNamedImages(C, T, [this](const std::string&, int64_t Pos) { return Decode(Pos); },
                              "neither in the .ipak packages nor in the zone");
        }

        std::string IndexKey() const override { return ZonekitZone::IndexKey() + "_images1"; }
        // the images this zone holds whole, for the install index (a texture another zone names)
        void Defines(std::vector<DiskGame::AssetId>& Out) const override
        {
            ZonekitZone::Defines(Out);
            DefineImages(Out);
        }

        std::unique_ptr<XImageDDS> Decode(int64_t Pos)
        {
            const auto& Z = Image();
            if (At<uint8_t>(Z, Pos + ImagePartCount) > 0)
            {
                if (Install.empty()) return nullptr;
                if (!Ipak) Ipak = PackagesOf(Install, fs::path(FastFilePath()).parent_path().filename().string());
                const uint64_t Key = (uint64_t)At<uint32_t>(Z, Pos + ImageHash) << 32 | At<uint32_t>(Z, Pos + ImagePartHash);
                uint32_t Size = 0;
                auto Data = Ipak->ExtractPackageObject(Key, Size);
                if (!Data) return nullptr;
                Counter("images_ipak")++;
                return CoDIWITranslator::TranslateIWI(Data, Size);
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
            const uint64_t Level = LevelOf(Format, W, H, Fmt);
            if (Level == 0)
            {
                Counter("images_in_zone_format_" + std::to_string(Format))++;
                return nullptr;
            }
            if (W == 0 || H == 0 || Level > Size || Def + 0xc + (int64_t)Level > (int64_t)Z.size()) return nullptr;
            return LoadDefImage::Dds(W, H, Fmt, Z.data() + Def + 0xc, Level);
        }

        // Every alias of every SndBank with an asset file: its data by asset id, in the bank files the
        // SndBank names and the banks they depend on, else in any bank of the install
        void ExportSounds(const Context& C, Totals& T)
        {
            ExportSoundFiles(C, T);
            if (C.OnSoundHeader || !SoundAliasCsv::Wanted(C)) return;   // (a dry run writes no file)
            // an alias's asset file as ExportSoundFiles writes it: without raw/ or devraw/, .flac or .wav
            const fs::path Root = C.KindDir(Kind::Sound);
            const std::string Format = ExportWriters::SoundFormatSetting();
            const auto File = [&](const std::string& Asset) -> std::string {
                const std::string Rel = WithoutRawRoot(ExportNaming::ForwardSlashes(Asset));
                if (Rel.empty()) return Rel;
                if (Format != "WAV") return Rel + ExportWriters::SoundExtension(Format);
                std::error_code Ec;
                return Rel + (fs::is_regular_file(Root / fs::path(Rel + ".flac"), Ec) ? ".flac" : ".wav");
            };
            SoundAliasCsv::Rows Rows;
            for (const auto& I : Walk().Instances)
                if (OwnerIs(I, "sound") && I.Parent < 0) Treyarch::ReadBo2SoundBank(AliasView(), I.Pos, File, Rows);
            SoundAliasCsv::Write(C, T, Name(), Rows);
        }

        void ExportSoundFiles(const Context& C, Totals& T)
        {
            auto& Kt = T.Kinds[Kind::Sound];
            const auto& Z = Image();
            if (C.OnSoundHeader) T.Counters["sound_headers"] = 1;
            std::map<uint32_t, SoundEntry> Entries;
            std::map<std::string, uint32_t> Files;   // asset file -> asset id
            std::vector<std::string> Banks;
            for (const auto& I : Walk().Instances)
            {
                if (!OwnerIs(I, "sound") || I.Parent >= 0) continue;
                const int64_t Lists = Off(PointerAt(I.Pos + BankAliases));
                const uint32_t Lcount = At<uint32_t>(Z, I.Pos + BankAliasCount);
                for (uint32_t L = 0; Lists >= 0 && L < Lcount; L++)
                {
                    const int64_t List = Lists + (int64_t)L * ListSize;
                    const int64_t Head = Off(PointerAt(List + ListHead));
                    const uint32_t Count = At<uint32_t>(Z, List + ListCount);
                    for (uint32_t K = 0; Head >= 0 && K < Count && K < 4096; K++)
                    {
                        const int64_t A = Head + (int64_t)K * AliasSize;
                        const uint32_t Id = At<uint32_t>(Z, A + AliasAssetId);
                        const std::string F = ExportNaming::ForwardSlashes(StringAt(PointerAt(A + AliasFile)));
                        if (Id != 0 && !F.empty()) Files.emplace(F, Id);
                    }
                }
                for (const auto& [At_, Ext] : { std::pair<int64_t, const char*>{ StreamBank, ".sabs" }, { LoadBank, ".sabl" } })
                {
                    const std::string Zn = StringAt(PointerAt(I.Pos + At_)), Lang = StringAt(PointerAt(I.Pos + At_ + 4));
                    if (!Zn.empty()) Banks.push_back(Zn + (Lang.empty() ? "" : "." + Lang) + Ext);
                }
            }
            if (Files.empty()) return;
            if (!Install.empty())
            {
                std::set<std::string> Opened;
                while (!Banks.empty())
                {
                    const std::string B = Banks.back();
                    Banks.pop_back();
                    if (!Opened.insert(ExportNaming::Lower(B)).second) continue;
                    std::map<uint32_t, SoundEntry> Got;
                    std::vector<std::string> Depends;
                    if (!ReadBank(fs::path(Install) / "sound" / B, Got, Depends))
                    {
                        Counter("sound_banks_missing")++;
                        continue;
                    }
                    Counter("sound_banks")++;
                    Entries.insert(Got.begin(), Got.end());
                    Banks.insert(Banks.end(), Depends.begin(), Depends.end());
                }
            }
            const fs::path Root = C.KindDir(Kind::Sound);
            size_t N = 0;
            std::map<std::string, std::ifstream> Open;
            std::shared_ptr<const std::map<uint32_t, SoundEntry>> Others;
            for (const auto& [F, Id] : Files)
            {
                if (!C.Advance(N++, Files.size(), F)) return;
                if (!C.Wants(Kind::Sound, F)) continue;
                Kt.Seen++;
                const auto It = Entries.find(Id);
                const SoundEntry* Found = It == Entries.end() ? nullptr : &It->second;
                if (!Found && !Install.empty())
                {
                    if (!Others) Others = AllBanksOf(Install);
                    const auto O = Others->find(Id);
                    if (O != Others->end())
                    {
                        Found = &O->second;
                        Counter("sounds_other_banks")++;
                    }
                }
                if (!Found)
                {
                    Counter("sounds_without_data")++;
                    T.Fail(Name(), Kind::Sound, F, "in no sound bank of the install");
                    continue;
                }
                const SoundEntry& S = *Found;
                if ((S.Format != 0 && S.Format != 8) || (S.Format == 0 && S.Rate >= std::size(FrameRates)))
                {
                    Counter("sounds_format_" + std::to_string(S.Format))++;
                    T.Fail(Name(), Kind::Sound, F, "format " + std::to_string(S.Format));
                    continue;
                }
                if (C.OnSoundHeader)
                {
                    // a dry run: the entry as its bank's table says - PCM behind the WAV header
                    // written for it, or a FLAC file kept as it is under WAV
                    DiskGame::SoundHeader H;
                    H.Name = WithoutRawRoot(F);
                    H.KeptUnderWav = true;
                    H.Channels = S.Channels;
                    H.Rate = S.Rate < std::size(FrameRates) ? FrameRates[S.Rate] : 0;
                    if (S.Format == 0)
                    {
                        H.Is = DiskGame::SoundHeader::Form::Wav;
                        H.Stored = WavHeader(S.Channels, FrameRates[S.Rate], S.Size).size() + (uint64_t)S.Size;
                        if (S.Channels) H.Frames = S.Size / (2ull * S.Channels);
                    }
                    else
                    {
                        H.Is = DiskGame::SoundHeader::Form::Flac;
                        H.Stored = S.Size;
                    }
                    C.OnSoundHeader(H);
                    Kt.Written++;
                    continue;
                }
                std::vector<uint8_t> Data(S.Size);
                auto& Fs = Open[S.File];
                if (!Fs.is_open()) Fs.open(S.File, std::ios::binary);
                Fs.clear();
                Fs.seekg(S.Offset);
                Fs.read(reinterpret_cast<char*>(Data.data()), (std::streamsize)S.Size);
                if (Fs.gcount() != (std::streamsize)S.Size)
                {
                    T.Fail(Name(), Kind::Sound, F, "data not read");
                    continue;
                }
                // the asset file without its raw/ or devraw/ root, the format's extension added
                const std::string Rel = WithoutRawRoot(F);
                const std::string Own = S.Format == 8 ? ".flac" : ".wav";
                const std::string Format = ExportWriters::SoundFormatSetting();
                const fs::path Out = Root / fs::path(Rel + ExportWriters::SoundExtension(Format, Own, true)).make_preferred();
                std::vector<uint8_t> File;
                if (S.Format == 0)
                {
                    File = WavHeader(S.Channels, FrameRates[S.Rate], S.Size);
                    File.insert(File.end(), Data.begin(), Data.end());
                }
                else
                    File.swap(Data);
                std::string Why;
                if (ExportWriters::WriteSound(ExportWriters::SoundSource::OfFile(File.data(), File.size(), Own, true), Out, Format, &Why))
                {
                    Kt.Written++;
                }
                else
                    T.Fail(Name(), Kind::Sound, F, Why);
            }
        }

        // RawFile: an .atr (animtree) buffer is {u32 inflated size, raw deflate}; any other file is
        // its bytes
        bool RawfileBytes(int64_t Pos, const std::string& Nm, std::vector<uint8_t>& Bytes)
        {
            const auto& Z = Image();
            const uint32_t Len = At<uint32_t>(Z, Pos + RawLen);
            const int64_t Data = Off(PointerAt(Pos + RawBuffer));
            bool Ok = Len == 0 || (Data >= 0 && (uint64_t)Data + Len <= Z.size());
            if (Ok && Len)
            {
                const std::string Ext = ExportNaming::Lower(fs::path(Nm).extension().string());
                if (Ext == ".atr" && Len > 4)
                {
                    size_t Got = 0;
                    void* Out = tinfl_decompress_mem_to_heap(Z.data() + Data + 4, Len - 4, &Got, 0);
                    Ok = Out != nullptr;
                    if (Ok)
                    {
                        Bytes.assign((uint8_t*)Out, (uint8_t*)Out + Got);
                        mz_free(Out);
                        Counter("rawfiles_inflated")++;
                    }
                }
                else
                    Bytes.assign(Z.data() + Data, Z.data() + Data + Len);
            }
            return Ok;
        }
    };


    class Bo2Game final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "bo2"; }
        const char* Title() const override { return "Call of Duty: Black Ops II"; }
        const char* Engine() const override { return "T6"; }
        std::vector<std::string> Aliases() const override { return { "t6" }; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<Bo2Zone>(std::string(), std::string()); }
        // the rich presence strings name every level: PRESENCE_ZM_HIGHRISE "Die Rise",
        // PRESENCE_MP_NUKETOWN2020, PRESENCE_ANGOLA; the survival modes their ZMUI_ name
        const LevelNaming* LevelNames() const override
        {
            static const LevelNaming Naming{ { "??_code_post_gfx_mp", "??_code_post_gfx_zm", "??_patch", "??_patch_zm" },
                                             { { "so_*", { "ZMUI_{REST}" } },
                                               { "*", { "PRESENCE_{NAME}", "MENU_LEVEL_{NAME}_CAPS" } } } };
            return &Naming;
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const ExportOptions& Options, std::string& Error) const override
        {
            auto Zone = std::make_unique<Bo2Zone>(Path, Iw3Family::InstallOf(Path, Options.Install));
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const Bo2Game Game;
    const DiskGame::Registration Registered(Game);
}
