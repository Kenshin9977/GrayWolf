#include "stdafx.h"

#include "Bo4DiskExport.h"
#include "Bo4SoundBanks.h"
#include "DiskGame.h"
#include "ICoDGame.h"
#include "VirtualGameSession.h"
#include "ExportNaming.h"
#include "ExportWriters.h"

#include "CoDAssets.h"
#include "CoDAssetType.h"
#include "CoDRawImageTranslator.h"
#include "DBGameAssets.h"
#include "GameBlackOps4.h"
#include "SettingsManager.h"
#include "Strings.h"
#include "FastFile.h"
#include "FileSystems.h"
#include "Image.h"
#include "WraithNameIndex.h"
#include "ZoneTableWalker.h"
#include "ZonekitZone.h"
#include "Games/Treyarch/TreyarchPackages.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace Bo4DiskExport
{
namespace
{
    // Image names: the fnv1a index shipped in package_index (the Dev build has none of its
    // own, so the Release one next to it is tried too).
    const WraithNameIndex& ImageNames()
    {
        static std::once_flag Once;
        static WraithNameIndex Index;
        std::call_once(Once, []() {
            const fs::path App(FileSystems::GetApplicationPath());
            for (const fs::path& Dir : {App / "package_index", App.parent_path() / "Release" / "package_index"})
                if (fs::exists(Dir / "fnv1a_ximages.wni"))
                {
                    Index.LoadIndex((Dir / "fnv1a_ximages.wni").string());
                    break;
                }
        });
        return Index;
    }

    // The zone's own string list, for GameStringHandler during a model export (bone names): the
    // bone ids index it. An encrypted one ([id & 0xC0 == 0x80][length][ciphertext]) is what the
    // game's interning (0x2756910) decrypts with 0x3c4c570 - the live cipher, DecryptString.
    // per thread: bo4-disk-export reads several zones at once
    thread_local std::vector<std::string> ZoneStrings;

    std::string DiskStringEntry(uint64_t Index)
    {
        return VirtualGameSession::SlotString(&ZoneStrings, Index);
    }

    void ReadZoneStrings(const std::vector<uint8_t>& Zone);
}

// the zone's string table, decrypted as the game does - for diagnosis (bo4-disk-export --strings)
const std::vector<std::string>& ZoneStringTable(const std::vector<uint8_t>& Zone)
{
    ReadZoneStrings(Zone);
    return ZoneStrings;
}

std::string ZoneString(const std::vector<uint8_t>& Zone, size_t Start, size_t& End)
{
    End = std::find(Zone.begin() + (ptrdiff_t)Start, Zone.end(), 0) - Zone.begin();
    const uint8_t* S = Zone.data() + Start;
    const size_t Len = End - Start;
    if (Len >= 2 && (S[0] & 0xC0) == 0x80)
    {
        // DecryptString works over the declared length (S[1]), which the bytes up to the NUL
        // may not reach: pad the copy to it
        std::vector<uint8_t> Copy(S + 2, S + Len);
        Copy.resize(std::max<size_t>(Copy.size(), S[1]) + 1, 0);
        return GameBlackOps4::DecryptString(Copy.data(), S[1], S[0], 0);
    }
    return std::string(reinterpret_cast<const char*>(S), Len);
}

namespace
{
    void ReadZoneStrings(const std::vector<uint8_t>& Zone)
    {
        ZoneStrings.clear();
        if (Zone.size() < 0x20) return;
        int32_t Count = 0;
        uint64_t Ptr = 0;
        std::memcpy(&Count, Zone.data(), 4);
        std::memcpy(&Ptr, Zone.data() + 8, 8);
        if (Ptr == 0 || Count <= 0) return;
        size_t Pos = 0x20 + 8 * (size_t)Count;
        for (int32_t K = 0; K < Count && Pos < Zone.size(); K++)
        {
            uint64_t Slot = 0;
            std::memcpy(&Slot, Zone.data() + 0x20 + 8 * (size_t)K, 8);
            if (Slot != 0xFFFFFFFFFFFFFFFFull) { ZoneStrings.emplace_back(); continue; }
            size_t End = 0;
            ZoneStrings.push_back(ZoneString(Zone, Pos, End));
            Pos = End + 1;
        }
    }

    using ExportNaming::At;
    using ExportNaming::Lower;
    using ExportNaming::SafeFileName;

    // What an image's output file is made of: the streamed payload's package key (the key names
    // the bytes) or a hash of the in-zone bytes, plus every field that shapes the file and the
    // extension. 0 when the image has no pixels to write.
    uint64_t ContentKey(const std::vector<uint8_t>& Zone, const ImageInfo& Img, const std::string& Ext)
    {
        uint64_t H = 0x9E3779B97F4A7C15ull;
        auto Mix = [&H](const void* P, size_t N) {
            const uint8_t* B = static_cast<const uint8_t*>(P);
            size_t I = 0;
            for (; I + 8 <= N; I += 8)
            {
                uint64_t W;
                std::memcpy(&W, B + I, 8);
                H = (H ^ W) * 0xFF51AFD7ED558CCDull;
                H ^= H >> 32;
            }
            for (; I < N; I++) H = (H ^ B[I]) * 0x100000001B3ull;
        };
        if (!Img.Streamed && Img.PayloadPos >= 0)
        {
            if (Img.PayloadPos + (int64_t)Img.PayloadSize > (int64_t)Zone.size()) return 0;
            Mix(Zone.data() + Img.PayloadPos, Img.PayloadSize);
            Mix(&Img.Width, 2); Mix(&Img.Height, 2); Mix(&Img.Depth, 2); Mix(&Img.Mips, 1);
        }
        else if (Img.MipHash != 0 && Img.MipSize != 0)
        {
            Mix(&Img.MipHash, 8); Mix(&Img.MipSize, 4); Mix(&Img.MipWidth, 2); Mix(&Img.MipHeight, 2);
        }
        else
            return 0;
        Mix(&Img.Format, 4); Mix(&Img.MapType, 1); Mix(Ext.data(), Ext.size());
        return H ? H : 1;
    }

}

bool ReadZone(const std::vector<uint8_t>& Zone, const std::string& Profile, ZoneAssets& Out,
              std::string& Error)
{
    const ExportWriters::LoadedProfile* L = ExportWriters::ZoneProfile(Profile, Error);
    if (L == nullptr) return false;
    const ZoneTables::ZoneResult R = ZoneTables::WalkZone(Zone, L->Tables, L->Profile.TypeNames,
                                                      L->Profile.Rules, true);
    if (R.ParseFailed)
    {
        Error = R.Why;
        return false;
    }
    // every pointer resolved the way the disk template does (ResolveLinks, ZonekitZone.h)
    const ResolvedLinks Links = ResolveLinks(R, L->Profile.Rules, true, &Zone);
    Out.Links = Links.Links;
    Out.BackRefs = R.BackRefs.size();
    for (const char* K : {"links_asset", "links_data"})
        if (auto It = Links.Counters.find(K); It != Links.Counters.end()) Out.BackRefsResolved += It->second;
    DescribeZone(Zone, R, Out);
    return true;
}

void DescribeZone(const std::vector<uint8_t>& Zone, const ZoneTables::ZoneResult& R, ZoneAssets& Out)
{
    Out.Why = R.Why;
    Out.Complete = R.Why == "walked to the end";
    const WraithNameIndex& Names = ImageNames();
    for (const auto& I : R.Instances)
    {
        const std::string& Type = R.OwnerNames[I.Owner];
        if (I.Parent < 0) Out.Assets.emplace_back(Type, I.Pos);
        Out.Roots.emplace_back(Type, I.Pos);
        if (Type == "xmodel" && I.EndRead > I.FirstRead)
            Out.Models.emplace_back(At<uint64_t>(Zone, I.Pos) & 0xFFFFFFFFFFFFFFFull, I.Pos);
        if (Type == "material" && I.EndRead > I.FirstRead)
            Out.Materials.emplace_back(At<uint64_t>(Zone, I.Pos) & 0xFFFFFFFFFFFFFFFull, I.Pos);
        if (Type == "xanim" && I.EndRead > I.FirstRead)
            Out.Anims.emplace_back(At<uint64_t>(Zone, I.Pos + offsetof(BO4XAnim, NamePtr)) & 0xFFFFFFFFFFFFFFFull,
                                   I.Pos);
        if (Type != "image" || I.EndRead <= I.FirstRead) continue;
        ImageInfo Img;
        Img.Pos = I.Pos;
        Img.Hash = At<uint64_t>(Zone, I.Pos + 0x20) & 0xFFFFFFFFFFFFFFFull;
        const auto Found = Names.NameDatabase.find(Img.Hash);
        char Fallback[32] = {};
        snprintf(Fallback, sizeof(Fallback), "ximage_%llx", (unsigned long long)Img.Hash);
        Img.Name = Found != Names.NameDatabase.end() ? Found->second : Fallback;
        Img.PayloadSize = At<uint32_t>(Zone, I.Pos + 0x60);
        Img.Format = At<uint32_t>(Zone, I.Pos + 0x64);
        Img.Width = At<uint16_t>(Zone, I.Pos + 0x68);
        Img.Height = At<uint16_t>(Zone, I.Pos + 0x6a);
        Img.Depth = At<uint16_t>(Zone, I.Pos + 0x6c);
        Img.MapType = At<uint8_t>(Zone, I.Pos + 0x79);
        Img.Mips = At<uint8_t>(Zone, I.Pos + 0x7a);
        Img.Streamed = At<uint8_t>(Zone, I.Pos + 0x7c) != 0;
        // the payload is the image's own read of u32@0x60 bytes (its SELF read is the first)
        if (!Img.Streamed && Img.PayloadSize != 0)
            for (uint32_t K = I.FirstRead + 1; K < I.EndRead; K++)
                if (R.Owners[K] == I.Owner && R.Reads[K] == (int64_t)Img.PayloadSize)
                {
                    Img.PayloadPos = R.ReadPos[K];
                    break;
                }
        // the mip records: the last read of u8@0x7d x 0x28 bytes (the table's BYTES+ at +0x30)
        const uint8_t MipCount = At<uint8_t>(Zone, I.Pos + 0x7d);
        int64_t MipPos = -1;
        for (uint32_t K = I.FirstRead + 1; MipCount && K < I.EndRead; K++)
            if (R.Owners[K] == I.Owner && R.Reads[K] == (int64_t)MipCount * 0x28) MipPos = R.ReadPos[K];
        // as GameBlackOps4::LoadXImage: the widest mip, its size the delta of the running sizes
        uint32_t Prev = 0;
        for (uint8_t M = 0; MipPos >= 0 && M < MipCount; M++)
        {
            const int64_t P = MipPos + (int64_t)M * 0x28;
            Img.MipKeys.push_back(At<uint64_t>(Zone, P));
            const uint32_t Size = At<uint32_t>(Zone, P + 0x20);
            const uint16_t W = At<uint16_t>(Zone, P + 0x24);
            if (W > Img.MipWidth)
            {
                Img.MipWidth = W;
                Img.MipHeight = At<uint16_t>(Zone, P + 0x26);
                Img.MipHash = At<uint64_t>(Zone, P);
                Img.MipSize = M == 0 ? Size >> 4 : (Size >> 4) - (Prev >> 4);
            }
            Prev = Size;
        }
        Out.Images.push_back(std::move(Img));
    }
}

bool LoadZone(const std::string& Path, std::vector<uint8_t>& Zone, std::string& Error)
{
    if (Lower(fs::path(Path).extension().string()) == ".ff")
    {
        fs::path Patch(Path);
        Patch.replace_extension(".fd");
        std::error_code Ec;
        return FastFile::Decompress(fs::exists(Patch, Ec) ? Patch.string() : Path, Zone, Error);
    }
    std::ifstream In(Path, std::ios::binary | std::ios::ate);
    if (!In)
    {
        Error = "cannot open " + Path;
        return false;
    }
    const std::streamsize Size = In.tellg();
    In.seekg(0);
    Zone.resize(static_cast<size_t>(Size));
    if (!In.read(reinterpret_cast<char*>(Zone.data()), Size))
    {
        Error = "cannot read " + Path;
        return false;
    }
    return true;
}

bool Zones(const std::string& Input, CoDPackageCache* Packages, const std::string& CacheDir,
           const std::string& UiRes, std::vector<std::string>& Out, size_t& Skipped, std::string& Error)
{
    if (Input.rfind("casc:", 0) == 0)
    {
        if (Packages == nullptr)
        {
            Error = "casc: zones need --install";
            return false;
        }
        Out = TreyarchPackages::ExtractFastFiles(Packages, Input.substr(5), CacheDir, Error);
        if (Out.empty())
        {
            Error = "no fast file matches " + Input + " " + Error;
            return false;
        }
    }
    else if (fs::is_directory(Input))
    {
        for (const auto& E : fs::directory_iterator(Input))
        {
            const std::string Ext = E.path().extension().string();
            if (Ext == ".ff" || Ext == ".bin") Out.push_back(E.path().string());
        }
    }
    else if (fs::path(Input).extension() == ".txt")
    {
        std::ifstream F(Input);
        std::string Line;
        while (std::getline(F, Line))
        {
            while (!Line.empty() && (Line.back() == '\r' || Line.back() == ' ')) Line.pop_back();
            if (!Line.empty()) Out.push_back(Line);
        }
    }
    else
        Out.push_back(Input);
    const size_t Listed = Out.size();
    Out.erase(std::remove_if(Out.begin(), Out.end(), [&](const std::string& Z) { return !KeepZoneForUiRes(Z, UiRes); }),
              Out.end());
    Skipped = Listed - Out.size();
    if (Out.empty() && Error.empty()) Error = "no zone in " + Input;
    return !Out.empty();
}

bool KeepZoneForUiRes(const std::string& Zone, const std::string& UiRes)
{
    const std::string Res = Lower(UiRes);
    if (Res.empty() || Res == "all") return true;
    // the resolution tag is the first token, or the second after a locale prefix
    const std::string Stem = Lower(fs::path(Zone).stem().string());
    const size_t A = Stem.find('_');
    const std::string T0 = Stem.substr(0, A);
    const std::string T1 = A == std::string::npos ? "" : Stem.substr(A + 1, Stem.find('_', A + 1) - A - 1);
    const bool Is1080 = T0 == "1080" || T1 == "1080";
    const bool Is4k = T0 == "4k" || T1 == "4k";
    if (Is1080) return Res == "1080";
    if (Is4k) return Res == "4k";
    return true;
}


namespace
{
    // An image as DDS: in-zone pixels from the zone, streamed ones from the packages by the widest
    // mip's key. Null when neither is there; FromPackages tells which path it took.
    std::unique_ptr<XImageDDS> BuildDds(const std::vector<uint8_t>& Zone, const ImageInfo& Img,
                                        CoDPackageCache* Packages, bool& FromPackages)
    {
        FromPackages = false;
        const bool Cube = Img.MapType == 4 || Img.MapType == 5;
        std::unique_ptr<uint8_t[]> Data;
        uint32_t Size = 0, Width = Img.Width, Height = Img.Height;
        uint8_t Levels = std::max<uint8_t>(1, Img.Mips);
        if (!Img.Streamed && Img.PayloadPos >= 0)
        {
            // a cube map keeps its six faces; an array keeps its first layer
            const uint32_t Layers = (!Cube && Img.MapType != 3 && Img.Depth > 1) ? Img.Depth : 1;
            Size = Img.PayloadSize / Layers;
            Data = std::make_unique<uint8_t[]>(Size);
            std::memcpy(Data.get(), Zone.data() + Img.PayloadPos, Size);
        }
        else if (Packages != nullptr && Img.MipHash != 0 && Img.MipSize != 0)
        {
            Data = Packages->ExtractPackageObject(Img.MipHash, (int32_t)Img.MipSize, Size);
            Width = Img.MipWidth;
            Height = Img.MipHeight;
            Levels = 1;
            FromPackages = Data && Size;
        }
        if (!Data || Size == 0) return nullptr;
        // as GameBlackOps4::LoadXImage: the sRGB BC formats written as their UNORM twins
        uint32_t Format = Img.Format;
        switch (Format)
        {
        case 72: Format = 71; break;
        case 75: Format = 74; break;
        case 78: Format = 77; break;
        case 99: Format = 98; break;
        default: break;
        }
        return CoDRawImageTranslator::TranslateBC(Data, Size, Width, Height, static_cast<uint8_t>(Format),
                                                  Levels, Cube);
    }
}

ExportStats ExportImages(const std::vector<uint8_t>& Zone, const ZoneAssets& Assets,
                         const std::string& OutDir, const NameTest& Wants,
                         CoDPackageCache* Packages, const StepFn& Step,
                         const std::function<void(const std::string&)>& OnReference)
{
    std::error_code Ec;
    fs::create_directories(OutDir, Ec);
    ExportStats S;
    // Streamed images are read in the order they lie in the packages (package, offset), not in
    // asset order: on a spinning disk every out-of-order read is a seek. In-zone images need no
    // read and go first. GH_BO4_UNSORTED=1 keeps asset order, for comparing the two.
    std::vector<size_t> Order(Assets.Images.size());
    for (size_t I = 0; I < Order.size(); I++) Order[I] = I;
    if (Packages != nullptr && GetEnvironmentVariableA("GH_BO4_UNSORTED", nullptr, 0) == 0)
    {
        std::vector<std::pair<uint32_t, uint64_t>> Where(Assets.Images.size(), {0, 0});
        for (size_t I = 0; I < Assets.Images.size(); I++)
        {
            const ImageInfo& Img = Assets.Images[I];
            uint32_t Pkg = 0;
            uint64_t Off = 0;
            if (Img.MipHash != 0 && Packages->Locate(Img.MipHash, Pkg, Off)) Where[I] = {Pkg + 1, Off};
        }
        std::stable_sort(Order.begin(), Order.end(), [&](size_t A, size_t B) { return Where[A] < Where[B]; });
    }

    // --image-format (setting "exportimg"): DDS writes the texture as stored; PNG/TGA decode the BC
    // blocks and encode - the CPU half of the work, done by the sink's pool while this thread reads
    // in disk order. Same bytes already exported (this zone or an earlier one) become a link: keyed
    // by content, not name, as a fifth of the names shared across zones are different images.
    ExportWriters::ImageSink Sink;
    const std::string& Ext = Sink.Extension();
    std::set<std::string> Referenced, Defined;
    for (size_t N = 0; N < Order.size(); N++)
    {
        const size_t Index = Order[N];
        const ImageInfo& Img = Assets.Images[Index];
        if (Step && !Step(N, Order.size(), Img.Name)) break;
        if (Wants && !Wants(Img.Name)) continue;
        S.Images++;
        if (IsReference(Img))
        {
            S.References++;
            Referenced.insert(Img.Name);
            continue;
        }
        Defined.insert(Img.Name);
        const std::string Path = (fs::path(OutDir) / (SafeFileName(Img.Name) + Ext)).string();
        if (Sink.LinkIfKnown(ContentKey(Zone, Img, Ext), Path, Index)) continue;
        bool FromPackages = false;
        auto Dds = BuildDds(Zone, Img, Packages, FromPackages);
        if (FromPackages) S.FromPackages++;
        if (!Dds)
        {
            S.Missing++;
            if (S.MissingSamples.size() < 12)
            {
                char Why[256] = {};
                snprintf(Why, sizeof(Why), "%s streamed=%d payload=%u at=%lld mips=%u mip_hash=%llx mip_size=%u %ux%u",
                         Img.Name.c_str(), Img.Streamed ? 1 : 0, Img.PayloadSize, (long long)Img.PayloadPos,
                         Img.Mips, (unsigned long long)Img.MipHash, Img.MipSize, Img.MipWidth, Img.MipHeight);
                S.MissingSamples.push_back(Why);
            }
            continue;
        }
        Sink.Write(std::move(Dds), Path);
    }
    const auto Written = Sink.Finish(
        [&](size_t Index) {
            bool FromPackages = false;
            auto Dds = BuildDds(Zone, Assets.Images[Index], Packages, FromPackages);
            if (FromPackages) S.FromPackages++;
            return Dds;
        },
        S.Missing,
        [&](size_t Index) { return ContentKey(Zone, Assets.Images[Index], Ext); });
    S.Written += Written.Written;
    S.Failed += Written.Failed;
    S.Linked += Written.Linked;
    // a reference to an image this zone also holds whole is no dependency
    if (OnReference)
        for (const auto& Name : Referenced)
            if (!Defined.count(Name)) OnReference(Name);
    return S;
}


namespace
{
    // for the image handler below, the zone being served (one per thread)
    thread_local const std::vector<uint8_t>* LiveZoneBytes = nullptr;
    thread_local std::unordered_map<int64_t, const ImageInfo*> LiveImages;

    // CoDAssets' image handler during a disk export: the image the pointer names, read the disk
    // way (GameBlackOps4::LoadXImage would read the runtime-only loaded-mip fields, random on
    // disk - a multi-GB read once)
    std::unique_ptr<XImageDDS> DiskXImage(const XImage_t& Image)
    {
        constexpr uintptr_t Base = VirtualGameSession::Base;
        if (LiveZoneBytes == nullptr || Image.ImagePtr < Base) return nullptr;
        auto It = LiveImages.find(static_cast<int64_t>(Image.ImagePtr - Base));
        if (It == LiveImages.end()) return nullptr;
        bool FromPackages = false;
        return BuildDds(*LiveZoneBytes, *It->second, CoDAssets::GamePackageCache.get(), FromPackages);
    }

}

VirtualGameSession::Handlers LiveHandlers()
{
    static std::once_flag Setup;
    std::call_once(Setup, []() { GameBlackOps4::PerformInitialSetup(); });
    return { DiskStringEntry, DiskXImage };
}

void ServeZone(const std::vector<uint8_t>& Zone, const ZoneAssets& Assets)
{
    ReadZoneStrings(Zone);
    LiveZoneBytes = &Zone;
    LiveImages.clear();
    for (const ImageInfo& I : Assets.Images) LiveImages[I.Pos] = &I;
}

void EndServe()
{
    LiveZoneBytes = nullptr;
    LiveImages.clear();
}

std::string LiveName(uint64_t Hash, const char* Prefix)
{
        const auto Found = GameBlackOps4::AssetNameCache.NameDatabase.find(Hash);
        if (Found != GameBlackOps4::AssetNameCache.NameDatabase.end())
            return FileSystems::GetFileNamePurgeExtensions(Found->second);
        char Fallback[40] = {};
        snprintf(Fallback, sizeof(Fallback), "%s_%llx", Prefix, (unsigned long long)Hash);
        return Fallback;
}

namespace
{
    void NoteFailure(ExportStats& S, const char* Kind, const std::string& Name, int64_t Pos, int Code)
    {
        S.Failed++;
        if (S.MissingSamples.size() >= 12) return;
        char Why[256] = {};
        snprintf(Why, sizeof(Why), "%s %s at %#llx result=%d%s", Kind, Name.c_str(),
                 (unsigned long long)Pos, Code < 0 ? 2 : Code, Code < 0 ? " (crashed)" : "");
        S.MissingSamples.push_back(Why);
    }
}

ExportStats ExportSounds(CoDPackageCache* Packages, const std::string& Zone, const std::string& OutDir,
                         const NameTest& Wants, const StepFn& Step, const HeaderFn& Headers)
{
    ExportStats S;
    CoDFileSystem* FS = Packages ? Packages->GetFileSystem() : nullptr;
    if (FS == nullptr) return S;
    const Bo4SoundBanks::Zone Banks = Bo4SoundBanks::Read(*FS, Zone);
    for (const auto& Why : Banks.Failures) NoteFailure(S, "bank", Why, 0, 2);

    // the entries' samples read from the banks in place (Bo4SoundBanks::Reader) while they are written
    const auto Reader = Bo4SoundBanks::Reader(*FS);
    const ICoDGame* const SavedGame = CoDAssets::Game;
    CoDAssets::Game = Reader.get();
    struct Restore { const ICoDGame* Game; ~Restore() { CoDAssets::Game = Game; } } Restored{ SavedGame };

    const bool UseIndex = SettingsManager::GetSetting("usesabindexbo4", "false") == "true";
    const std::string Extension = ExportWriters::SoundExtension(ExportWriters::SoundFormatSetting());
    std::error_code Ec;
    for (size_t N = 0; N < Banks.Entries.size(); N++)
    {
        const Bo4SoundBanks::Entry& E = Banks.Entries[N];
        const Bo4SoundBanks::Bank& Bank = Banks.Banks[E.Bank];
        const std::string Name = Bo4SoundBanks::NameOf(E.Key, Bank.Language);
        CoDSound_t Sound;
        Sound.AssetName = !Name.empty() ? FileSystems::GetFileNamePurgeExtensions(Name)
                        : UseIndex ? std::format("{}.{}", Bank.File, E.Index) : std::format("xsound_{:x}", E.Key);
        Sound.FullPath = Bo4SoundBanks::Folder(Name, Bank.Language);
        Sound.BankFile = Bank.Path;
        Sound.AssetPointer = E.Offset;
        Sound.AssetSize = E.Size;
        Sound.FrameRate = E.Rate;
        Sound.FrameCount = E.Frames;
        Sound.ChannelsCount = E.Channels;
        Sound.Length = E.Rate ? (uint32_t)(1000.0 * E.Frames / E.Rate) : 0;
        Sound.DataType = SoundDataTypes::FLAC_WithHeader;
        Sound.AssetType = WraithAssetType::Sound;
        Sound.AssetStatus = WraithAssetStatus::Loaded;
        Sound.IsFileEntry = true;
        if (Step && !Step(N, Banks.Entries.size(), Sound.AssetName)) break;
        if (Wants && !Wants(Sound.AssetName)) continue;
        // a placeholder: no bank of its language holds samples for that key
        if (E.Size == 0)
        {
            S.Empty++;
            continue;
        }
        S.Images++;
        const fs::path Dir = fs::path(OutDir) / Sound.FullPath;
        if (Headers)
        {
            // a dry run: the entry as its bank's table gives it (a whole FLAC file)
            DiskGame::SoundHeader H;
            H.Name = (Dir / Sound.AssetName).lexically_relative(OutDir).string();
            H.Is = DiskGame::SoundHeader::Form::Flac;
            H.Stored = E.Size;
            H.Rate = E.Rate, H.Channels = E.Channels, H.Frames = E.Frames;
            Headers(H);
            S.Written++;
            continue;
        }
        fs::create_directories(Dir, Ec);
        std::string Crash;
        const int Code = ExportWriters::ExportSound(Sound, Dir.string(), Extension, Crash);
        if (Code == static_cast<int>(ExportGameResult::Success)) S.Written++;
        else NoteFailure(S, "sound", Sound.AssetName, static_cast<int64_t>(E.Offset), Code);
    }
    return S;
}

std::vector<MaterialImage> MaterialImages(const std::vector<uint8_t>& Zone, const ZoneAssets& Assets)
{
    // BO4XMaterial: name hash at 0, image table pointer at 0x38, image count u8 at 0x130; each
    // table entry (0x20) is BO4XMaterialImage: image pointer at 0, semantic hash u32 at 8
    std::unordered_map<int64_t, int64_t> Link(Assets.Links.begin(), Assets.Links.end());
    std::vector<MaterialImage> Out;
    for (const auto& [Hash, Pos] : Assets.Materials)
    {
        const auto Table = Link.find(Pos + 0x38);
        if (Table == Link.end()) continue;
        const uint8_t Count = At<uint8_t>(Zone, Pos + 0x130);
        for (uint32_t K = 0; K < Count; K++)
        {
            const int64_t Entry = Table->second + 0x20 * static_cast<int64_t>(K);
            MaterialImage M;
            M.Material = Hash;
            M.Slot = K;
            M.Semantic = At<uint32_t>(Zone, Entry + 8);
            const auto Img = Link.find(Entry);
            if (Img != Link.end()) M.Image = At<uint64_t>(Zone, Img->second + 0x20) & 0xFFFFFFFFFFFFFFFull;
            Out.push_back(M);
        }
    }
    return Out;
}

namespace
{
    // the common zones a zone's level loads before it (CommonMaterials), not itself
    std::vector<std::string> CommonZonesOf(const std::string& Zone)
    {
        const std::string S = Lower(fs::path(Zone).stem().string());
        std::vector<std::string> Out;
        if (S.find("core_common") == std::string::npos) Out.push_back("core_common");
        for (const std::string Mode : { "zm_", "mp_", "wz_" })
            if ((S.rfind(Mode, 0) == 0 || S.find("_" + Mode) != std::string::npos) && S.find(Mode + "common") == std::string::npos)
                Out.push_back(Mode + "common");
        return Out;
    }

    // the zone's full materials; the first occurrence of a name gives its images
    MaterialBook BookOf(const std::vector<uint8_t>& Zone, const ZoneAssets& Assets)
    {
        MaterialBook Book;
        std::vector<std::pair<uint64_t, uint32_t>>* Open = nullptr;
        for (const auto& M : MaterialImages(Zone, Assets))
        {
            if (M.Slot == 0)
            {
                const auto [It, New] = Book.try_emplace(M.Material);
                Open = New ? &It->second.Images : nullptr;
            }
            if (Open != nullptr && M.Image != 0) Open->emplace_back(M.Image, M.Semantic);
        }
        for (auto It = Book.begin(); It != Book.end();) It = It->second.Images.empty() ? Book.erase(It) : std::next(It);
        // the technique set (+0x30, its name hash at 0) and the constant blob (+0x120 size, +0x128 bytes)
        std::unordered_map<int64_t, int64_t> Link(Assets.Links.begin(), Assets.Links.end());
        for (const auto& [Hash, Pos] : Assets.Materials)
        {
            const auto Entry = Book.find(Hash);
            if (Entry == Book.end() || Entry->second.Techset != 0) continue;
            if (const auto Ts = Link.find(Pos + 0x30); Ts != Link.end()) Entry->second.Techset = At<uint64_t>(Zone, Ts->second) & 0xFFFFFFFFFFFFFFFull;
            const uint64_t Size = At<uint64_t>(Zone, Pos + 0x120);
            const auto Data = Link.find(Pos + 0x128);
            if (Data != Link.end() && Size > 0 && Size <= 0x4000 && Data->second + static_cast<int64_t>(Size) <= static_cast<int64_t>(Zone.size()))
                Entry->second.Constants.assign(Zone.begin() + Data->second, Zone.begin() + Data->second + static_cast<int64_t>(Size));
        }
        return Book;
    }

    bool LoadBook(const std::string& Path, MaterialBook& Book)
    {
        std::ifstream In(Path, std::ios::binary);
        uint64_t Count = 0;
        if (!In.read(reinterpret_cast<char*>(&Count), 8)) return false;
        for (uint64_t K = 0; K < Count; K++)
        {
            uint64_t Hash = 0;
            uint32_t N = 0;
            if (!In.read(reinterpret_cast<char*>(&Hash), 8) || !In.read(reinterpret_cast<char*>(&N), 4)) return false;
            auto& M = Book[Hash];
            M.Images.resize(N);
            for (auto& [Image, Semantic] : M.Images)
                if (!In.read(reinterpret_cast<char*>(&Image), 8) || !In.read(reinterpret_cast<char*>(&Semantic), 4)) return false;
            uint32_t Bytes = 0;
            if (!In.read(reinterpret_cast<char*>(&M.Techset), 8) || !In.read(reinterpret_cast<char*>(&Bytes), 4) || Bytes > 0x4000) return false;
            M.Constants.resize(Bytes);
            if (Bytes && !In.read(reinterpret_cast<char*>(M.Constants.data()), Bytes)) return false;
        }
        return true;
    }

    void SaveBook(const std::string& Path, const MaterialBook& Book)
    {
        const std::string Temp = Path + ".tmp";
        {
            std::ofstream Out(Temp, std::ios::binary | std::ios::trunc);
            const uint64_t Count = Book.size();
            Out.write(reinterpret_cast<const char*>(&Count), 8);
            for (const auto& [Hash, M] : Book)
            {
                const uint32_t N = static_cast<uint32_t>(M.Images.size());
                Out.write(reinterpret_cast<const char*>(&Hash), 8);
                Out.write(reinterpret_cast<const char*>(&N), 4);
                for (const auto& [Image, Semantic] : M.Images)
                {
                    Out.write(reinterpret_cast<const char*>(&Image), 8);
                    Out.write(reinterpret_cast<const char*>(&Semantic), 4);
                }
                const uint32_t Bytes = static_cast<uint32_t>(M.Constants.size());
                Out.write(reinterpret_cast<const char*>(&M.Techset), 8);
                Out.write(reinterpret_cast<const char*>(&Bytes), 4);
                if (Bytes) Out.write(reinterpret_cast<const char*>(M.Constants.data()), Bytes);
            }
            if (!Out) return;
        }
        std::error_code Ec;
        fs::rename(Temp, Path, Ec);
    }
}

std::vector<std::shared_ptr<const MaterialBook>> CommonMaterials(const std::string& Zone, CoDPackageCache* Packages,
                                                                 const std::string& FfCache, const std::string& BookDir)
{
    static std::mutex Lock;
    static std::map<std::string, std::shared_ptr<const MaterialBook>> Kept;   // by book file
    std::vector<std::shared_ptr<const MaterialBook>> Out;
    std::string Error;
    const ExportWriters::LoadedProfile* Profile = ExportWriters::ZoneProfile("bo4", Error);
    if (Profile == nullptr) return Out;
    const std::string Key = ProfileKey(*Profile);
    std::error_code Ec;
    fs::create_directories(BookDir, Ec);
    for (const std::string& Name : CommonZonesOf(Zone))
    {
        // the common zone: beside this one, else from the packages
        std::string Ff = (fs::path(Zone).parent_path() / (Name + ".ff")).string();
        if (!fs::exists(Ff, Ec))
        {
            const auto Pulled = Packages ? TreyarchPackages::ExtractFastFiles(Packages, Name + ".ff", FfCache, Error) : std::vector<std::string>();
            if (Pulled.empty()) continue;
            Ff = Pulled.front();
        }
        const fs::path Fd = fs::path(Ff).replace_extension(".fd");
        const uint64_t Sizes = fs::file_size(Ff, Ec) * 31 + (fs::exists(Fd, Ec) ? fs::file_size(Fd, Ec) : 0);
        const std::string File = (fs::path(BookDir) / (Name + "_" + Key + "_" + std::to_string(Sizes) + ".v2.book")).string();
        std::lock_guard<std::mutex> Guard(Lock);
        if (const auto It = Kept.find(File); It != Kept.end())
        {
            Out.push_back(It->second);
            continue;
        }
        auto Book = std::make_shared<MaterialBook>();
        if (!LoadBook(File, *Book))
        {
            Book->clear();
            std::vector<uint8_t> Bytes;
            ZoneAssets Assets;
            if (!LoadZone(Ff, Bytes, Error) || !ReadZone(Bytes, "bo4", Assets, Error)) continue;
            *Book = BookOf(Bytes, Assets);
            SaveBook(File, *Book);
        }
        Kept[File] = Book;
        Out.push_back(std::move(Book));
    }
    return Out;
}

std::vector<AssetRef> AssetRefs(const ZoneAssets& Assets)
{
    std::unordered_map<int64_t, const std::string*> RootAt;
    for (const auto& [Type, Pos] : Assets.Roots) RootAt.emplace(Pos, &Type);
    // zone-level assets in zone order: the owner of a field is the last one starting at or before it
    std::vector<std::pair<int64_t, const std::string*>> Top;
    for (const auto& [Type, Pos] : Assets.Assets) Top.emplace_back(Pos, &Type);
    std::sort(Top.begin(), Top.end());
    std::vector<AssetRef> Out;
    for (const auto& [Field, Target] : Assets.Links)
    {
        const auto T = RootAt.find(Target);
        if (T == RootAt.end()) continue;
        auto O = std::upper_bound(Top.begin(), Top.end(), std::make_pair(Field, static_cast<const std::string*>(nullptr)),
                                  [](const auto& A, const auto& B) { return A.first < B.first; });
        if (O == Top.begin()) continue;
        --O;
        if (O->first == Target) continue;          // a pointer inside an asset to itself
        Out.push_back({*O->second, O->first, *T->second, Target});
    }
    return Out;
}
}
