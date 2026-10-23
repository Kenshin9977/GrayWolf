#include "stdafx.h"

// Call of Duty Online on the disk template: a ZonekitZone with the codol profile (32-bit), served
// to the CODOL readers (Games/CODOL/CodolReaders.h) through its live game (Games/CODOL/LiveGame.cpp). The zones are
// the sealed fast files of the install's .ifs archives (IfsArchive.h, SealedFastFile.h), or loose
// ones (ifs-extract's layout); images are in the zone (a GfxImageLoadDef) or .iwi files of the
// archives (FileCrypt.h).

#include "CoDIWITranslator.h"
#include "DiskGame.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "Games/CODOL/CodolReaders.h"
#include "Games/CODOL/CodolWeapons.h"
#include "Games/CODOL/FileCrypt.h"
#include "Games/CODOL/IfsArchive.h"
#include "Games/CODOL/SealedFastFile.h"
#include "Games/IW/Iw32Level.h"
#include "Games/IW/IWSoundAliases.h"

#include "Image.h"
#include "ZonekitZone.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <regex>
#include <set>

namespace fs = std::filesystem;

namespace
{
    using DiskGame::Context;
    using DiskGame::ExportOptions;
    using DiskGame::Kind;
    using DiskGame::Totals;

    // where the archives keep the fast files, and what ifs-extract writes
    const char* const ZoneFolder = "client_shipretail/chinese";

    // An install's archives, indexed once per install (every zone of a run)
    std::mutex InstallsLock;
    std::map<std::string, std::shared_ptr<Ifs::Install>> Installs;

    std::shared_ptr<Ifs::Install> InstallOf(const std::string& Dir, std::string& Error)
    {
        std::lock_guard<std::mutex> G(InstallsLock);
        auto& I = Installs[ExportNaming::Lower(Dir)];
        if (!I)
        {
            auto Opened = std::make_shared<Ifs::Install>();
            if (!Opened->Open(Dir, Error)) return nullptr;
            I = std::move(Opened);
        }
        return I;
    }

    bool IsInstall(const std::string& Dir)
    {
        std::error_code Ec;
        return fs::is_directory(Ifs::Utf8Path(Dir) / "IIPS" / "IIPSDownload", Ec);
    }

    // <install>/client_shipretail/chinese/<zone>.ff: the install is the folder above
    // client_shipretail, when the path has one
    std::string InstallFromZone(const std::string& Zone, const std::string& Given)
    {
        if (!Given.empty()) return Given;
        const std::string Lower = ExportNaming::Lower(Zone);
        std::string Key = Lower;
        std::replace(Key.begin(), Key.end(), '\\', '/');
        const auto At = Key.rfind(std::string("/") + ZoneFolder + "/");
        return At == std::string::npos ? std::string() : Zone.substr(0, At);
    }

    // The install's zone names (<zone>), sorted
    std::vector<std::string> ZoneNames(const Ifs::Install& I, const std::string& Glob)
    {
        const auto Pattern = Ifs::GlobRegex(Glob);
        std::vector<std::string> Out;
        const std::string Prefix = std::string(ZoneFolder) + "/";
        for (const auto& [Key, L] : I.Files())
        {
            if (Key.size() < 4 || Key.compare(Key.size() - 3, 3, ".ff") != 0 || Key.compare(0, Prefix.size(), Prefix) != 0) continue;
            const std::string Name = Key.substr(Prefix.size(), Key.size() - Prefix.size() - 3);
            if (std::regex_match(Name, Pattern)) Out.push_back(Name);
        }
        std::sort(Out.begin(), Out.end());
        return Out;
    }

    class CodolZone final : public Iw32LevelZone
    {
    public:
        CodolZone(const std::string& Path, std::string InstallDir) : Iw32LevelZone(Path), Install(std::move(InstallDir))
        {
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
            // weapon (0x101c, the WeaponDef itself) and weaponattach (0xc), named by the string at 0
            // (CodolWeapons.h)
            ExportsStructDumps(Kind::Weapons, { { "weapon", 0, IwWeaponDefs::DumpFields(Codol::WeaponLayout()), -1 }, { "weaponattach", 0, {}, -1 } });
            Exports(Kind::WeaponStats, [this](const Context& C, Totals& T) { ExportWeaponStats(C, T); });
            // rawfile (0x10) {name, u32 compressed size 4, size 8, buffer 0xc}: zlib when compressed,
            // else size + 1 bytes read; stringtable (0x10) {name, i32 columns 4, rows 8, cells 0xc of
            // 8 bytes {string, hash}}, its text GBK, written as UTF-8
            Exports(Kind::Rawfile, [this](const Context& C, Totals& T) {
                ExportRawfiles(C, T, { { "rawfile", [this](int64_t Pos, std::string&, std::vector<uint8_t>& Bytes) {
                                            return RawBytes(Pos, { 8, 0xc, 4 }, Bytes);
                                        } } });
            });
            Exports(Kind::Data, [this](const Context& C, Totals& T) {
                ExportStringTables(C, T, "stringtable", [this](int64_t Pos, uint32_t& Columns, uint32_t& Rows, std::vector<std::string>& Cells) {
                    if (!StringTableCells(Pos, { 4, 8, 0xc, 8 }, Columns, Rows, Cells)) return false;
                    for (auto& Cell : Cells) Cell = ExportWriters::Utf8(Cell, LocalizeCodePage());
                    return true;
                });
            });
        }
        ~CodolZone() override
        {
            if (IsServed()) Serve(false);
        }

    protected:
        const char* Profile() const override { return "codol"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("CODOL"); }
        // an asset pointer's back reference names the slot holding it (the Ptr loaders read
        // *(block + offset), 0x493a78)
        bool DerefAssetPointers() const override { return true; }
        unsigned LocalizeCodePage() const override { return 936; }   // GBK, the Chinese release's
        VirtualGameSession::Handlers Handlers() override
        {
            Slots = ZoneStrings(Zone());
            return { SlotString, DiskImage };
        }
        void Served(bool On) override { Serve(On); }
        void Fixups() override { Serve(true); }

        // the zone the static handlers and the streamed surfaces read (main/models/<name> of its
        // install's archives)
        void Serve(bool On)
        {
            if (!On)
            {
                Codol::ServeStreamedSurfaces(nullptr);
                return;
            }
            Codol::ServeStreamedSurfaces([this](const std::string& File, std::vector<uint8_t>& Data) {
                std::string Error;
                const auto Archives = Archives_(Error);
                const std::string Key = "main/models/" + File;
                const auto* L = Archives ? Archives->Find(Key) : nullptr;
                if (L == nullptr || !Archives->Read(*L, Data, Error)) return false;
                Codol::UnwrapFile(Key, Data);
                Counter("models_streamed_surfaces")++;
                return true;
            });
        }
        std::string AssetName(Kind K, int64_t Pos) const override
        {
            return StringAt(PointerAt(K == Kind::Image ? Pos + Codol::ImageName : Pos));
        }
        std::string IndexKey() const override { return ZonekitZone::IndexKey() + "|codol images 1"; }

        // The level (the codol tables, MW2's 32-bit shapes): GfxWorld (0x64c) surface count 0x10,
        // skies 0x18 (GfxSky 0x10, its image at 8), GfxWorldDraw at 0x54 (vertexCount +0x34, 44-byte
        // vertices +0x38, indexCount +0x4c, u16 indices +0x50), brush models 0xe0/0xe4 (60 bytes: u16
        // surface count at 52, first surface at 54), GfxWorldDpvsStatic at 0x5a0 (surfaces +0x54,
        // 0x18 each, material at 0x10; static model draw records +0x5c, 0x4c each: the placement at
        // 0, the model at 0x34); clipMap_t (col_map_mp, 0x100) dynamic entities u16 0xc4/0xc6, lists
        // 0xc8/0xcc, 0x5c each; ComPrimaryLight 0x44, colour at 4
        MapLayout Map() const override
        {
            constexpr int64_t Draw = 0x54, Dpvs = 0x5a0;
            MapLayout M{ Draw + 0x4c, Draw + 0x50, 0x10, -1, -1, Draw + 0x34, Draw + 0x38, 0xe0, 0xe4, 60, 52, 54, 2,
                         Dpvs, Dpvs + 0x54, 0x18, 0x10, Dpvs + 0x5c, 0x4c, 0xc4, 0xc8, 0x5c, 0x44, 4 };
            M.SModelPlacement = 0, M.SModelModel = 0x34;
            M.Skies = 0x18, M.SkyImageInSky = 8;
            return M;
        }
        XMaterial_t Material(uint64_t Address) const override { return Codol::ReadXMaterial(Address); }
        // clipMap_t's brushes (the codol tables, MW3's 32-bit ClipInfo): the world's ClipInfo at 8
        // (0x40: ClipMaterial 0xc, name, surface, contents at 8/0xc; leaf brush nodes 0x20/0x24;
        // brushes u16 0x30, records 0x34, Bounds 0x38, contents 0x3c); cbrush_t 0x24 (u16 numsides,
        // sides at 4, axial material numbers at 0xc); cbrushside_t 8 (plane, u16 material number);
        // brush models u32 0x90, cmodel_t 0x48 at 0x94 (its ClipInfo pointer at 0x1c, its leaf's leafBrushNode at 0x44)
        int64_t TriggersAt() const override { return 0xc; }   // MapEnts' MapTriggers (the codol tables)
        Maps::ClipLayout ClipBrushes() const override
        {
            Maps::ClipLayout C;
            C.InfoAt = 8;
            C.Info.MaterialCount = 8, C.Info.Materials = 0xc, C.Info.BrushCount = 0x30, C.Info.Brushes = 0x34;
            C.Info.BrushBounds = 0x38, C.Info.BrushContents = 0x3c, C.Info.LeafBrushNodeCount = 0x20, C.Info.LeafBrushNodes = 0x24;
            C.MaterialSize = 0xc, C.MaterialName = 0, C.MaterialSurface = 4, C.MaterialContents = 8;
            C.BrushSize = 0x24, C.BrushSideCount = 0, C.BrushSideCountBytes = 2, C.BrushSides = 4, C.AxialMaterial = 0xc;
            C.SideSize = 8, C.SidePlane = 0, C.SideMaterial = 4, C.SideMaterialBytes = 2;
            C.ModelCount = 0x90, C.Models = 0x94, C.ModelSize = 0x48, C.ModelInfo = 0x1c, C.ModelLeafBrushNode = 0x44;
            C.VertexCount = 0x64, C.Vertices = 0x68, C.TriangleCount = 0x6c, C.Triangles = 0x70;   // u16 vertex numbers
            return C;
        }
        void Defines(std::vector<DiskGame::AssetId>& Out) const override
        {
            ZonekitZone::Defines(Out);
            DefineImages(Out);
        }

        // A loose fast file when there is one, else the install's archives
        bool LoadBytes(std::vector<uint8_t>& Out, std::string& Error) override
        {
            std::error_code Ec;
            if (fs::exists(Ifs::Utf8Path(FastFilePath()), Ec)) return ZonekitZone::LoadBytes(Out, Error);
            const auto Archives = Archives_(Error);
            if (!Archives) return false;
            const auto* L = Archives->Find(std::string(ZoneFolder) + "/" + Name() + ".ff");
            std::vector<uint8_t> File;
            if (L == nullptr)
            {
                Error = "no " + Name() + ".ff in the install's archives";
                return false;
            }
            return Archives->Read(*L, File, Error) && Codol::DecryptFastFile(File, Out, Error);
        }

    private:
        std::string Install;
        std::shared_ptr<Ifs::Install> Opened;

        // The weaponstats kind: the weapons as weapon files, the weaponattach assets as attachment files,
        // the zone's strings and its weapon attachment table (code_post_gfx_mp's) to WeaponFiles
        void ExportWeaponStats(const Context& C, Totals& T)
        {
            IwWeaponStats::ZoneView V;
            V.Image = &Image();
            V.Walk = &Walk();
            V.Off = [this](uint64_t A) { return Off(A); };
            V.Text = [this](int64_t Field) { return TextAt(Field); };
            const IwWeaponDefs::Layout L = Codol::WeaponLayout();
            auto Weapons = IwWeaponDefs::Weapons(L, V);
            for (auto& A : Codol::AttachmentFiles(V)) Weapons.push_back(std::move(A));
            std::vector<std::pair<std::string, std::string>> S;
            Strings(S);
            // (GBK, every one: 16 weapon names, the Desert Eagle's C9 B3 D3 A5 among them, are GBK bytes
            // that also read as UTF-8)
            for (auto& [Key, Text] : S) Text = ExportWriters::Utf8(Text, LocalizeCodePage(), true);
            std::map<std::string, std::string> Tables;
            for (const auto& I : Walk().Instances)
            {
                if (I.Parent >= 0 || !OwnerIs(I, "stringtable") || ExportNaming::Lower(AssetName(Kind::Data, I.Pos)) != Codol::AttachmentTable) continue;
                uint32_t Columns = 0, Rows = 0;
                std::vector<std::string> Cells;
                if (!StringTableCells(I.Pos, { 4, 8, 0xc, 8 }, Columns, Rows, Cells) || Cells.size() < (size_t)Columns * Rows) continue;
                std::string Csv;
                for (uint32_t R_ = 0; R_ < Rows; R_++)
                {
                    for (uint32_t K = 0; K < Columns; K++) Csv += (K ? "," : "") + Cells[(size_t)R_ * Columns + K];
                    Csv += "\n";
                }
                Tables[Codol::AttachmentTable] = Csv;
            }
            WeaponFiles::ExportStats(Weapons, S, Name(), C, T, IwWeaponDefs::StatsNotes(L), Tables);
        }

        std::shared_ptr<Ifs::Install> Archives_(std::string& Error)
        {
            if (!Opened && !Install.empty()) Opened = InstallOf(Install, Error);
            if (!Opened && Error.empty()) Error = "no install: --install <the Call of Duty Online folder>";
            return Opened;
        }

        // The textures a model names, for the live code: by name, as the image kind decodes them
        static std::unique_ptr<XImageDDS> DiskImage(const XImage_t& Image)
        {
            CodolZone* Current = ServedZone<CodolZone>();
            return Current ? Current->FromArchives(Image.ImageName) : nullptr;
        }

        // The script strings: count at 0x28, the list pointer at 0x2c, u32
        // slots from 0x38, then the text of every -1 slot
        static std::vector<std::string> ZoneStrings(const std::vector<uint8_t>& Z)
        {
            const uint32_t Count = At<uint32_t>(Z, 0x28);
            if (At<uint32_t>(Z, 0x2c) == 0 || Count == 0 || 0x38 + (size_t)Count * 4 > Z.size()) return {};
            return InlineStrings(Z, 0x38, Count, 4, 0xFFFFFFFFu);
        }

        // An .iwi of the archives by image name: hires/images first (what the game loads on
        // high settings), else main/images
        std::unique_ptr<XImageDDS> FromArchives(const std::string& ImageName)
        {
            std::string Error;
            const auto Archives = Archives_(Error);
            if (!Archives || ImageName.empty()) return nullptr;
            for (const char* Folder : { "hires/images/", "main/images/" })
            {
                const std::string Key = Folder + ExportNaming::Lower(ImageName) + ".iwi";
                const auto* L = Archives->Find(Key);
                if (L == nullptr) continue;
                std::vector<uint8_t> Data;
                if (!Archives->Read(*L, Data, Error)) continue;
                Codol::UnwrapFile(Key, Data);
                auto Buffer = std::make_unique<uint8_t[]>(Data.size());
                std::memcpy(Buffer.get(), Data.data(), Data.size());
                auto Result = CoDIWITranslator::TranslateIWI(Buffer, (uint32_t)Data.size());
                if (Result)
                {
                    Counter(Folder[0] == 'h' ? "images_hires" : "images_archives")++;
                    return Result;
                }
            }
            return nullptr;
        }

        // GfxImage (0x34): mapType u8 at 4 (5 a cube), width u16 at 0x20, height at 0x22
        static constexpr int64_t ImageMapType = 0x4, ImageWidth = 0x20, ImageHeight = 0x22;

        // The pixels a GfxImage carries in the zone: the largest level of each face
        std::unique_ptr<XImageDDS> LoadDef(int64_t Pos)
        {
            const int64_t Def = Off(PointerAt(Pos + Codol::ImageLoadDef));
            if (Def < 0) return nullptr;
            const auto& Z = Image();
            const uint32_t Size = At<uint32_t>(Z, Def + Codol::LoadDefSize);
            const uint32_t W = At<uint16_t>(Z, Pos + ImageWidth), H = At<uint16_t>(Z, Pos + ImageHeight);
            if (Size == 0 || W == 0 || H == 0) return nullptr;
            ImageFormat Fmt;
            uint64_t Level = 0;
            const uint64_t Blocks = (uint64_t)std::max(1u, (W + 3) / 4) * std::max(1u, (H + 3) / 4);
            bool Swap = false;
            switch (At<uint32_t>(Z, Def + Codol::LoadDefFormat))
            {
            case 0x31545844: Fmt = ImageFormat::DDS_BC1_UNORM; Level = Blocks * 8; break;    // DXT1
            case 0x33545844: Fmt = ImageFormat::DDS_BC2_UNORM; Level = Blocks * 16; break;   // DXT3
            case 0x35545844: Fmt = ImageFormat::DDS_BC3_UNORM; Level = Blocks * 16; break;   // DXT5
            case 0x32495441: Fmt = ImageFormat::DDS_BC5_UNORM; Level = Blocks * 16; break;   // ATI2
            case 21: case 22: Fmt = ImageFormat::DDS_Standard_A8R8G8B8; Level = (uint64_t)W * H * 4; Swap = true; break;
            case 28: case 50: Fmt = ImageFormat::DDS_Standard_A8_UNORM; Level = (uint64_t)W * H; break;   // A8, L8
            default:
                Counter("images_in_zone_unknown_format")++;
                return nullptr;
            }
            const bool Cube = At<uint8_t>(Z, Pos + ImageMapType) == 5;
            const uint64_t Faces = Cube ? 6 : 1;
            if (Level * Faces > Size || Def + Codol::LoadDefData + (int64_t)Size > (int64_t)Z.size()) return nullptr;
            auto Result = std::make_unique<XImageDDS>();
            auto Buffer = new int8_t[Image::GetMaximumDDSHeaderSize() + Level * Faces];
            uint32_t Header = 0;
            Image::WriteDDSHeaderToStream(Buffer, W, H, 1, Fmt, Header, Cube);
            for (uint64_t F = 0; F < Faces; F++)
                std::memcpy(Buffer + Header + F * Level, Z.data() + Def + Codol::LoadDefData + F * (Size / Faces), (size_t)Level);
            if (Swap)
                for (uint64_t P = 0; P + 3 < Level * Faces; P += 4) std::swap(Buffer[Header + P], Buffer[Header + P + 2]);
            Result->DataBuffer = Buffer;
            Result->DataSize = (uint32_t)(Header + Level * Faces);
            Counter("images_in_zone")++;
            return Result;
        }

        std::unique_ptr<XImageDDS> Decode(const std::string& ImageName, int64_t Pos)
        {
            if (auto Inline = LoadDef(Pos)) return Inline;
            return FromArchives(ImageName);
        }

        // Every image the zone names (its own and its materials'). A "," name is a reference.
        void ExportImages(const Context& C, Totals& T)
        {
            ExportNamedImages(C, T, [this](const std::string& N, int64_t Pos) { return Decode(N, Pos); },
                              "neither in the zone nor in the archives");
        }

        // snd_alias_list_t (0xc): aliases at 4, count at 8; snd_alias_t (0x6c): SoundFile at 0x14;
        // SoundFile (0xc): type u8 at 0 (1 loaded), then the LoadedSound, or the streamed file's
        // directory (4) and name (8) under main/sound/ in the archives. LoadedSound (0x30): name
        // 0, its sound info from 4: format 4, data size 0xc, rate 0x10, bits 0x14, channels 0x18,
        // block size 0x24; the data at 0x2c.
        static constexpr int64_t ListAliases = 4, ListCount = 8, AliasSize = 0x6c, AliasFile = 0x14;

        static void Riff(std::vector<uint8_t>& Out, uint16_t Format, uint16_t Channels, uint32_t Rate, uint16_t Bits,
                         uint16_t Block, const uint8_t* Data, uint32_t Size)
        {
            const bool Adpcm = Format == 0x11;
            const uint32_t FmtSize = Adpcm ? 20 : 16;
            const uint16_t PerBlock = Adpcm && Channels ? (uint16_t)((Block - 4 * Channels) * 8 / (4 * Channels) + 1) : 0;
            const uint32_t Avg = Adpcm && PerBlock ? (uint32_t)((uint64_t)Rate * Block / PerBlock) : Rate * Channels * Bits / 8;
            const uint16_t Align = Adpcm ? Block : (uint16_t)(Channels * Bits / 8);
            auto U32 = [&](uint32_t V) { for (int K = 0; K < 4; K++) Out.push_back((uint8_t)(V >> (8 * K))); };
            auto U16 = [&](uint16_t V) { Out.push_back((uint8_t)V); Out.push_back((uint8_t)(V >> 8)); };
            auto Tag = [&](const char* S) { Out.insert(Out.end(), S, S + 4); };
            Tag("RIFF"); U32(4 + 8 + FmtSize + 8 + Size); Tag("WAVE");
            Tag("fmt "); U32(FmtSize); U16(Format); U16(Channels); U32(Rate); U32(Avg); U16(Align); U16(Bits);
            if (Adpcm) { U16(2); U16(PerBlock); }
            Tag("data"); U32(Size);
            Out.insert(Out.end(), Data, Data + Size);
        }

        void ExportSounds(const Context& C, Totals& T)
        {
            ExportSoundFiles(C, T);
            if (SoundAliasCsv::Wanted(C)) ExportSoundAliases(C, T);
        }

        // snd_alias_t (0x6c): zonetool's CODO header is 8 bytes short; measured on the firing range's 104
        // variants instead (IW5's order without volModIndex): volumes 0x1c/0x20, pitches 0x24/0x28,
        // distances 0x2c/0x30, flags 0x38 (looping 0, type 7-8 matching the SoundFiles), a master
        // priority byte 0x3c (50, 75), probability 0x48, center 0x50, start delay 0x54, the curve
        // pointer 0x58, envelope 0x5c/0x60/0x64, the speaker map 0x68
        void ExportSoundAliases(const Context& C, Totals& T)
        {
            IWSoundAliases::Layout L;
            L.ListCount = ListCount, L.ListCountBytes = 4, L.Size = AliasSize;
            L.Subtitle = 4, L.Secondary = 8, L.SoundFile = AliasFile, L.VolMin = 0x1c, L.VolMax = 0x20, L.PitchMin = 0x24;
            L.PitchMax = 0x28, L.DistMin = 0x2c, L.DistMax = 0x30, L.Flags = 0x38, L.Probability = 0x48, L.CenterPercentage = 0x50;
            L.StartDelay = 0x54, L.EnvelopMin = 0x5c, L.EnvelopMax = 0x60, L.EnvelopPercentage = 0x64;
            // a SoundFile's file as ExportSoundFiles writes it: a loaded sound under its name (.wav
            // added), a streamed one under its folder
            const auto File = [this](const std::string&, uint32_t, int64_t Spec) -> std::string {
                if (Spec < 0) return std::string();
                if (At<uint8_t>(Image(), Spec) == 1)
                {
                    const int64_t Snd = Off(PointerAt(Spec + 4));
                    std::string N = Snd < 0 ? std::string() : StringAt(PointerAt(Snd));
                    if (!N.empty() && N[0] == ',') N.erase(0, 1);
                    if (!N.empty() && ExportNaming::Lower(fs::path(N).extension().string()) != ".wav") N += ".wav";
                    return N.empty() ? N : ExportWriters::SoundFileName(N, ExportWriters::SoundFormatSetting());
                }
                const std::string Dir = StringAt(PointerAt(Spec + 4)), N = StringAt(PointerAt(Spec + 8));
                return N.empty() ? N : ExportWriters::SoundFileName(Dir.empty() ? N : Dir + "/" + N, ExportWriters::SoundFormatSetting());
            };
            const SoundAliasCsv::ZoneView Z = AliasView();
            SoundAliasCsv::Rows Rows;
            for (const auto& I : Walk().Instances)
            {
                if (!OwnerIs(I, "sound") || I.Parent >= 0) continue;
                const std::string N = AssetName(Kind::Sound, I.Pos);
                if (IsReference(Kind::Sound, N, I.Pos)) continue;
                IWSoundAliases::Read(Z, I.Pos, N, L, File, Rows);
            }
            SoundAliasCsv::Write(C, T, Name(), Rows);
        }

        void ExportSoundFiles(const Context& C, Totals& T)
        {
            auto& Kt = T.Kinds[Kind::Sound];
            const auto& Z = Image();
            std::map<std::string, int64_t> Loaded;     // file -> its LoadedSound
            std::set<std::string> Streamed;            // main/sound/<dir>/<name>
            for (const auto& I : Walk().Instances)
            {
                if (!OwnerIs(I, "sound") || I.Parent >= 0) continue;
                const int64_t Head = Off(PointerAt(I.Pos + ListAliases));
                const uint32_t Count = At<uint32_t>(Z, I.Pos + ListCount);
                for (uint32_t K = 0; Head >= 0 && K < Count && K < 4096; K++)
                {
                    const int64_t File = Off(PointerAt(Head + (int64_t)K * AliasSize + AliasFile));
                    if (File < 0) continue;
                    if (At<uint8_t>(Z, File) == 1)
                    {
                        const int64_t Snd = Off(PointerAt(File + 4));
                        const std::string N = Snd < 0 ? std::string() : StringAt(PointerAt(Snd));
                        if (!N.empty()) Loaded.emplace(N, Snd);
                    }
                    else
                    {
                        const std::string Dir = StringAt(PointerAt(File + 4)), N = StringAt(PointerAt(File + 8));
                        if (!N.empty()) Streamed.insert("main/sound/" + (Dir.empty() ? N : Dir + "/" + N));
                    }
                }
            }
            const fs::path Root = C.KindDir(Kind::Sound);
            const std::string Format = ExportWriters::SoundFormatSetting();
            size_t N = 0;
            const size_t All = Loaded.size() + Streamed.size();
            for (const auto& [F, Snd] : Loaded)
            {
                if (!C.Advance(N++, All, F)) return;
                if (!C.Wants(Kind::Sound, F)) continue;
                if (F[0] == ',')   // a name-only header: another zone holds the sound
                {
                    Kt.References++;
                    continue;
                }
                Kt.Seen++;
                const uint32_t Codec = At<uint32_t>(Z, Snd + 4), Size = At<uint32_t>(Z, Snd + 0xc);
                const int64_t Data = Off(PointerAt(Snd + 0x2c));
                if (Size == 0 || Data < 0 || (uint64_t)Data + Size > Z.size() || (Codec != 1 && Codec != 0x11))
                {
                    Counter("sounds_loaded_unread_format_" + std::to_string(Codec))++;
                    T.Fail(Name(), Kind::Sound, F, "no payload in a known format");
                    continue;
                }
                std::vector<uint8_t> Wav;
                Riff(Wav, (uint16_t)Codec, (uint16_t)At<uint32_t>(Z, Snd + 0x18), At<uint32_t>(Z, Snd + 0x10),
                     (uint16_t)At<uint32_t>(Z, Snd + 0x14), (uint16_t)At<uint32_t>(Z, Snd + 0x24), Z.data() + Data, Size);
                std::string File = F;
                if (File.size() < 4 || ExportNaming::Lower(File.substr(File.size() - 4)) != ".wav") File += ".wav";
                std::string Why;
                if (ExportWriters::WriteSound(ExportWriters::SoundSource::OfFile(Wav.data(), Wav.size(), File),
                                              (Root / Ifs::Utf8Path(ExportWriters::SoundFileName(File, Format))).make_preferred(), Format, &Why))
                {
                    Kt.Written++;
                    Counter(Codec == 0x11 ? "sounds_loaded_ima_adpcm" : "sounds_loaded_pcm")++;
                }
                else
                    T.Fail(Name(), Kind::Sound, F, Why);
            }
            std::string Error;
            const auto Archives = Archives_(Error);
            for (const auto& F : Streamed)
            {
                if (!C.Advance(N++, All, F)) return;
                const std::string Rel = F.substr(std::string("main/sound/").size());
                if (!C.Wants(Kind::Sound, Rel)) continue;
                Kt.Seen++;
                const auto* L = Archives ? Archives->Find(F) : nullptr;
                std::vector<uint8_t> Data;
                if (L == nullptr || !Archives->Read(*L, Data, Error))
                {
                    Counter("sounds_streamed_missing")++;
                    T.Fail(Name(), Kind::Sound, Rel, "not in the archives");
                    continue;
                }
                Codol::UnwrapFile(F, Data);
                std::string Why;
                if (ExportWriters::WriteSound(ExportWriters::SoundSource::OfFile(Data.data(), Data.size(), Rel),
                                              (Root / Ifs::Utf8Path(ExportWriters::SoundFileName(Rel, Format))).make_preferred(), Format, &Why))
                {
                    Kt.Written++;
                    Counter("sounds_streamed")++;
                }
                else
                    T.Fail(Name(), Kind::Sound, Rel, Why);
            }
        }
    };

    class CodolGame final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "codol"; }
        const char* Title() const override { return "Call of Duty Online"; }
        const char* Engine() const override { return "CODOL"; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<CodolZone>(std::string(), std::string()); }
        // the localized zones' MPUI_ strings, in GBK: a map by the key its list mp/comaplist.csv
        // gives it (mp_cross_fire MPUI_CROSSFIRE), else by its name with or without mp_ or a
        // variant suffix (mp_hardhat_sh MPUI_HARDHAT, mp_battlecity MPUI_MP_BATTLECITY), a co-op
        // mission by its own (MPUI_COOP_DEFENSE_MP_CRASH_TROPICAL), a mode's addon zone by its map's
        const LevelNaming* LevelNames() const override
        {
            static const LevelNaming Naming = [] {
                const std::vector<std::string> Map = { "MPUI_MP_{REST}", "MPUI_{REST}" };
                LevelNaming N;
                N.From = { "code_post_gfx_mp", "localized_common_gametype_*", "localized_common_mp", "localized_ui_mp",
                           "localized_code_post_gfx_mp" };
                N.Lists = { "mp/comaplist.csv" };
                N.Keys = { { "coop_*", { "MPUI_{NAME}" } },
                           { "*_addon_mp_*", { "MPUI_MP_{REST2}", "MPUI_{REST2}" } },
                           { "*_addon_*", { "MPUI_{REST2}" } },
                           { "mp_*", Map },
                           { "mp_*_sh", Map },
                           { "mp_*_cls", Map },
                           { "mp_*_long", Map },
                           { "zm_*_sh", { "MPUI_ZM_{REST}" } },
                           { "za_*", { "MPUI_{REST}" } } };
                N.CodePage = 936;
                return N;
            }();
            return &Naming;
        }

        // An install folder (every zone of its archives), one .ifs archive (its zones),
        // `ifs:<glob>` (those zones of --install),
        // a loose .ff, or a folder of them
        bool Zones(const std::string& Input, const ExportOptions& Options, std::vector<std::string>& Out,
                   std::string& Error) const override
        {
            if (ExportNaming::Lower(Ifs::Utf8(Ifs::Utf8Path(Input).extension())) == ".ifs")
            {
                // one archive: its fast files, each at its latest version in the install the
                // archive belongs to (<install>/IIPS/IIPSDownload/<archive>)
                Ifs::Archive A;
                if (!A.Open(Input, Error)) return false;
                const std::string Dir = Options.Install.empty()
                    ? Ifs::Utf8(Ifs::Utf8Path(Input).parent_path().parent_path().parent_path()) : Options.Install;
                for (const auto& E : A.Files())
                {
                    const std::string Key = Ifs::Install::Key(E.Name);
                    if (Key.size() > 3 && Key.compare(Key.size() - 3, 3, ".ff") == 0)
                        Out.push_back(Ifs::Utf8(Ifs::Utf8Path(Dir) / ZoneFolder / Ifs::Utf8Path(Key).filename()));
                }
                if (Out.empty()) Error = "no fast file in " + Input;
                return !Out.empty();
            }
            const bool Pattern = Input.rfind("ifs:", 0) == 0;
            const std::string Dir = Pattern ? Options.Install : Input;
            if (!Pattern && !IsInstall(Dir)) return IDiskGame::Zones(Input, Options, Out, Error);
            const auto I = InstallOf(Dir, Error);
            if (!I) return false;
            for (const auto& N : ZoneNames(*I, Pattern ? Input.substr(4) : "*"))
                Out.push_back(Ifs::Utf8(Ifs::Utf8Path(Dir) / ZoneFolder / (N + ".ff")));
            if (Out.empty()) Error = "no zone matches " + Input;
            return !Out.empty();
        }

        bool ListZones(const std::string& Install, const ExportOptions& Options, std::vector<std::string>& Names,
                       std::string& Error) const override
        {
            if (!IsInstall(Install)) return IDiskGame::ListZones(Install, Options, Names, Error);
            const auto I = InstallOf(Install, Error);
            if (!I) return false;
            for (const auto& N : ZoneNames(*I, "*")) Names.push_back(std::string(ZoneFolder) + "/" + N + ".ff");
            return !Names.empty();
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const ExportOptions& Options, std::string& Error) const override
        {
            auto Zone = std::make_unique<CodolZone>(Path, InstallFromZone(Path, Options.Install));
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }

        const char* DefaultImageFormat() const override { return "dds"; }
    };

    const CodolGame Game;
    const DiskGame::Registration Registered(Game);
}
