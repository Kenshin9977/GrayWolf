#include "stdafx.h"

#include "Bo4SoundBanks.h"

#include "CoDAssetType.h"
#include "CoDFileSystem.h"
#include "DBGameFiles.h"
#include "ExportNaming.h"
#include "FileSystems.h"
#include "ICoDGame.h"
#include "WraithNameIndex.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <unordered_map>

namespace fs = std::filesystem;

namespace Bo4SoundBanks
{
namespace
{
    constexpr uint64_t KeyMask = 0x7FFFFFFFFFFFFFFFull;
    constexpr uint32_t Magic = 0x23585532;   // '2UX#'
    constexpr uint32_t Version = 0x15;

    bool ReadAt(CoDFileSystem& FS, HANDLE H, uint64_t Pos, void* Out, size_t Size)
    {
        if (FS.Seek(H, (size_t)Pos, FILE_BEGIN) != Pos) return false;
        return FS.Read(H, static_cast<uint8_t*>(Out), 0, Size) == Size;
    }

    // zm_red.fr.p.sabs -> fr
    std::string LanguageOf(const std::string& File)
    {
        const size_t A = File.find('.'), B = A == std::string::npos ? A : File.find('.', A + 1);
        return B == std::string::npos ? std::string("all") : ExportNaming::Lower(File.substr(A + 1, B - A - 1));
    }

    bool ReadBank(CoDFileSystem& FS, const Bank& B, size_t BankIndex, std::vector<Entry>& Out, std::string& Why)
    {
        HANDLE H = FS.OpenFile(B.Path, "r");
        if (H == NULL || H == INVALID_HANDLE_VALUE)
        {
            Why = "does not open";
            return false;
        }
        const uint64_t Length = FS.Size(H);
        SABFileHeader Header{};
        bool Ok = ReadAt(FS, H, 0, &Header, sizeof(Header));
        if (!Ok) Why = "no header";
        else if (Header.Magic != Magic || Header.Version != Version || Header.SizeOfAudioEntry != sizeof(SABv21Entry))
        {
            Why = std::format("magic {:#x} version {:#x} entry {:#x}: not a Black Ops 4 bank", Header.Magic, Header.Version, Header.SizeOfAudioEntry);
            Ok = false;
        }
        else if (Header.EntryTableOffset + (uint64_t)Header.EntriesCount * sizeof(SABv21Entry) > Length)
        {
            Why = std::format("{} entries past its end", Header.EntriesCount);
            Ok = false;
        }
        std::vector<SABv21Entry> Table(Ok ? Header.EntriesCount : 0);
        if (Ok && !Table.empty() && !ReadAt(FS, H, Header.EntryTableOffset, Table.data(), Table.size() * sizeof(SABv21Entry)))
        {
            Why = "entry table unread";
            Ok = false;
        }
        FS.CloseHandle(H);
        if (!Ok) return false;

        constexpr uint32_t Rates[] = { 8000, 12000, 16000, 24000, 32000, 44100, 48000, 96000, 192000 };
        size_t Past = 0;
        for (uint32_t I = 0; I < Table.size(); I++)
        {
            const SABv21Entry& T = Table[I];
            Entry E;
            E.Key = T.Key & KeyMask;
            E.Offset = T.Offset;
            E.Size = T.Size;
            E.Frames = T.FrameCount;
            E.Rate = T.FrameRateIndex < std::size(Rates) ? Rates[T.FrameRateIndex] : 0;
            E.Channels = T.ChannelCount;
            E.Bank = BankIndex;
            E.Index = I;
            if (E.Size > 0 && E.Offset + E.Size > Length)
            {
                Past++;
                E.Size = 0;
            }
            Out.push_back(E);
        }
        if (Past) Why = std::format("{} entries' samples past its end", Past);
        return true;
    }

    const WraithNameIndex& Index()
    {
        static WraithNameIndex Names;
        static std::once_flag Once;
        std::call_once(Once, [] {
            const fs::path Dir = fs::path(FileSystems::GetApplicationPath()) / "package_index";
            // bo4_sab.wni: the original Greyhound's BO4 sound index (not shipped today); the
            // community's fnv1a_xsounds.wni names BO4 bank keys among other games' ids
            Names.LoadIndex((Dir / "bo4_sab.wni").string(), KeyMask);
            Names.LoadIndex((Dir / "fnv1a_xsounds.wni").string(), KeyMask);
        });
        return Names;
    }

    // a language's keys named through the index's paths of another language: Hash(<lang>\rest)
    // for each indexed "<xx>\rest" whose own key is its hash (a Black Ops 4 sound path)
    const std::unordered_map<uint64_t, std::string>& LanguageNames(const std::string& Language)
    {
        static std::mutex Lock;
        static std::map<std::string, std::unordered_map<uint64_t, std::string>> ByLanguage;
        std::lock_guard<std::mutex> Guard(Lock);
        const auto Found = ByLanguage.find(Language);
        if (Found != ByLanguage.end()) return Found->second;
        auto& Out = ByLanguage[Language];
        for (const auto& [Key, Name] : Index().NameDatabase)
        {
            if (Name.size() < 4 || (Name[2] != '\\' && Name[2] != '/')) continue;
            if (!std::isalpha((unsigned char)Name[0]) || !std::isalpha((unsigned char)Name[1])) continue;
            if (ExportNaming::Lower(Name.substr(0, 2)) == Language || Hash(Name) != Key) continue;
            std::string Path = Language + "\\" + Name.substr(3);
            Out.emplace(Hash(Path), std::move(Path));
        }
        return Out;
    }

    class BankReader final : public ICoDGame
    {
    public:
        explicit BankReader(CoDFileSystem& FS) : FS(FS) {}
        ~BankReader() override
        {
            for (const auto& [Path, H] : Open) FS.CloseHandle(H);
        }
        const char* Engine() const override { return "T8"; }
        std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound) const override
        {
            std::lock_guard<std::mutex> Guard(Lock);
            auto It = Open.find(Sound->BankFile);
            if (It == Open.end())
            {
                HANDLE H = FS.OpenFile(Sound->BankFile, "r");
                if (H == NULL || H == INVALID_HANDLE_VALUE) return nullptr;
                It = Open.emplace(Sound->BankFile, H).first;
            }
            auto Result = std::make_unique<XSound>();
            Result->DataBuffer = new int8_t[(size_t)Sound->AssetSize];
            Result->DataSize = (uint32_t)Sound->AssetSize;
            Result->DataType = SoundDataTypes::FLAC_WithHeader;
            Result->FrameRate = Sound->FrameRate;
            Result->Channels = Sound->ChannelsCount;
            Result->FrameCount = Sound->FrameCount;
            if (!ReadAt(FS, It->second, Sound->AssetPointer, Result->DataBuffer, (size_t)Sound->AssetSize)) return nullptr;
            return Result;
        }

    private:
        CoDFileSystem& FS;
        mutable std::mutex Lock;
        mutable std::map<std::string, HANDLE> Open;
    };
}

uint64_t Hash(const std::string& Path)
{
    std::string S = ExportNaming::Lower(Path);
    std::replace(S.begin(), S.end(), '/', '\\');
    return ExportNaming::Fnv1a(S) & KeyMask;
}

Zone Read(CoDFileSystem& FS, const std::string& ZoneName)
{
    Zone Z;
    const std::string Prefix = ExportNaming::Lower(ZoneName) + ".";   // zm_red.all.sabl, zm_red.fr.p.sabs...
    for (const char* Pattern : { "*.sabl", "*.sabs" })
    {
        FS.EnumerateFiles(Pattern, [&](const std::string& Name, const size_t) {
            const std::string File = fs::path(Name).filename().string();
            // the pattern also matches zm_escape.en.sabs_1 and _2, pieces of the whole .sabs beside them
            const std::string Extension = ExportNaming::Lower(fs::path(File).extension().string());
            if (ExportNaming::Lower(File).rfind(Prefix, 0) != 0 || (Extension != ".sabl" && Extension != ".sabs")) return;
            Z.Banks.push_back({ Name, File, LanguageOf(File.substr(Prefix.size() - 1)) });
        });
    }
    std::sort(Z.Banks.begin(), Z.Banks.end(), [](const Bank& A, const Bank& B) { return ExportNaming::Lower(A.File) < ExportNaming::Lower(B.File); });

    std::vector<Entry> All;
    for (size_t B = 0; B < Z.Banks.size(); B++)
    {
        std::string Why;
        if (!ReadBank(FS, Z.Banks[B], B, All, Why) || !Why.empty()) Z.Failures.push_back(Z.Banks[B].File + ": " + Why);
    }
    Z.Read = All.size();

    // one entry per (language, key): the most bytes, the first bank on a tie
    std::map<std::pair<std::string, uint64_t>, size_t> Kept;
    std::vector<bool> Keep(All.size(), false);
    for (size_t I = 0; I < All.size(); I++)
    {
        const auto [It, New] = Kept.try_emplace({ Z.Banks[All[I].Bank].Language, All[I].Key }, I);
        if (New) Keep[I] = true;
        else if (All[I].Size > All[It->second].Size)
        {
            Keep[It->second] = false;
            Keep[I] = true;
            It->second = I;
        }
    }
    for (size_t I = 0; I < All.size(); I++)
        if (Keep[I]) Z.Entries.push_back(All[I]);
    Z.Duplicates = All.size() - Z.Entries.size();
    std::stable_sort(Z.Entries.begin(), Z.Entries.end(), [](const Entry& A, const Entry& B) {
        return A.Bank != B.Bank ? A.Bank < B.Bank : A.Offset < B.Offset;
    });
    return Z;
}

std::string NameOf(uint64_t Key, const std::string& Language)
{
    const auto& Db = Index().NameDatabase;
    if (const auto Found = Db.find(Key); Found != Db.end()) return Found->second;
    if (Language.empty() || Language == "all") return "";
    const auto& Derived = LanguageNames(Language);
    const auto Found = Derived.find(Key);
    return Found != Derived.end() ? Found->second : std::string();
}

std::string Folder(const std::string& Name, const std::string& Language)
{
    const std::string Dir = FileSystems::GetDirectoryName(Name);
    if (Language.empty() || Language == "all") return Dir;
    std::string Out = Language;
    bool First = true;
    size_t Start = 0;
    while (Start < Dir.size())
    {
        size_t End = Dir.find_first_of("\\/", Start);
        if (End == std::string::npos) End = Dir.size();
        const std::string Part = Dir.substr(Start, End - Start);
        // a language's path starts with its folder: written under it once
        if (!Part.empty() && !(std::exchange(First, false) && ExportNaming::Lower(Part) == Language)) Out += "\\" + Part;
        Start = End + 1;
    }
    return Out;
}

std::unique_ptr<ICoDGame> Reader(CoDFileSystem& FS)
{
    return std::make_unique<BankReader>(FS);
}
}
