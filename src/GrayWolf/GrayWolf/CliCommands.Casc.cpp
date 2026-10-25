// NOMINMAX before <windows.h>: std::max/std::min below would be mangled by its macros.
#define NOMINMAX

#include <stdio.h>
#include <cmath>
#include <memory>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <vector>
#include <iterator>
#include <string>
#include <format>

#include "CliOptions.h"
#include "CliCommands.h"

#include <CLI/CLI.hpp>
#include "CliSupport.h"

#include "WraithX.h"
#include "CoDAssets.h"
#include "FastFile.h"

#include "SABSupport.h"
#include "CascFileSystem.h"
#include "WinFileSystem.h"
#include "CoDFileHandle.h"
#include "DBGameFiles.h"
#include "Siren.h"
#include "XSUBCache.h"
#include "Games/Treyarch/TreyarchPackages.h"

#include <cstring>
#include <map>

// Diagnostic: opens a CASC storage (a directory containing .build.info) directly and lists
// every entry matching --casc-pattern (default *.sab*).
int RunCascList(const CliOptions& Options)
{
    CascFileSystem FS(Options.CascList);
    if (!FS.IsValid())
    {
        CliReport(Options, std::format("result=error\nreason=could not open CASC storage\ncode={}\n", FS.GetLastError()));
        return 5;
    }

    const std::string Pattern = Options.CascPattern.empty() ? "*.sab*" : Options.CascPattern;
    std::string Report = "result=ok\npattern=" + Pattern + "\n";
    size_t Count = 0;
    FS.EnumerateFilesWithKey(Pattern, [&](const std::string& Name, const size_t Size, const std::string& Key)
    {
        uint64_t Content = 0, Encoded = 0;
        std::string Spans;
        FS.StorageInfo(Name, Content, Encoded, Spans);
        Report += std::format("{}\tsize={}\tckey={}\tcontent={}\tencoded={}\tspans={}\n", Name.c_str(), Size,
                                  Key.c_str(), (unsigned long long)Content, (unsigned long long)Encoded, Spans.c_str());
        Count++;
    });
    Report += std::format("count={}\n", Count);

    CliReport(Options, Report);
    return 0;
}


// Diagnostic: every .xpak's own index as TSV (package, key, offset, stored size), to account for
// what the streamed data is made of and which objects a later package supersedes.
int RunXpakIndex(const CliOptions& Options)
{
    CascFileSystem FS(Options.XpakIndex);
    if (!FS.IsValid() || Options.Out.empty())
    {
        CliReport(Options, "result=error\nreason=needs a CASC storage directory and --out <file>\n");
        return 5;
    }
    std::vector<std::string> Names;
    FS.EnumerateFiles("*.xpak", [&](const std::string& Name, const size_t) { Names.push_back(Name); });
    std::ofstream Out(Options.Out, std::ios::binary);
    size_t Packages = 0, Entries = 0, Skipped = 0;
    for (const auto& Name : Names)
    {
        CoDFileHandle Reader(FS.OpenFile(Name, "r"), &FS);
        if (!Reader.IsValid()) { Skipped++; continue; }
        const auto Header = Reader.Read<BO3XPakHeader>();
        if (Header.Magic != 0x4950414b || Header.HashOffset >= Reader.Size()) { Skipped++; continue; }
        Reader.Seek(Header.HashOffset, SEEK_SET);
        std::vector<BO3XPakHashEntry> Table((size_t)Header.HashCount);
        Reader.Read(reinterpret_cast<uint8_t*>(Table.data()), Table.size() * sizeof(BO3XPakHashEntry));
        for (const auto& E : Table)
            Out << Name << '\t' << std::hex << E.Key << std::dec << '\t' << E.Offset << '\t'
                << (E.Size & 0xFFFFFFFFFFFFFFull) << '\n';
        Packages++;
        Entries += Table.size();
    }
    CliReport(Options, std::format("result=ok\npackages={}\nentries={}\nskipped={}\n", Packages, Entries, Skipped));
    return 0;
}

// Diagnostic: every .xsub's hash table as TSV (package, header type, key, offset, stored size, the
// packed word's low bit; version 23 adds the object's size), read through CASC (Cold War) or a
// plain install (Modern Warfare III, Black Ops 6 and 7) without extracting the packages. Same
// decoding as XSUBCache::LoadPackage: offset = (PackedInfo >> 32) << 7, stored size =
// (PackedInfo >> 1) & 0x3FFFFFFF. Every package is listed, not only the data ones (type 3), so
// what the others index is visible too; the report gives the per-type package counts.
int RunXsubIndex(const CliOptions& Options)
{
#pragma pack(push, 1)
    struct XSubHeader
    {
        uint32_t Magic;
        uint16_t Unknown1;
        uint16_t Version;
        uint64_t Unknown;
        uint64_t Type;
        uint64_t Size;
        uint8_t UnknownHashes[1896];
        int64_t FileCount;
        int64_t DataOffset;
        int64_t DataSize;
        int64_t HashCount;
        int64_t HashOffset;
        int64_t HashSize;
    };
    struct XSubHashEntry
    {
        uint64_t Key;
        uint64_t PackedInfo;
    };
#pragma pack(pop)
    std::error_code BuildInfoEc;
    std::unique_ptr<CoDFileSystem> FileSystem;
    if (std::filesystem::is_regular_file(std::filesystem::path(Options.XsubIndex) / ".build.info", BuildInfoEc))
        FileSystem = std::make_unique<CascFileSystem>(Options.XsubIndex);
    else
        FileSystem = std::make_unique<WinFileSystem>(Options.XsubIndex);
    CoDFileSystem& FS = *FileSystem;
    if (!FS.IsValid() || Options.Out.empty())
    {
        CliReport(Options, "result=error\nreason=needs an install (CASC storage or plain folder) and --out <file>\n");
        return 5;
    }
    std::vector<std::string> Names;
    FS.EnumerateFiles(Options.CascPattern.empty() ? "*.xsub" : Options.CascPattern,
                      [&](const std::string& Name, const size_t) { Names.push_back(Name); });
    // --type probe: also walk each object's block headers (XSUBCache::ExtractPackageObject's
    // format) for its decompressed size, and decompress its first block for a few header words,
    // to tell what the object is (a sound's 288-byte header, ...). Reads only the headers, 4 bytes
    // per block and the first block.
    const bool Probe = Options.Type == "probe";
    if (Probe) Siren::Initialize(L"oo2core_8_win64.dll");
    std::vector<uint8_t> Block, Payload;
    auto ProbeObject = [&](CoDFileHandle& Reader, uint64_t Offset, uint64_t Stored) -> std::string {
        // columns: payload bytes (-1 unknown), blocks, first block flag, first-block words at
        // 0, 4, 8, 0x10, 0x118 (280), 0x11c, 0x120, 0x124, first-block decompressed size
        uint64_t Read = 0, Pos = Offset, PayloadSize = 0;
        uint32_t Blocks = 0, FirstFlag = 0xFFFFFFFF;
        bool Unknown = false, Raw = false;
        Payload.clear();
        while (Read < Stored)
        {
            uint32_t Hdr[2] = {0, 0};
            Reader.Seek(Pos, SEEK_SET);
            Reader.Read(reinterpret_cast<uint8_t*>(Hdr), 8);
            const uint32_t Count = Hdr[0];
            if (Count == 0 || Count > 256) { Raw = Blocks == 0; break; }
            uint32_t Commands[256] = {};
            Reader.Read(reinterpret_cast<uint8_t*>(Commands), Count * 4);
            const uint64_t HeaderSize = Count <= 30 ? 128 : 8 + 4ull * Count;
            Pos += HeaderSize;
            Read += HeaderSize;
            for (uint32_t I = 0; I < Count; I++)
            {
                const uint64_t Size = Commands[I] & 0xFFFFFF;
                const uint32_t Flag = Commands[I] >> 24;
                if (FirstFlag == 0xFFFFFFFF) FirstFlag = Flag;
                if (Flag == 8 || Flag == 9)
                {
                    uint32_t Dec = 0;
                    Reader.Seek(Pos, SEEK_SET);
                    Reader.Read(reinterpret_cast<uint8_t*>(&Dec), 4);
                    PayloadSize += Dec;
                    if (Blocks == 0 && Size > 4 && Dec <= (8u << 20))
                    {
                        Block.resize(Size);
                        Reader.Seek(Pos, SEEK_SET);
                        Reader.Read(Block.data(), Size);
                        Payload.resize(Dec);
                        const auto N = Siren::Decompress(Block.data() + 4, Size - 4, Payload.data(), Dec);
                        Payload.resize(N);
                    }
                    Blocks++;
                }
                else if (Flag == 0)
                {
                    if (Blocks == 0)
                    {
                        Payload.resize(std::min<uint64_t>(Size, 4096));
                        Reader.Seek(Pos, SEEK_SET);
                        Reader.Read(Payload.data(), Payload.size());
                    }
                    PayloadSize += Size;
                    Blocks++;
                }
                else if (Flag == 3 || Flag == 6)
                {
                    Unknown = true;
                    Blocks++;
                }
                const uint64_t Next = I + 1 < Count ? Pos + Size : ((Pos + Size + 0x7F) & ~0x7Full);
                Read += Next - Pos;
                Pos = Next;
            }
        }
        if (Raw)
        {
            PayloadSize = Stored;
            Payload.resize(std::min<uint64_t>(Stored, 4096));
            Reader.Seek(Offset, SEEK_SET);
            Reader.Read(Payload.data(), Payload.size());
        }
        auto W = [&](size_t At) -> uint32_t {
            uint32_t V = 0;
            if (At + 4 <= Payload.size()) std::memcpy(&V, Payload.data() + At, 4);
            return V;
        };
        return std::format("{}\t{}\t{}\t{:08x}\t{:08x}\t{:08x}\t{:08x}\t{:08x}\t{:08x}\t{:08x}\t{:08x}\t{}",
            Unknown ? -1ll : (long long)PayloadSize, Blocks, Raw ? std::string("raw") : std::to_string(FirstFlag),
            W(0), W(4), W(8), W(0x10), W(0x118), W(0x11c), W(0x120), W(0x124), Payload.size());
    };
    std::ofstream Out(Options.Out, std::ios::binary);
    std::map<uint64_t, size_t> PackagesByType;
    size_t Entries = 0, Skipped = 0;
    std::string Headers;
    for (const auto& Name : Names)
    {
        CoDFileHandle Reader(FS.OpenFile(Name, "r"), &FS);
        if (!Reader.IsValid()) { Skipped++; continue; }
        const auto Header = Reader.Read<XSubHeader>();
        Headers += std::format("header\t{}\tmagic={:08x}\tversion={}\ttype={}\tfiles={}\tdata={}+{}\thashes={}@{}\tsize={}\n",
            Name, Header.Magic, Header.Version, Header.Type, Header.FileCount, Header.DataOffset, Header.DataSize,
            Header.HashCount, Header.HashOffset, (unsigned long long)Reader.Size());
        if (Header.Magic != 0x4950414b || Header.HashCount <= 0 || Header.HashOffset <= 0 ||
            (uint64_t)Header.HashOffset + (uint64_t)Header.HashCount * sizeof(XSubHashEntry) > Reader.Size())
        {
            Skipped++;
            continue;
        }
        PackagesByType[Header.Type]++;
        if (Header.Version >= 23)
        {
            // {u64 key, u32 stored size << 1 | framed, u32 offset >> 7, u32 size} (XSUBCache)
            if ((uint64_t)Header.HashOffset + (uint64_t)Header.HashCount * 20 > Reader.Size())
            {
                Skipped++;
                continue;
            }
            Reader.Seek(Header.HashOffset, SEEK_SET);
            std::vector<uint8_t> Raw((size_t)Header.HashCount * 20);
            Reader.Read(Raw.data(), 0, Raw.size());
            for (size_t i = 0; i < (size_t)Header.HashCount; i++)
            {
                uint64_t Key = 0;
                uint32_t Packed = 0, Offset = 0, Size = 0;
                std::memcpy(&Key, Raw.data() + i * 20, 8);
                std::memcpy(&Packed, Raw.data() + i * 20 + 8, 4);
                std::memcpy(&Offset, Raw.data() + i * 20 + 12, 4);
                std::memcpy(&Size, Raw.data() + i * 20 + 16, 4);
                Out << Name << '\t' << Header.Type << '\t' << std::hex << Key << std::dec << '\t'
                    << ((uint64_t)Offset << 7) << '\t' << (Packed >> 1) << '\t' << (Packed & 1) << '\t' << Size << '\n';
            }
            Entries += (size_t)Header.HashCount;
            continue;
        }
        Reader.Seek(Header.HashOffset, SEEK_SET);
        std::vector<XSubHashEntry> Table((size_t)Header.HashCount);
        Reader.Read(reinterpret_cast<uint8_t*>(Table.data()), Table.size() * sizeof(XSubHashEntry));
        for (const auto& E : Table)
        {
            const uint64_t Offset = (E.PackedInfo >> 32) << 7, Stored = (E.PackedInfo >> 1) & 0x3FFFFFFF;
            Out << Name << '\t' << Header.Type << '\t' << std::hex << E.Key << std::dec << '\t'
                << Offset << '\t' << Stored << '\t' << (E.PackedInfo & 1);
            if (Probe) Out << '\t' << ProbeObject(Reader, Offset, Stored);
            Out << '\n';
        }
        Entries += Table.size();
    }
    std::string Report = std::format("result=ok\npackages={}\nentries={}\nskipped={}\n", Names.size(), Entries, Skipped);
    for (const auto& [Type, Count] : PackagesByType)
        Report += std::format("packages_type_{}={}\n", Type, Count);
    CliReport(Options, Report + Headers);
    return 0;
}

// Diagnostic: Cold War .xsub objects by key (a streamkey's at its 8, a mip's, a mesh's), decompressed
// as the live readers get them (XSUBCache), each written to <out>/<key>.bin
int RunXsubExtract(const CliOptions& Options)
{
    CoDPackageCache* Packages = TreyarchPackages::Open<XSUBCache>(Options.XsubExtract);
    if (Packages == nullptr || Options.Out.empty())
    {
        CliReport(Options, "result=error\nreason=needs a Cold War install and --out <dir>\n");
        return 5;
    }
    std::error_code Ec;
    std::filesystem::create_directories(Options.Out, Ec);
    std::string Report;
    size_t Written = 0, Missing = 0;
    size_t From = 0;
    while (From <= Options.XsubKeys.size())
    {
        const size_t Comma = std::min(Options.XsubKeys.find(',', From), Options.XsubKeys.size());
        const std::string Text = Options.XsubKeys.substr(From, Comma - From);
        From = Comma + 1;
        if (Text.empty()) continue;
        const uint64_t Key = std::stoull(Text, nullptr, 16);
        uint32_t Size = 0;
        auto Data = Packages->Exists(Key) ? Packages->ExtractPackageObject(Key, Size) : nullptr;
        if (Data == nullptr)
        {
            Missing++;
            Report += std::format("missing\t{:x}\n", Key);
            continue;
        }
        std::ofstream((std::filesystem::path(Options.Out) / std::format("{:x}.bin", Key)).string(), std::ios::binary)
            .write(reinterpret_cast<const char*>(Data.get()), Size);
        Written++;
        Report += std::format("object\t{:x}\t{}\n", Key, Size);
    }
    CliReport(Options, std::format("result=ok\nwritten={}\nmissing={}\n", Written, Missing) + Report);
    return 0;
}

// Diagnostic: pulls one file out of a CASC storage as raw bytes, unmodified, so its header can
// be inspected directly.
int RunCascExtractDir(const CliOptions& Options)
{
    CascFileSystem FS(Options.CascExtractDir);
    if (!FS.IsValid())
    {
        CliReport(Options, std::format("result=error\nreason=could not open CASC storage\ncode={}\n", FS.GetLastError()));
        return 5;
    }

    auto Handle = FS.OpenFile(Options.CascExtractName, "r");
    if (Handle == NULL)
    {
        CliReport(Options, std::format("result=error\nreason=could not open entry\ncode={}\n", FS.GetLastError()));
        return 5;
    }

    auto Size = FS.Size(Handle);
    auto Buffer = std::make_unique<uint8_t[]>(Size);
    FS.Read(Handle, Buffer.get(), 0, Size);
    FS.CloseHandle(Handle);

    std::ofstream Out(Options.CascExtractOut, std::ios::binary | std::ios::trunc);
    Out.write((const char*)Buffer.get(), (std::streamsize)Size);

    CliReport(Options, std::format("result=ok\nsize={}\nout={}\n", Size, Options.CascExtractOut.c_str()));
    return 0;
}


// FastFile::Decompress on a local .ff fastfile (e.g. pulled out via casc-extract), its raw
// decompressed zone bytes written to disk.
int RunZoneDecompress(const CliOptions& Options)
{
    std::vector<uint8_t> Zone;
    std::string Error;

    if (!FastFile::Decompress(Options.ZoneDecompressIn, Zone, Error))
    {
        CliReport(Options, std::format("result=error\nreason={}\n", Error.c_str()));
        return 5;
    }

    std::ofstream Out(Options.ZoneDecompressOut, std::ios::binary | std::ios::trunc);
    Out.write((const char*)Zone.data(), (std::streamsize)Zone.size());

    CliReport(Options, std::format("result=ok\nsize={}\nout={}\n", Zone.size(), Options.ZoneDecompressOut.c_str()));
    return 0;
}

// ---- the subcommands (CliCommands.h)

void AddCascCommands(CliCommandBuilders& B)
{
    B["casc-list"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("casc-list", "List every file in a CASC storage matching --casc-pattern");
        Sub->add_option("dir", Options.CascList, "CASC storage directory")->required()->check(CLI::ExistingDirectory);
        Sub->add_option("--casc-pattern", Options.CascPattern, "Glob pattern (default: *.sab*)");
    };
    B["xpak-index"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("xpak-index", "Write every .xpak's index (package, key, offset, stored size) as TSV to --out");
        Sub->add_option("dir", Options.XpakIndex, "CASC storage directory")->required()->check(CLI::ExistingDirectory);
    };
    B["xsub-index"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("xsub-index", "Write every .xsub's hash table (package, type, key, offset, stored size; version 23 adds the size) as TSV to --out");
        Sub->add_option("dir", Options.XsubIndex, "Install: CASC storage or plain folder")->required()->check(CLI::ExistingDirectory);
        Sub->add_option("--casc-pattern", Options.CascPattern, "Packages to read (default: *.xsub); --type probe also reads each object's block headers and first block");
    };
    B["xsub-extract"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("xsub-extract", "Write .xsub objects (Cold War, Modern Warfare III, Black Ops 6 and 7), decompressed, as <out>/<key>.bin (a streamkey's key at its 8)");
        Sub->add_option("dir", Options.XsubExtract, "Install (CASC storage or plain folder)")->required()->check(CLI::ExistingDirectory);
        Sub->add_option("--keys", Options.XsubKeys, "Object keys, hex, comma separated")->required();
    };
    B["casc-extract"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("casc-extract", "Pull one file out of CASC storage as-is");
        Sub->add_option("dir", Options.CascExtractDir, "CASC storage directory")->required()->check(CLI::ExistingDirectory);
        Sub->add_option("name", Options.CascExtractName, "File name inside CASC storage")->required();
        Sub->add_option("outfile", Options.CascExtractOut, "Where to write the extracted file")->required();
    };
    B["zone-decompress"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("zone-decompress", "Decompress a local fastfile to raw zone bytes (FastFile::Decompress)");
        Sub->add_option("in", Options.ZoneDecompressIn, "Local .ff fastfile path (already extracted via casc-extract)")->required()->check(CLI::ExistingFile);
        Sub->add_option("outfile", Options.ZoneDecompressOut, "Where to write the raw decompressed zone bytes")->required();
    };
}
