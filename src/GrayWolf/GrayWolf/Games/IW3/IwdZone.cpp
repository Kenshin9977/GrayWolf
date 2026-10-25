#include "stdafx.h"

#include "ExportWriters.h"

#include "Games/IW3/IwdZone.h"
#include "Games/IW3/LoadDefImage.h"

#include "CoDIWITranslator.h"
#include "Image.h"
#include "IWDCache.h"
#include <miniz/miniz.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace fs = std::filesystem;

namespace
{
    using DiskGame::Context;
    using DiskGame::Kind;
    using DiskGame::Totals;

    // GfxImage (0x24 in CoD4 and World at War): its name at +0x20
    constexpr int64_t ImageNameAt = 0x20;

    // Every file path of the install's .iwd archives (lower case, '/' separated) -> the archive
    // holding it, the newest archive first, as the game's search path; built once per install
    std::mutex ArchivesLock;
    std::map<std::string, std::shared_ptr<const std::map<std::string, fs::path>>> ArchiveIndexes;

    std::shared_ptr<const std::map<std::string, fs::path>> ArchiveIndexOf(const std::string& Install)
    {
        std::lock_guard<std::mutex> G(ArchivesLock);
        auto& Index = ArchiveIndexes[Install];
        if (Index) return Index;
        std::vector<fs::path> Archives;
        std::error_code Ec;
        for (const auto& E : fs::directory_iterator(fs::path(Install) / "main", Ec))
            if (ExportNaming::Lower(E.path().extension().string()) == ".iwd") Archives.push_back(E.path());
        std::sort(Archives.rbegin(), Archives.rend());
        auto Built = std::make_shared<std::map<std::string, fs::path>>();
        for (const auto& A : Archives)
        {
            mz_zip_archive Zip;
            std::memset(&Zip, 0, sizeof(Zip));
            if (!mz_zip_reader_init_file(&Zip, A.string().c_str(), 0)) continue;
            const mz_uint Count = mz_zip_reader_get_num_files(&Zip);
            for (mz_uint K = 0; K < Count; K++)
            {
                mz_zip_archive_file_stat Stat;
                if (mz_zip_reader_file_stat(&Zip, K, &Stat)) Built->emplace(ExportNaming::Lower(Stat.m_filename), A);
            }
            mz_zip_reader_end(&Zip);
        }
        Index = Built;
        return Index;
    }

    // A file of the install's .iwd archives by its path inside them (what IWDCache does not index:
    // the .mp3 of streamed music and ambience; a name its stem does not find), whatever its case and
    // separators (World at War's aliases mix '\' and '/' and the case of the archives' names)
    std::unique_ptr<uint8_t[]> ArchiveFile(const std::string& Install, std::string Inner, uint32_t& Size)
    {
        std::replace(Inner.begin(), Inner.end(), '\\', '/');
        const auto Index = ArchiveIndexOf(Install);
        const auto It = Index->find(ExportNaming::Lower(Inner));
        if (It == Index->end()) return nullptr;
        mz_zip_archive Zip;
        std::memset(&Zip, 0, sizeof(Zip));
        if (!mz_zip_reader_init_file(&Zip, It->second.string().c_str(), 0)) return nullptr;
        std::unique_ptr<uint8_t[]> Out;
        const int At = mz_zip_reader_locate_file(&Zip, It->first.c_str(), nullptr, 0);   // case-insensitive
        mz_zip_archive_file_stat Stat;
        if (At >= 0 && mz_zip_reader_file_stat(&Zip, (mz_uint)At, &Stat))
        {
            Out = std::make_unique<uint8_t[]>((size_t)Stat.m_uncomp_size);
            if (mz_zip_reader_extract_to_mem(&Zip, (mz_uint)At, Out.get(), (size_t)Stat.m_uncomp_size, 0))
                Size = (uint32_t)Stat.m_uncomp_size;
            else
                Out.reset();
        }
        mz_zip_reader_end(&Zip);
        return Out;
    }

}

namespace Iw3Family
{
    IwdZone::IwdZone(const std::string& Path, std::string InstallDir) : Iw32LevelZone(Path), Install(std::move(InstallDir))
    {
        Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
        Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
        Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
        // the .gsc/.csc rawfiles {name, u32 len, buffer}: source, stored as is
        Exports(Kind::Script, [this](const Context& C, Totals& T) {
            ExportScripts(C, T, {}, { { "rawfile", [this](int64_t Pos, Scripts::Script& S) {
                              if (!Scripts::IsSourceName(S.Name)) S.Name.clear();
                              return S.Name.empty() || BytesAt(PointerAt(Pos + 8), At<uint32_t>(Image(), Pos + 4), S.Bytes);
                          } } });
        });
        // rawfile (0xc) {name, u32 len 4, buffer 8 (len + 1 read)}, every file as it is;
        // stringtable (0x10) {name, i32 columns 4, rows 8, cells 0xc: a string pointer each}
        Exports(Kind::Rawfile, [this](const Context& C, Totals& T) {
            ExportRawfiles(C, T, { { "rawfile", [this](int64_t Pos, std::string&, std::vector<uint8_t>& Bytes) {
                                        return RawBytes(Pos, { 4, 8 }, Bytes);
                                    } } });
        });
        Exports(Kind::Data, [this](const Context& C, Totals& T) {
            ExportStringTables(C, T, "stringtable", [this](int64_t Pos, uint32_t& Columns, uint32_t& Rows, std::vector<std::string>& Cells) {
                return StringTableCells(Pos, { 4, 8, 0xc, 4 }, Columns, Rows, Cells);
            });
        });
    }

    IwdZone::~IwdZone()
    {
        Serve(false);
    }

    VirtualGameSession::Handlers IwdZone::Handlers()
    {
        Slots = ZoneStrings(Zone());
        return { SlotString, ImageHandler() };
    }

    void IwdZone::Served(bool On)
    {
        Serve(On);
    }

    void IwdZone::Fixups()
    {
        Serve(true);
    }

    std::string IwdZone::AssetName(Kind K, int64_t Pos) const
    {
        return StringAt(PointerAt(K == Kind::Image ? Pos + ImageNameAt : Pos));
    }

    // the live image handler reads the .iwd through CoDAssets::GamePackageCache while the zone is
    // served; the previous cache comes back when it is not
    void IwdZone::Serve(bool On)
    {
        if (Install.empty()) return;
        ServePackages(On, [this] {
            if (!Iwd) Iwd = IwdPackagesOf(Install);
            return SharedPackages(Iwd);
        });
    }

    // The zone's script strings: the count right after the XFile, the list
    // pointer after it (any non-null value: present), u32 slots after the 16-byte asset list, then
    // the text of every -1 slot
    std::vector<std::string> IwdZone::ZoneStrings(const std::vector<uint8_t>& Z) const
    {
        const int64_t List = XFileSize(), First = List + 0x10;
        const uint32_t Count = At<uint32_t>(Z, List);
        if (At<uint32_t>(Z, List + 4) == 0 || Count == 0 || (size_t)First + (size_t)Count * 4 > Z.size()) return {};
        return InlineStrings(Z, First, Count, 4, 0xFFFFFFFFu);
    }

    // Every image the zone names (its own and its materials'): from the .iwd by name, or from the
    // pixels the zone carries (a loadDef: the code images, $white and the like). A "," name is a
    // reference, defined in another zone.
    void IwdZone::ExportImages(const Context& C, Totals& T)
    {
        ExportNamedImages(C, T, [this](const std::string& N, int64_t Pos) { return Decode(N, Pos); },
                          "neither in the .iwd archives nor in the zone");
    }

    // the images and loaded sounds this zone holds whole, for the install index (a texture or a
    // sound another zone names)
    void IwdZone::Defines(std::vector<DiskGame::AssetId>& Out) const
    {
        ZonekitZone::Defines(Out);
        const int64_t LoadedSize = Sounds().LoadedSize;
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, "image"))
            {
                const std::string N = AssetName(Kind::Image, I.Pos);
                if (!N.empty() && !IsReference(Kind::Image, N, I.Pos)) Out.push_back({ Kind::Image, N });
            }
            else if (OwnerIs(I, "loaded_sound") && At<uint32_t>(Image(), I.Pos + LoadedSize) != 0)
            {
                const std::string N = AssetName(Kind::Sound, I.Pos);
                if (!N.empty() && !IsReference(Kind::Sound, N, I.Pos)) Out.push_back({ Kind::Sound, N });
            }
    }

    std::unique_ptr<XImageDDS> IwdZone::Decode(const std::string& ImageName, int64_t Pos)
    {
        if (!Install.empty())
        {
            if (!Iwd) Iwd = IwdPackagesOf(Install);
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

    // GfxImageLoadDef (the image's +4, read in the temporary block: 0x10 then u32@0xc bytes):
    // levelCount u8, flags u8, dimensions u16 x 3 at +2, a D3DFORMAT at +8, the size at +0xc, the
    // pixels from +0x10, the largest level first
    std::unique_ptr<XImageDDS> IwdZone::LoadDef(int64_t Pos)
    {
        const int64_t Def = Off(PointerAt(Pos + 4));
        if (Def < 0) return nullptr;
        const auto& Z = Image();
        const uint32_t W = At<uint16_t>(Z, Def + 2), H = At<uint16_t>(Z, Def + 4);
        const uint32_t Format = At<uint32_t>(Z, Def + 8), Size = At<uint32_t>(Z, Def + 0xc);
        ImageFormat Fmt = ImageFormat::DDS_BC1_UNORM;
        uint64_t Level = 0;
        bool Swap = false;
        if (!LoadDefImage::D3dFormat(Format, W, H, Fmt, Level, Swap))
        {
            Counter("images_in_zone_unknown_format")++;
            return nullptr;
        }
        if (W == 0 || H == 0 || Level > Size || Def + 0x10 + (int64_t)Level > (int64_t)Z.size()) return nullptr;
        return LoadDefImage::Dds(W, H, Fmt, Z.data() + Def + 0x10, Level, Swap);
    }

    // Loaded sounds (the title's), then the files of the .iwd archives' sound folder: streamed
    // aliases (a folder and a file name) and loaded sounds whose header names a file another
    // place holds, written as they are
    void IwdZone::ExportSounds(const Context& C, Totals& T)
    {
        ExportLoadedSounds(C, T);
        ExportSoundFiles(C, T);
        if (SoundAliasCsv::Wanted(C)) ExportSoundAliases(C, T);
    }

    std::string IwdZone::AliasFile(int64_t File) const
    {
        if (File < 0) return std::string();
        const SoundLayout L = Sounds();
        std::string F;
        if (At<uint8_t>(Image(), File) == 1)
        {
            // a loaded sound (",name": its samples in another zone)
            const int64_t Loaded = Off(PointerAt(File + 4));
            F = Loaded < 0 ? std::string() : StringAt(PointerAt(Loaded));
            if (!F.empty() && F[0] == ',') F.erase(0, 1);
            return F.empty() ? F : LoadedSoundFile(F);
        }
        // a file of the archives, written under its folder (ExportSoundFiles)
        const std::string Dir = StringAt(PointerAt(File + L.StreamDir)), Leaf = StringAt(PointerAt(File + L.StreamName));
        if (Leaf.empty()) return std::string();
        F = Dir.empty() ? Leaf : Dir + "/" + Leaf;
        const std::string Leafname = ExportWriters::SoundFileName(fs::path(F).filename().string(), ExportWriters::SoundFormatSetting());
        const fs::path P = fs::path(F).parent_path() / ExportNaming::SafeFileName(Leafname);
        return P.generic_string();
    }

    void IwdZone::ExportSoundAliases(const Context& C, Totals& T)
    {
        const SoundAliasCsv::ZoneView Z = AliasView();
        const IWSoundAliases::Layout L = AliasLayout();
        SoundAliasCsv::Rows Rows;
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "sound")) continue;
            const std::string N = AssetName(Kind::Sound, I.Pos);
            if (IsReference(Kind::Sound, N, I.Pos)) continue;
            IWSoundAliases::Read(Z, I.Pos, N, L, [this](const std::string&, uint32_t, int64_t File) { return AliasFile(File); }, Rows);
        }
        SoundAliasCsv::Write(C, T, Name(), Rows);
    }

    void IwdZone::ExportSoundFiles(const Context& C, Totals& T)
    {
        auto& Kt = T.Kinds[Kind::Sound];
        const SoundLayout L = Sounds();
        std::set<std::string> Files;
        const auto& Z = Image();
        for (const auto& I : Walk().Instances)
        {
            if (OwnerIs(I, "loaded_sound") && At<uint32_t>(Z, I.Pos + L.LoadedSize) == 0)
            {
                // a "," name: a loaded sound another zone holds the samples of (the index finds it)
                const std::string N = AssetName(Kind::Sound, I.Pos);
                if (IsReference(Kind::Sound, N, I.Pos) && C.Wants(Kind::Sound, Referenced(Kind::Sound, N)))
                {
                    Kt.References++;
                    C.Needs(Kind::Sound, Referenced(Kind::Sound, N));
                }
                else if (!N.empty())
                    Files.insert(N);
            }
            // snd_alias_list: its aliases at +4, their count at +8
            if (!OwnerIs(I, "sound")) continue;
            const int64_t Head = Off(PointerAt(I.Pos + 4));
            const uint32_t Count = At<uint32_t>(Z, I.Pos + 8);
            for (uint32_t K = 0; Head >= 0 && K < Count && K < 4096; K++)
            {
                const int64_t File = Off(PointerAt(Head + (int64_t)K * L.AliasSize + L.AliasFile));
                if (File < 0 || At<uint8_t>(Z, File) == 1) continue;
                const std::string Dir = StringAt(PointerAt(File + L.StreamDir)), Leaf = StringAt(PointerAt(File + L.StreamName));
                if (!Leaf.empty()) Files.insert(Dir.empty() ? Leaf : Dir + "/" + Leaf);
            }
        }
        if (Files.empty() || Install.empty()) return;
        if (!Iwd) Iwd = IwdPackagesOf(Install);
        const fs::path Root = C.KindDir(Kind::Sound);
        size_t N = 0;
        for (const auto& F : Files)
        {
            if (!C.Advance(N++, Files.size(), F)) break;
            if (!C.Wants(Kind::Sound, F)) continue;
            Kt.Seen++;
            // by its path first: a stem names several files (`null.wav` in two folders), the image/wav
            // index keeps one of them
            uint32_t Size = 0;
            auto Data = ArchiveFile(Install, "sound/" + F, Size);
            if (!Data) Data = Iwd->ExtractPackageObject(Iwd->HashPackageID(fs::path(F).stem().string()), Size);
            if (!Data)
            {
                Counter("sound_files_missing")++;
                T.Fail(Name(), Kind::Sound, F, "not in the .iwd archives");
                continue;
            }
            const std::string Format = ExportWriters::SoundFormatSetting();
            const fs::path Out = Root / fs::path(F).parent_path() /
                                 ExportNaming::SafeFileName(ExportWriters::SoundFileName(fs::path(F).filename().string(), Format));
            std::string Why;
            if (ExportWriters::WriteSound(ExportWriters::SoundSource::OfFile(Data.get(), Size, F), Out.string(), Format, &Why))
            {
                Kt.Written++;
                Counter("sound_files")++;
            }
            else
                T.Fail(Name(), Kind::Sound, F, Why);
        }
    }
}
