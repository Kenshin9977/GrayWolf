#include "stdafx.h"
#include "ByteView.h"

// The "IWffa100" container shared by Modern Warfare 2019, Modern Warfare III, Black Ops 6 and
// Black Ops 7 (IwffContainer.h): one decoder, each header version's constants registered by
// its title.

#include "IwffContainer.h"

#include "FastFile.h"
#include "FastFile.Internal.h"
#include "Compression.h"
#include "Siren.h"
#include "VCDiff.h"

#include <algorithm>
#include <atomic>
#include <execution>
#include <future>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>

namespace
{
    constexpr size_t PatchHeaderSize = 0x38;
    constexpr size_t AuthSize = 0x8000;           // "IWffs100" chunk (and the first hash block)
    constexpr size_t HashBlock = 0x4000;
    constexpr uint32_t StreamMagic = 0x43574902;  // "\x02IWC"
    constexpr uint32_t SectionsSigned = 0x41464154; // "TAFA"
    constexpr uint32_t SectionsPlain = 0x46464154;  // "TAFF"
    constexpr size_t SignatureSize = 0x124;
    constexpr uint32_t SectionBlob = 0x7337dacf;  // u64 at 0: the bytes between the sections and the stream

    std::map<uint32_t, IwffContainer::Layout>& Layouts()
    {
        static std::map<uint32_t, IwffContainer::Layout> Rows;
        return Rows;
    }

    bool LoadOodle(const IwffContainer::Layout& L, std::string& Error)
    {
        for (const wchar_t* Dll : L.OodleDlls)
            if (Siren::Initialize(Dll) == SirenStatus::Success)
                return true;
        std::string Names;
        for (const wchar_t* Dll : L.OodleDlls)
        {
            Names += Names.empty() ? "" : " or ";
            for (const wchar_t* C = Dll; *C != 0; C++)
                Names += static_cast<char>(*C); // the names are ASCII
        }
        Error = std::format("{} need {}: it is in neither the game's folder nor beside GrayWolf.exe", L.Title, Names);
        return false;
    }

    // The stream starting at At with a 0x4000 hash hole after every Every bytes taken out
    std::vector<uint8_t> Unsplice(ByteView File, size_t At, uint64_t Every)
    {
        std::vector<uint8_t> Stream;
        Stream.reserve(File.size() > At ? File.size() - At : 0);
        for (uint64_t Logical = 0;;)
        {
            const auto Physical = At + Logical + (Logical / Every) * HashBlock;
            if (Physical >= File.size())
                break;
            const auto UntilHole = Every - Logical % Every;
            const auto Take = (size_t)std::min<uint64_t>(UntilHole, File.size() - Physical);
            Stream.insert(Stream.end(), File.data() + Physical, File.data() + Physical + Take);
            Logical += Take;
            if (Take < UntilHole)
                break;
        }
        return Stream;
    }

    // The stream whose "\x02IWC" is at At: "IWffs100" and its 0x8000 bytes when signed, {u32 zone
    // size, u32 flags (byte 7 the compressor)}, then blocks {u32 compressed, u32 size, u32 0} and
    // their bytes padded to 4 from the block's start (a patch's stream starts anywhere)
    bool DecodeStreamAt(ByteView File, size_t At, const IwffContainer::Layout& L,
                        ZoneBytes& Out, std::string& Error)
    {
        if (At + 4 > File.size() || FastFileInternal::ReadU32(File, At) != StreamMagic)
        {
            Error = std::format("no stream at 0x{:x}", At);
            return false;
        }
        At += 4;
        const bool Secure = At + 8 <= File.size() && std::memcmp(File.data() + At, "IWffs100", 8) == 0;
        if (Secure)
            At += AuthSize;
        if (At + 8 > File.size())
        {
            Error = "the stream has no header";
            return false;
        }
        const uint64_t Declared = FastFileInternal::ReadU32(File, At);
        const uint8_t Compressor = File[At + 7];
        At += 8;
        // 1 stored, 5 LZ4 (Modern Warfare 2019's srv_ zones); the Oodle ones (Kraken 6/7,
        // Leviathan 0xc/0xd, Mermaid 0xe/0xf, Selkie 0x10/0x11) through the one Oodle entry point
        const bool Oodle = Compressor == 6 || Compressor == 7 || (Compressor >= 0xc && Compressor <= 0x11);
        if (!Oodle && Compressor != 1 && Compressor != 5)
        {
            Error = std::format("unknown compressor {}", Compressor);
            return false;
        }
        if (Oodle && !LoadOodle(L, Error))
            return false;
        // the blocks' headers first, then the blocks decompressed side by side into the zone sized once
        struct Job
        {
            size_t At, OutAt;
            uint32_t Compressed, Size;
        };
        std::vector<Job> Jobs;
        size_t Total = 0;
        for (size_t Block = 0; Total < Declared; Block++)
        {
            if (Secure && (Block & 0x1ff) == 0x1ff)
                At += HashBlock;
            if (At + 12 > File.size())
            {
                Error = std::format("the stream ends at 0x{:x} with {} of {} bytes", At, Total, Declared);
                return false;
            }
            const auto Compressed = FastFileInternal::ReadU32(File, At);
            const auto Size = FastFileInternal::ReadU32(File, At + 4);
            At += 12;
            // No ratio check: a block of zeros is a few bytes (59 for 64 KB); Oodle stops at Size
            if (Compressed == 0 || Size == 0 || At + Compressed > File.size() || Total + Size > Declared)
            {
                Error = std::format("bad block at 0x{:x} ({} -> {} bytes)", At - 12, Compressed, Size);
                return false;
            }
            Jobs.push_back({ At, Total, Compressed, Size });
            Total += Size;
            At += (Compressed + 3) & ~(size_t)3;
        }
        Out.clear();
        Out.resize(Total);
        std::atomic<size_t> Failed{ SIZE_MAX };
        std::for_each(std::execution::par, Jobs.begin(), Jobs.end(), [&](const Job& J) {
            bool Done = false;
            if (Compressor == 1)
            {
                Done = J.Compressed == J.Size;
                if (Done)
                    std::memcpy(Out.data() + J.OutAt, File.data() + J.At, J.Size);
            }
            else if (Compressor == 5)
                Done = Compression::DecompressLZ4Block((const int8_t*)File.data() + J.At, (int8_t*)Out.data() + J.OutAt,
                                                       (int32_t)J.Compressed, (int32_t)J.Size) == J.Size;
            else
                Done = Siren::Decompress(File.data() + J.At, J.Compressed, Out.data() + J.OutAt, J.Size) == J.Size;
            if (!Done)
            {
                // the first block that failed
                size_t Was = Failed.load();
                const size_t Me = (size_t)(&J - Jobs.data());
                while (Me < Was && !Failed.compare_exchange_weak(Was, Me)) {}
            }
        });
        if (const size_t F = Failed.load(); F != SIZE_MAX)
        {
            Error = std::format("the block at 0x{:x} did not decompress", Jobs[F].At - 12);
            return false;
        }
        return true;
    }

    bool DecodeStream(ByteView File, size_t At, const IwffContainer::Layout& L,
                      ZoneBytes& Out, std::string& Error)
    {
        if (L.HashEveryBytes == 0)
            return DecodeStreamAt(File, At, L, Out, Error);
        return DecodeStreamAt(Unsplice(File, At, L.HashEveryBytes), 0, L, Out, Error);
    }

    // The first "\x02IWC" at or after From
    size_t FindStream(ByteView File, size_t From)
    {
        for (size_t At = From; At + 4 <= File.size(); At++)
            if (FastFileInternal::ReadU32(File, At) == StreamMagic)
                return At;
        return std::string::npos;
    }

    // Where the stream of a file whose headers end at HeadersEnd starts, npos if none
    size_t StreamStart(ByteView File, size_t HeadersEnd, const IwffContainer::Layout& L,
                       bool IsPatch, std::string& Error)
    {
        if (!L.Sections)
            return HeadersEnd + L.AuthBeforeStream;
        size_t SectionsEnd = 0;
        uint64_t Blob = 0;
        if (!IwffContainer::SkipSections(File, HeadersEnd, SectionsEnd, Blob))
        {
            Error = IsPatch ? "patch: its sections do not parse" : "the header's sections do not parse";
            return std::string::npos;
        }
        if (!IsPatch)
        {
            // The stream's bytes (u32 0x14) end the file, after the blob
            const auto At = File.size() - std::min<size_t>(File.size(), FastFileInternal::ReadU32(File, 0x14));
            if (At == SectionsEnd + Blob && FastFileInternal::ReadU32(File, At) == StreamMagic)
                return At;
        }
        const auto At = FindStream(File, SectionsEnd);
        if (At == std::string::npos)
            Error = IsPatch ? "patch: no stream" : "no stream after the header";
        return At;
    }

    // A .fp read and its delta decoded (Empty: a patch of no bytes, the zone left as it is)
    struct PatchDelta
    {
        ZoneBytes Delta;
        uint64_t NewSize = 0;
        bool Empty = false;
    };
    bool ReadPatch(const std::string& PatchPath, ByteView BaseHeader, const IwffContainer::Layout& L, PatchDelta& Out,
                   std::string& Error)
    {
        std::vector<uint8_t> Patch;
        if (!FastFileInternal::ReadWholeFile(PatchPath, Patch, Error))
            return false;
        if (Patch.size() < PatchHeaderSize + 2 * L.HeaderSize || std::memcmp(Patch.data(), "IWffd100", 8) != 0)
        {
            Error = "the .fp is not an IWffd100 patch";
            return false;
        }
        // A patch made against another build of the .ff would turn it into garbage
        if (std::memcmp(Patch.data() + PatchHeaderSize, BaseHeader.data(), L.HeaderSize) != 0)
        {
            Error = "the .fp patches another version of the .ff";
            return false;
        }
        Out.NewSize = FastFileInternal::ReadU64(Patch, PatchHeaderSize + L.HeaderSize + L.ZoneSizeAt);
        // A patch of no bytes leaves the zone as it is (its sizes all zero: boot.fp, ww_ui_mp.fp)
        Out.Empty = FastFileInternal::ReadU64(Patch, 0x30) == 0;
        if (Out.Empty) return true;
        const auto StreamAt = StreamStart(Patch, PatchHeaderSize + 2 * L.HeaderSize, L, true, Error);
        if (StreamAt == std::string::npos)
            return false;
        if (!DecodeStream(Patch, StreamAt, L, Out.Delta, Error))
        {
            Error = "patch: " + Error;
            return false;
        }
        return true;
    }

    bool ApplyPatch(const PatchDelta& P, ZoneBytes& Zone, std::string& Error)
    {
        const uint64_t NewSize = P.NewSize;
        if (P.Empty)
        {
            if (NewSize != Zone.size())
            {
                Error = std::format("an empty patch, but the patched zone is {} bytes, not {}", NewSize, Zone.size());
                return false;
            }
            return true;
        }
        ZoneBytes Target;
        std::string VError;
        if (!VCDiff::Decode(Zone, P.Delta.data(), P.Delta.size(), Target, VError))
        {
            Error = "patch: VCDIFF: " + VError;
            return false;
        }
        if (Target.size() != NewSize)
        {
            Error = std::format("the patched zone is {} bytes, its header says {}", Target.size(), NewSize);
            return false;
        }
        Zone = std::move(Target);
        return true;
    }

    bool Decode(const std::string& FilePath, ByteView File, ZoneBytes& Zone, std::string& Error)
    {
        if (File.size() < 12)
        {
            Error = "file is shorter than its header";
            return false;
        }
        const auto Found = Layouts().find(FastFileInternal::ReadU32(File, 8));
        if (Found == Layouts().end())
        {
            Error = std::format("no IWffa100 layout for header version {}", FastFileInternal::ReadU32(File, 8));
            return false;
        }
        const auto& L = Found->second;
        if (File.size() < L.HeaderSize + 12)
        {
            Error = "file is shorter than its header";
            return false;
        }
        if (L.XFileVersion != 0 && FastFileInternal::ReadU32(File, 0xc) != L.XFileVersion)
        {
            Error = std::format("not an xfile version {} header", L.XFileVersion);
            return false;
        }
        const auto StreamAt = StreamStart(File, L.HeaderSize, L, false, Error);
        if (StreamAt == std::string::npos)
            return false;
        // its .fp read and its delta decoded while the zone is
        auto PatchPath = std::filesystem::path(FilePath);
        PatchPath.replace_extension(".fp");
        std::error_code Ec;
        const bool Patched = std::filesystem::exists(PatchPath, Ec);
        PatchDelta Patch;
        std::string PatchError;
        std::future<bool> PatchRead;
        if (Patched)
            PatchRead = std::async(std::launch::async, [&] {
                return ReadPatch(PatchPath.string(), File.subspan(0, L.HeaderSize), L, Patch, PatchError);
            });
        const bool Read = DecodeStream(File, StreamAt, L, Zone, Error);
        const bool PatchOk = !Patched || PatchRead.get();
        if (!Read)
            return false;
        if (L.CheckZoneSize)
        {
            const auto Declared = FastFileInternal::ReadU64(File, L.ZoneSizeAt);
            if (Zone.size() != Declared)
            {
                Error = std::format("the zone is {} bytes, its header says {}", Zone.size(), Declared);
                return false;
            }
        }

        if (!Patched)
            return true;
        if (!PatchOk)
        {
            Error = PatchError;
            return false;
        }
        return ApplyPatch(Patch, Zone, Error);
    }
}

namespace IwffContainer
{
    Registration::Registration(const Layout& L)
    {
        Layouts()[L.HeaderVersion] = L;
        const FastFileContainerRegistration Registered("IWffa100", L.HeaderVersion, Decode);
    }

    bool SkipSections(ByteView File, size_t At, size_t& End, uint64_t& BlobSize)
    {
        BlobSize = 0;
        if (At + 8 > File.size())
            return false;
        const auto Magic = FastFileInternal::ReadU32(File, At);
        if (Magic != SectionsSigned && Magic != SectionsPlain)
            return false;
        const auto Count = FastFileInternal::ReadU32(File, At + 4);
        At += 8;
        for (uint32_t i = 0; i < Count; i++)
        {
            if (At + 8 > File.size())
                return false;
            const auto Type = FastFileInternal::ReadU32(File, At);
            const auto Size = FastFileInternal::ReadU32(File, At + 4);
            if (At + 8 + Size > File.size())
                return false;
            if (Type == SectionBlob && Size >= 8)
                BlobSize = FastFileInternal::ReadU64(File, At + 8);
            At += 8 + Size;
        }
        if (Magic == SectionsSigned)
            At += SignatureSize;
        End = At;
        return End <= File.size();
    }

    bool ReadSections(const std::string& FilePath, size_t HeaderSize, uint32_t& Magic, std::vector<uint32_t>& Types)
    {
        Types.clear();
        std::ifstream In(std::filesystem::path(FilePath), std::ios::binary);
        std::vector<uint8_t> Head(0x2000);
        In.read(reinterpret_cast<char*>(Head.data()), (std::streamsize)Head.size());
        Head.resize((size_t)In.gcount());
        if (Head.size() < HeaderSize + 8 || std::memcmp(Head.data(), "IWffa100", 8) != 0)
            return false;
        size_t At = HeaderSize;
        Magic = FastFileInternal::ReadU32(Head, At);
        if (Magic != SectionsSigned && Magic != SectionsPlain)
            return false;
        const auto Count = FastFileInternal::ReadU32(Head, At + 4);
        At += 8;
        for (uint32_t i = 0; i < Count && At + 8 <= Head.size(); i++)
        {
            Types.push_back(FastFileInternal::ReadU32(Head, At));
            At += 8 + (size_t)FastFileInternal::ReadU32(Head, At + 4);
        }
        return true;
    }
}
