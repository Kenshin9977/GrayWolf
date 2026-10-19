#include "stdafx.h"

#include "ExportWriters.h"

#include "Bo3ZoneSounds.h"

#include "ExportNaming.h"
#include "SABSupport.h"
#include "DBGameFiles.h"
#include "CoDAssetType.h"
#include "BinaryReader.h"
#include "WraithNameIndex.h"
#include "FileSystems.h"
#include "Strings.h"
#include "Sound.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    using ExportNaming::Lower;

    // Keeps real subfolders (a bank entry's name is a full relative path, e.g.
    // "amb\alarms\air_raid\...") but strips characters Windows can't put in ONE path component -
    // unlike Bo3ZoneMaterials.cpp's SafeFileName, this must not flatten the separators away.
    std::string SanitizeComponent(std::string S)
    {
        for (auto& C : S)
            if (C == ':' || C == '*' || C == '?' || C == '"' || C == '<' || C == '>' || C == '|')
                C = '_';
        return S;
    }

    struct SabEntry
    {
        std::string Name;   // purged of extensions, e.g. "air_raid_horn_00_l"
        std::string SubDir; // e.g. "amb\alarms\air_raid", as stored
        uint64_t Offset = 0;
        uint32_t Size = 0;
        uint32_t FrameRate = 0;
        uint32_t FrameCount = 0;
        uint8_t Channels = 0;
        uint32_t Key = 0;
    };

    uint32_t FrameRateForIndex(uint8_t Index)
    {
        // Same table SABSupport::HandleSABv15 uses.
        switch (Index)
        {
        case 0: return 8000;
        case 1: return 12000;
        case 2: return 16000;
        case 3: return 24000;
        case 4: return 32000;
        case 5: return 44100;
        case 6: return 48000;
        case 7: return 96000;
        case 8: return 192000;
        default: return 0;
        }
    }

    // Local reproduction of SABSupport::HandleSABv15's header/name-table/entry scan, not
    // SABSupport::ParseSAB itself (see Bo3ZoneSounds.h).
    bool ScanSabV15(const std::string& BankPath, std::vector<SabEntry>& Out, std::string& Reason)
    {
        BinaryReader Reader;
        if (!Reader.Open(BankPath))
        {
            Reason = "cannot open bank";
            return false;
        }

        // Per-file zm_genesis Salsa20 decrypt, the path ParseSAB/LoadSound use.
        SABSupport::HandleSABEncryption(Reader, BankPath);

        const auto Header = Reader.Read<SABFileHeader>();

        if (Header.Magic != 0x23585532)
        {
            Reason = "not a SAB file";
            return false;
        }
        if (Header.Version != 0xF)
        {
            Reason = "unexpected SAB version (not BO3/v15)";
            return false;
        }
        if (Header.EntriesCount > 5000000)
        {
            Reason = "implausible entries count";
            return false;
        }

        Reader.SetPosition(0x250);
        const auto NameTableOffset = Reader.Read<uint64_t>();
        const bool HasNames = (NameTableOffset > 0) && (Reader.Read<uint64_t>(NameTableOffset) != 0);

        std::vector<std::string> Names;
        WraithNameIndex NameIndex;
        if (!HasNames)
        {
            NameIndex.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\bo3_sab.wni").string());
        }
        else
        {
            Names.reserve(Header.EntriesCount);
            Reader.SetPosition(NameTableOffset);
            for (uint32_t i = 0; i < Header.EntriesCount; i++)
            {
                auto Name = Reader.ReadNullTerminatedString();
                const auto AdvanceBy = ((size_t)Header.SizeOfNameEntry * 2) - (Name.size() + 1);
                Names.push_back(std::move(Name));
                Reader.Advance(AdvanceBy);
            }
        }

        Reader.SetPosition(Header.EntryTableOffset);
        Out.reserve(Header.EntriesCount);

        for (uint32_t i = 0; i < Header.EntriesCount; i++)
        {
            const auto Entry = Reader.Read<SABv15Entry>();

            if (Entry.Size == 0)
            {
                continue; // nothing to export
            }

            std::string EntryName;
            if (HasNames && i < Names.size())
            {
                EntryName = Names[i];
            }
            else if (NameIndex.NameDatabase.find(Entry.Key) != NameIndex.NameDatabase.end())
            {
                EntryName = NameIndex.NameDatabase.at(Entry.Key);
            }
            else
            {
                EntryName = std::format("_{:x}", Entry.Key);
            }

            SabEntry E;
            E.Name = FileSystems::GetFileNamePurgeExtensions(EntryName);
            E.SubDir = FileSystems::GetDirectoryName(EntryName);
            E.Offset = Entry.Offset;
            E.Size = Entry.Size;
            E.FrameRate = FrameRateForIndex(Entry.FrameRateIndex);
            E.FrameCount = Entry.FrameCount;
            E.Channels = Entry.ChannelCount;
            E.Key = Entry.Key;
            Out.push_back(std::move(E));
        }

        return true;
    }
}

std::string Bo3SoundFormat(const std::string& AudioFormat)
{
    const std::string F = Strings::ToUpper(AudioFormat);
    return F == "WAV" || F == "FLAC" || F == "OPUS" ? F : ExportWriters::SoundFormatSetting();
}

size_t Bo3ExportZoneSounds(const std::string& FastFilePath, const std::string& OutDir,
                            const std::string& NameFilter, const std::string& AudioFormat,
                            std::string& Report, std::unordered_map<uint32_t, std::string>* WrittenKeys,
                            const std::function<void(const DiskGame::SoundHeader&)>& Headers)
{
    const fs::path ZoneFile(FastFilePath);
    const std::string ZoneStem = ZoneFile.stem().string();
    const fs::path SoundRoot = ZoneFile.parent_path() / "snd";

    std::error_code Ec;
    if (!fs::is_directory(SoundRoot, Ec))
    {
        Report = "result=ok\nbanks=0\nsounds_exported=0\nreason=no zone/snd directory\n";
        return 0;
    }

    // Locale subfolders ("all", "en", ...) plus the root itself - same shape
    // GameBlackOps3::LoadAssets' own disk scan uses.
    std::vector<fs::path> LocaleDirs{ SoundRoot };
    for (const auto& Sub : fs::directory_iterator(SoundRoot, Ec))
    {
        if (!Ec && Sub.is_directory(Ec)) LocaleDirs.push_back(Sub.path());
    }

    // Every bank whose stem matches the zone (case-insensitive), across every locale dir, BOTH
    // extensions (.sabl and .sabs).
    std::vector<fs::path> Banks;
    const std::string StemLowerDot = Lower(ZoneStem) + ".";
    for (const auto& Dir : LocaleDirs)
    {
        for (const auto& Entry : fs::directory_iterator(Dir, Ec))
        {
            if (Ec || !Entry.is_regular_file(Ec)) continue;
            const auto Ext = Lower(Entry.path().extension().string());
            if (Ext != ".sabl" && Ext != ".sabs") continue;

            const auto NameLower = Lower(Entry.path().filename().string());
            if (NameLower.rfind(StemLowerDot, 0) != 0) continue; // e.g. avoid "..._2"'s banks

            Banks.push_back(Entry.path());
        }
    }
    std::sort(Banks.begin(), Banks.end());

    if (Banks.empty())
    {
        Report = "result=ok\nbanks=0\nsounds_exported=0\n";
        return 0;
    }

    const ExportNaming::NameFilter Filter(NameFilter);

    // the bank entries are FLAC files: kept under FLAC, decoded to WAV, encoded to Opus
    const std::string Format = Bo3SoundFormat(AudioFormat);
    const std::string OutExt = ExportWriters::SoundExtension(Format, ".flac");

    if (!Headers) fs::create_directories(OutDir, Ec);

    size_t Candidates = 0, Matched = 0, Written = 0, WriteFailed = 0, DecodeFailed = 0, DedupSkipped = 0;
    std::set<std::string> WrittenPaths; // de-dup across banks for this zone (plain + .p variant, etc.)
    std::string BankFailures;

    for (const auto& Bank : Banks)
    {
        std::vector<SabEntry> Entries;
        std::string ScanReason;
        if (!ScanSabV15(Bank.string(), Entries, ScanReason))
        {
            BankFailures += "bank_failed=" + Bank.filename().string() + " reason=" + ScanReason + "\n";
            continue;
        }

        // Immediate parent folder name = this bank's locale ("all"/"en"/...).
        const auto LocaleDirName = Bank.parent_path().filename().string();

        for (const auto& E : Entries)
        {
            Candidates++;

            if (!Filter.Matches(E.Name)) continue;
            Matched++;

            fs::path OutPath = fs::path(OutDir);
            if (!LocaleDirName.empty()) OutPath /= SanitizeComponent(LocaleDirName);
            if (!E.SubDir.empty())
            {
                for (const auto& Part : Strings::SplitString(E.SubDir, '\\', true))
                {
                    for (const auto& Part2 : Strings::SplitString(Part, '/', true))
                    {
                        OutPath /= SanitizeComponent(Part2);
                    }
                }
            }
            OutPath /= (SanitizeComponent(E.Name) + OutExt);

            const std::string OutPathStr = OutPath.string();
            const auto Remember = [&] {
                if (WrittenKeys && E.Key != 0)
                {
                    std::string Rel = fs::path(OutPathStr).lexically_relative(OutDir).string();
                    std::replace(Rel.begin(), Rel.end(), '/', '\\');
                    WrittenKeys->emplace(E.Key, std::move(Rel));
                }
            };
            if (!WrittenPaths.insert(OutPathStr).second)
            {
                Remember();
                DedupSkipped++;
                continue;
            }

            if (Headers)
            {
                // a dry run: the entry as its bank header says (a whole FLAC file)
                DiskGame::SoundHeader H;
                H.Name = fs::path(OutPathStr).lexically_relative(OutDir).replace_extension().string();
                H.Is = DiskGame::SoundHeader::Form::Flac;
                H.Stored = E.Size;
                H.Rate = E.FrameRate, H.Channels = E.Channels, H.Frames = E.FrameCount;
                Headers(H);
                Written++;
                continue;
            }
            std::error_code MkEc;
            fs::create_directories(OutPath.parent_path(), MkEc);

            CoDSound_t Local;
            Local.AssetName = E.Name;
            Local.FullPath = E.SubDir;
            Local.BankFile = Bank.string();
            Local.AssetPointer = E.Offset;
            Local.AssetSize = E.Size;
            Local.FrameRate = E.FrameRate;
            Local.FrameCount = E.FrameCount;
            Local.ChannelsCount = E.Channels;
            // Every v15 entry is already a complete, standalone FLAC file.
            Local.DataType = SoundDataTypes::FLAC_WithHeader;

            auto Decoded = SABSupport::LoadSound(&Local);
            if (Decoded == nullptr || Decoded->DataBuffer == nullptr || Decoded->DataSize == 0)
            {
                DecodeFailed++;
                continue;
            }

            const bool Ok = ExportWriters::WriteSound(ExportWriters::SourceOf(*Decoded), OutPath, Format);

            if (Ok)
            {
                Written++;
                Remember();
            }
            else WriteFailed++;
        }
    }

    Report = std::format(
        "result=ok\nbanks={}\ncandidates={}\nmatched={}\nsounds_exported={}\n"
        "decode_failed={}\nwrite_failed={}\ndedup_skipped={}\n",
        Banks.size(), Candidates, Matched, Written, DecodeFailed, WriteFailed, DedupSkipped);
    Report += BankFailures;

    return Written;
}
