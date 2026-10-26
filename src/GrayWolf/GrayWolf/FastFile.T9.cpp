#include "stdafx.h"

// Black Ops Cold War (T9): the "flexible" container. The fixed header of BO3/BO4 becomes a list of
// typed sections ("TAFF", count, then type/size/bytes each; "TAFA" adds a signature after them),
// followed by the same 0x10-byte block headers BO4 uses, Oodle Kraken in the retail files. The
// update patch (.fd) is a 0x28-byte bdiff header, the new and the base flexible headers, then the
// patch data (zlib'd VCDIFF, BO4's bdiff) whose source is the whole decompressed .ff zone: the
// patched zone is what the loader reads, and what Decompress returns when the .fd is beside the
// .ff. Layouts from acts (ate47/atian-cod-tools: fastfile_flexible, decompressor_t9).

#include "FastFile.h"
#include "FastFile.Internal.h"

#include <miniz/miniz.h>
#include "Compression.h"
#include "Siren.h"
#include "VCDiff.h"

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

namespace
{
    constexpr uint32_t FlexibleMagic = 0x46464154;        // "TAFF"
    constexpr uint32_t FlexibleMagicSigned = 0x41464154;  // "TAFA": a signature follows the sections
    constexpr size_t FlexibleSignatureSize = 0x124;       // magic, sha256, 0x100 signature
    constexpr uint32_t ColdWarVersion = 0x64;
    constexpr size_t EmptyBlockAlignment = 0x800000;
    constexpr size_t BDiffHeaderSize = 0x28;

    enum SectionType : uint32_t
    {
        SectionVersion = 0x39CB44F1u,
        SectionPlatform = 0x2C2381CFu,
        SectionSize = 0x0F992DFEu,
        SectionInfo = 0xC7708CDAu,
    };

    struct PlatformData
    {
        uint8_t Server;
        uint8_t Compression;
        uint8_t Platform;
        uint8_t Encrypted;
    };

    struct Section
    {
        uint32_t Type = 0;
        const uint8_t* Data = nullptr;
        uint32_t Size = 0;
    };

    struct FlexibleHeader
    {
        std::vector<Section> Sections;

        const Section* Find(uint32_t Type) const
        {
            for (const auto& S : Sections)
                if (S.Type == Type) return &S;
            return nullptr;
        }
        template <class T>
        bool Get(uint32_t Type, T& Out) const
        {
            const auto* S = Find(Type);
            if (!S || S->Size < sizeof(T)) return false;
            std::memcpy(&Out, S->Data, sizeof(T));
            return true;
        }
    };

    bool ReadFile(const std::string& Path, std::vector<uint8_t>& Out, std::string& Error)
    {
        std::ifstream In(Path, std::ios::binary | std::ios::ate);
        if (!In)
        {
            Error = "could not open " + Path;
            return false;
        }
        Out.resize((size_t)In.tellg());
        In.seekg(0);
        if (!In.read(reinterpret_cast<char*>(Out.data()), (std::streamsize)Out.size()))
        {
            Error = "could not read " + Path;
            return false;
        }
        return true;
    }

    // The sections from Pos; Pos ends after them (and the signature of a "TAFA")
    bool ReadHeader(const std::vector<uint8_t>& File, size_t& Pos, FlexibleHeader& Header, std::string& Error)
    {
        uint32_t Magic = 0, Count = 0;
        if (Pos + 8 > File.size())
        {
            Error = "the flexible header is cut short";
            return false;
        }
        std::memcpy(&Magic, File.data() + Pos, 4);
        std::memcpy(&Count, File.data() + Pos + 4, 4);
        if (Magic != FlexibleMagic && Magic != FlexibleMagicSigned)
        {
            Error = std::format("not a flexible header (magic 0x{:08x})", Magic);
            return false;
        }
        if (Count > 0x10)
        {
            Error = std::format("too many header sections ({})", Count);
            return false;
        }
        Pos += 8;
        for (uint32_t i = 0; i < Count; i++)
        {
            Section S;
            if (Pos + 8 > File.size())
            {
                Error = "a header section is cut short";
                return false;
            }
            std::memcpy(&S.Type, File.data() + Pos, 4);
            std::memcpy(&S.Size, File.data() + Pos + 4, 4);
            Pos += 8;
            if (Pos + S.Size > File.size())
            {
                Error = std::format("header section 0x{:08x} runs past the file", S.Type);
                return false;
            }
            S.Data = File.data() + Pos;
            Pos += S.Size;
            Header.Sections.push_back(S);
        }
        if (Magic == FlexibleMagicSigned)
            Pos += FlexibleSignatureSize;
        return true;
    }

    bool LoadOodle(std::string& Error)
    {
        // Cold War's own Oodle (oo2core_8_win64.dll, shipped in its install): beside the exe or in the game's folder
        const bool Ready = Siren::Initialize(L"oo2core_8_win64.dll") == SirenStatus::Success;
        if (!Ready)
            Error = "Cold War needs oo2core_8_win64.dll: it is in neither the game's folder nor beside GrayWolf.exe";
        return Ready;
    }

    bool DecompressBlock(uint8_t Compression, const uint8_t* In, uint32_t InSize, uint8_t* Out, uint32_t OutSize,
                         std::string& Error)
    {
        switch ((FastFileCompression)Compression)
        {
        case FastFileCompression::None:
            if (InSize < OutSize)
            {
                Error = "a stored block is shorter than its size";
                return false;
            }
            std::memcpy(Out, In, OutSize);
            return true;
        case FastFileCompression::ZLib:
        case FastFileCompression::ZLibHighCompression:
        {
            mz_ulong Size = OutSize;
            if (mz_uncompress(Out, &Size, In, InSize) != MZ_OK || Size != OutSize)
            {
                Error = "a zlib block did not inflate";
                return false;
            }
            return true;
        }
        case FastFileCompression::LZ4:
        case FastFileCompression::LZ4HighCompression:
            if ((uint32_t)Compression::DecompressLZ4Block((const int8_t*)In, (int8_t*)Out, (int32_t)InSize,
                                                          (int32_t)OutSize) != OutSize)
            {
                Error = "an LZ4 block did not decompress";
                return false;
            }
            return true;
        case FastFileCompression::OodleKraken:
        case FastFileCompression::OodleMermaid:
        case FastFileCompression::OodleSelkie:
        case FastFileCompression::OodleLZNA:
            if (!LoadOodle(Error)) return false;
            if (Siren::Decompress(In, InSize, Out, OutSize) != OutSize)
            {
                Error = "an Oodle block did not decompress";
                return false;
            }
            return true;
        default:
            Error = std::format("block compression {} is not supported", (uint32_t)Compression);
            return false;
        }
    }

    // The .ff's zone: its blocks, after the header
    bool DecompressBase(const std::string& Path, std::vector<uint8_t>& Zone, std::string& Error)
    {
        std::vector<uint8_t> File;
        if (!ReadFile(Path, File, Error)) return false;
        size_t Pos = 0;
        FlexibleHeader Header;
        if (!ReadHeader(File, Pos, Header, Error)) return false;
        uint32_t Version = 0;
        PlatformData Platform = {};
        if (!Header.Get(SectionVersion, Version) || !Header.Get(SectionPlatform, Platform))
        {
            Error = "the header has no version or platform section";
            return false;
        }
        if (Version != ColdWarVersion)
        {
            Error = std::format("flexible fast file version 0x{:x} is not Cold War's (0x{:x})", Version, ColdWarVersion);
            return false;
        }
        if (Platform.Encrypted)
        {
            // Same policy as BO4 (FastFile.Treyarch.cpp): the AES keys are not in the game's files
            Error = "the fast file is encrypted (its key is not recoverable from the game's own files)";
            return false;
        }
        uint64_t Declared[3] = {};
        if (Header.Get(SectionSize, Declared) && Declared[0] < (1ull << 36))
            Zone.reserve((size_t)Declared[0]);

        while (Pos + sizeof(FastFileInternal::TreyarchBlockHeader) <= File.size())
        {
            FastFileInternal::TreyarchBlockHeader Block = {};
            std::memcpy(&Block, File.data() + Pos, sizeof(Block));
            if (Block.AlignedSize == 0)
                break;
            if (Block.Offset != Pos)
            {
                Error = std::format("block at 0x{:x} claims to be at 0x{:x}", (unsigned long long)Pos, Block.Offset);
                return false;
            }
            Pos += sizeof(Block);
            if (Block.UncompressedSize == 0)
            {
                Pos = (Pos + EmptyBlockAlignment - 1) / EmptyBlockAlignment * EmptyBlockAlignment;
                continue;
            }
            if (Pos + Block.AlignedSize > File.size() || Block.CompressedSize > Block.AlignedSize)
            {
                Error = std::format("block at 0x{:x} runs past the file", Block.Offset);
                return false;
            }
            const size_t At = Zone.size();
            Zone.resize(At + Block.UncompressedSize);
            if (!DecompressBlock(Platform.Compression, File.data() + Pos, Block.CompressedSize, Zone.data() + At,
                                 Block.UncompressedSize, Error))
            {
                Error = std::format("block at 0x{:x}: ", Block.Offset) + Error;
                return false;
            }
            Pos += Block.AlignedSize;
        }
        if (Zone.empty())
        {
            Error = "no blocks were decompressed";
            return false;
        }
        return true;
    }

    // The .fd applied to the .ff's zone
    bool ApplyPatch(const std::string& FdPath, const std::vector<uint8_t>& Base, std::vector<uint8_t>& Zone,
                    std::string& Error)
    {
        std::vector<uint8_t> File;
        if (!ReadFile(FdPath, File, Error)) return false;
        size_t Pos = BDiffHeaderSize;
        FlexibleHeader Patched, Source;
        if (File.size() < BDiffHeaderSize || !ReadHeader(File, Pos, Patched, Error) || !ReadHeader(File, Pos, Source, Error))
        {
            Error = "patch headers: " + Error;
            return false;
        }
        PlatformData Platform = {};
        uint64_t Sizes[3] = {};
        if (!Patched.Get(SectionPlatform, Platform) || !Patched.Get(SectionSize, Sizes))
        {
            Error = "the patch header has no platform or size section";
            return false;
        }
        std::vector<uint8_t> Delta;
        const uint8_t* Data = File.data() + Pos;
        const size_t DataSize = File.size() - Pos;
        if ((FastFileCompression)Platform.Compression == FastFileCompression::BDeltaUncompressed)
        {
            Delta.assign(Data, Data + DataSize);
        }
        else if ((FastFileCompression)Platform.Compression == FastFileCompression::BDeltaZLib)
        {
            // the same zlib'd VCDIFF as Black Ops 4's patches
            if (!FastFileInternal::InflateZLibStreams(Data, DataSize, Delta, Error))
                return false;
        }
        else
        {
            Error = std::format("patch compression {} is not supported", (uint32_t)Platform.Compression);
            return false;
        }
        std::string DiffError;
        if (!VCDiff::Decode(Base, Delta.data(), Delta.size(), Zone, DiffError))
        {
            Error = "the patch did not apply: " + DiffError;
            return false;
        }
        return true;
    }
}

bool FastFile::DecompressTreyarchT9(const std::string& FilePath, std::vector<uint8_t>& Zone, std::string& Error)
{
    std::vector<uint8_t> Base;
    if (!DecompressBase(FilePath, Base, Error)) return false;
    const auto Fd = std::filesystem::path(FilePath).replace_extension(".fd");
    if (!std::filesystem::exists(Fd))
    {
        Zone = std::move(Base);
        return true;
    }
    return ApplyPatch(Fd.string(), Base, Zone, Error);
}
