#include "stdafx.h"

#include "FastFile.h"
#include "FastFile.Internal.h"

#include <miniz/miniz.h>
#include "ZoneAssetTypes.h"
#include "WorldWar2Crypto.h"

#include <format>
#include <iterator>
#include <map>

using namespace FastFileInternal;

namespace
{
    struct RegisteredContainer
    {
        FastFileContainer Decode = nullptr;
        FastFilePathContainer DecodePath = nullptr;
    };
    std::map<std::pair<std::string, uint32_t>, RegisteredContainer>& Containers()
    {
        static std::map<std::pair<std::string, uint32_t>, RegisteredContainer> Registered;
        return Registered;
    }
}

FastFileContainerRegistration::FastFileContainerRegistration(const char* Magic, uint32_t Version, FastFileContainer Decode)
{
    Containers()[{ Magic, Version }] = { Decode, nullptr };
}

FastFileContainerRegistration::FastFileContainerRegistration(const char* Magic, uint32_t Version, FastFilePathContainer Decode)
{
    Containers()[{ Magic, Version }] = { nullptr, Decode };
}

bool HasFastFileContainer(const std::string& Magic, uint32_t Version)
{
    return Containers().contains({ Magic, Version });
}

bool DecodeRegisteredFastFile(const std::string& Magic, uint32_t Version, const std::string& FilePath,
    const std::vector<uint8_t>& File, std::vector<uint8_t>& Zone, std::string& Error)
{
    const auto It = Containers().find({ Magic, Version });
    if (It == Containers().end())
    {
        Error = "no container registered for this magic and version";
        return false;
    }
    return It->second.DecodePath ? It->second.DecodePath(FilePath, File, Zone, Error) : It->second.Decode(File, Zone, Error);
}


FastFileInfo FastFile::Identify(const std::string& FilePath)
{
    FastFileInfo Info;

    auto Handle = std::unique_ptr<FILE, decltype(&fclose)>(nullptr, fclose);
    {
        FILE* Raw = nullptr;
        if (fopen_s(&Raw, FilePath.c_str(), "rb") != 0 || Raw == nullptr)
        {
            return Info;
        }
        Handle.reset(Raw);
    }

    // Header plus the first two payload bytes, which is what tells zlib apart; +32 so the detector
    // can also look 9 bytes further on, where the 64-bit rebuilds of MW2 and MW3 put their zlib
    // stream or their signed zones' auth chunk.
    uint8_t Header[HeaderSize + 32] = {};
    const auto Read = fread(Header, 1, sizeof(Header), Handle.get());

    if (Read < HeaderSize)
    {
        return Info;
    }

    Info.Magic.assign(reinterpret_cast<const char*>(Header), 8);
    std::memcpy(&Info.Version, Header + 8, sizeof(uint32_t));
    Info.Format = HasFastFileContainer(Info.Magic, Info.Version)
        ? FastFileFormat::Registered
        : FormatFromHeader(Info.Magic, Info.Version, Header + HeaderSize, Read >= HeaderSize + 11,
                           Read > HeaderSize ? Read - HeaderSize : 0, Info.HeaderExtraBytes);

    // Ghosts' signed zones break the stream every 2 MB for a hash block (256 authed chunks of
    // 0x2000 each, then one 0x2000 block). Recorded here so Decompress can step over them.
    if (Info.Format == FastFileFormat::InfinityWardAuthedZLib)
    {
        Info.AuthedGroupBytes = 0x200000;
        Info.AuthedSkipBytes = 0x2000;
    }

    // Cold War: "TAFF" then a section count; the version is the value of its version section
    // (0x39CB44F1), which is what FindGameModule matches (FastFile.T9.cpp)
    if (Info.Format == FastFileFormat::TreyarchFlexible)
    {
        Info.Magic.resize(4);
        Info.Version = 0;
        uint8_t Head[0x400] = {};
        const auto Got = (_fseeki64(Handle.get(), 0, SEEK_SET) == 0) ? fread(Head, 1, sizeof(Head), Handle.get()) : 0;
        uint32_t Count = 0;
        std::memcpy(&Count, Head + 4, 4);
        size_t At = 8;
        for (uint32_t K = 0; K < Count && K < 0x10 && At + 8 <= Got; K++)
        {
            uint32_t Type = 0, Size = 0;
            std::memcpy(&Type, Head + At, 4);
            std::memcpy(&Size, Head + At + 4, 4);
            if (Type == 0x39CB44F1u && Size >= 4 && At + 12 <= Got)
            {
                std::memcpy(&Info.Version, Head + At + 8, 4);
                break;
            }
            At += 8 + (size_t)Size;
        }
    }

    // The Treyarch header carries how it was packed, which decides everything below
    if (Info.Format == FastFileFormat::TreyarchChunked)
    {
        uint8_t Flags[4] = {};
        if (_fseeki64(Handle.get(), 12, SEEK_SET) == 0 && fread(Flags, 1, 4, Handle.get()) == 4)
        {
            Info.Compression = (FastFileCompression)Flags[1];
            Info.Encrypted = Flags[3] != 0;
        }

        if (IsBO4TreyarchVersion(Info.Version))
        {
            // Black Ops 4 splices a missionFastFiles[16][64] table in ahead of the size field
            // that Black Ops 3 does not have, so it lives at a different, BO4-specific offset -
            // see BO4SizeOffset above.
            uint64_t Declared = 0;
            if (_fseeki64(Handle.get(), (int64_t)BO4SizeOffset, SEEK_SET) == 0 &&
                fread(&Declared, 1, sizeof(Declared), Handle.get()) == sizeof(Declared))
            {
                Info.DeclaredZoneSize = Declared;
            }
        }
        else if (Info.Version > TreyarchAlphaVersion)
        {
            uint64_t Declared = 0;
            if (_fseeki64(Handle.get(), (int64_t)TreyarchSizeOffset, SEEK_SET) == 0 &&
                fread(&Declared, 1, sizeof(Declared), Handle.get()) == sizeof(Declared))
            {
                Info.DeclaredZoneSize = Declared;
            }
        }
    }

    // Sledgehammer states its zone size up front too, which lets the block walk be checked
    // against something rather than just trusted
    if (Info.Format == FastFileFormat::SledgehammerLZ4)
    {
        Info.Compression = FastFileCompression::LZ4;

        // The signed variant is the same header behind a signature and a hash table, fixed size
        // on all but the largest files. Rather than assume that, look for the four bytes that
        // close the header and only accept the offset if they are there - files whose table is
        // longer fall through as unrecognised, rather than decompressing garbage mid-hash-table.
        const auto IsWW2 = Info.Version == WorldWar2Version;

        const auto Expected = IsWW2 ? WorldWar2HeaderTrailer : SledgehammerHeaderTrailer;

        // The signed variant repeats the whole header behind a signature block, so the counted
        // table WWII splices in must be read from whichever copy is in use, not the outer one.
        bool Found = false;
        uint64_t Spliced = 0;

        for (const auto Base : { (uint64_t)0, SledgehammerSignedDataOffset })
        {
            uint64_t Here = 0;

            if (IsWW2)
            {
                // Two tables back to back, each a count then its entries
                auto At = Base + WorldWar2FirstCountOffset;

                for (int Table = 0; Table < 2; Table++)
                {
                    uint32_t Entries = 0;

                    if (_fseeki64(Handle.get(), (int64_t)At, SEEK_SET) != 0 ||
                        fread(&Entries, 1, sizeof(Entries), Handle.get()) != sizeof(Entries))
                    {
                        Here = UINT64_MAX;
                        break;
                    }

                    Here += (uint64_t)Entries * WorldWar2EntrySize;
                    At += 4 + (uint64_t)Entries * WorldWar2EntrySize;
                }

                if (Here == UINT64_MAX)
                {
                    continue;
                }
            }
            else
            {
                // Advanced Warfare: the game (DB_LoadXFile, 0x140242200 in the MP dump) reads
                // a count at 24 and that many 24-byte records before the stream, signed or not
                Here = SledgehammerTableBytes(Handle.get());
            }

            const auto TrailerAt = IsWW2
                ? (WorldWar2TrailerOffset + Here)
                : (SledgehammerTrailerOffset + Here);

            uint32_t Trailer = 0;

            if (_fseeki64(Handle.get(), (int64_t)(Base + TrailerAt), SEEK_SET) == 0 &&
                fread(&Trailer, 1, sizeof(Trailer), Handle.get()) == sizeof(Trailer) &&
                Trailer == Expected)
            {
                Info.DataOffset = Base;
                Spliced = Here;
                Found = true;
                break;
            }
        }

        if (!Found)
        {
            // A signed WWII file has no readable header because it is encrypted - let it
            // through for the decryption path; anything else is genuinely not a known container.
            if (IsWW2 && Info.Magic == "S1ff0100")
            {
                Info.Encrypted = true;
                return Info;
            }

            Info.Format = FastFileFormat::Unknown;
            return Info;
        }

        Info.SplicedTableBytes = Spliced;
        const auto SizeAt = IsWW2 ? (WorldWar2ZoneSizeOffset + Spliced) : (SledgehammerZoneSizeOffset + Spliced);

        uint64_t Declared = 0;
        if (_fseeki64(Handle.get(), (int64_t)(Info.DataOffset + SizeAt), SEEK_SET) == 0 &&
            fread(&Declared, 1, sizeof(Declared), Handle.get()) == sizeof(Declared))
        {
            Info.DeclaredZoneSize = Declared;
        }
    }

    return Info;
}


ZoneLayout FastFile::LayoutFor(const FastFileInfo& Info)
{
    ZoneLayout Layout;

    // Two sizes then one per block, so the content offset is 8 + blocks * 4
    const auto Content = [](uint32_t Blocks) { return (uint64_t)8 + (uint64_t)Blocks * 4; };
    // A 32-bit title: its blocks, the bits of a pointer naming one, its type names
    const auto Title32 = [&](const char* Title, uint32_t Blocks, uint32_t BlockBits, const char* const* Names, size_t Count) {
        Layout.Known = true;
        Layout.ContentOffset = Content(Blocks);
        Layout.BlockBits = BlockBits;
        Layout.TypeNames = Names;
        Layout.TypeNameCount = Count;
        Layout.Title = Title;
        return Layout;
    };

    if (Info.Magic == "IWffu100" || Info.Magic == "IWff0100")
    {
        // The 64-bit rebuilds of MW2/MW3 keep their old version numbers, so the nine
        // spliced header bytes are the only thing that tells them apart from the 32-bit
        // originals - and reading an original as 64-bit yields a wrong but plausible list.
        if ((Info.HeaderExtraBytes == 9 || Info.HeaderExtraBytes == 9 + 0x4000) && (Info.Version == 276 || Info.Version == 1))
        {
            Layout.Known = true;
            Layout.ContentOffset = Info.Version == 276 ? 40 : 44;
            Layout.PointerSize = 8;
            Layout.Title = Info.Version == 276 ? "Modern Warfare 2 (64 bit)" : "Modern Warfare 3 (64 bit)";
            return Layout;
        }

        // Ghosts: the container is read (see FormatFromHeader), the asset list is not modelled yet,
        // so it gets a title and nothing else. Known stays false: claiming a layout we do not have
        // would make every caller read garbage rather than say "not supported yet".
        if (Info.Version == 565)
        {
            Layout.Title = "Ghosts";
            return Layout;
        }

        switch (Info.Version)
        {
        case 5:   // Call of Duty 4
            return Title32("Call of Duty 4", 9, 4, ZoneAssetTypes::CallOfDuty4, std::size(ZoneAssetTypes::CallOfDuty4));
        case 387: // World at War
            return Title32("World at War", 7, 3, ZoneAssetTypes::WorldAtWar, std::size(ZoneAssetTypes::WorldAtWar));
        case 473: // Black Ops
            return Title32("Black Ops", 7, 3, ZoneAssetTypes::BlackOps, std::size(ZoneAssetTypes::BlackOps));
        default:
            return Layout;
        }
    }

    // Black Ops 2, the only one that lists its dependencies
    if ((Info.Magic == "TAff0100" || Info.Magic == "TAffu100") && Info.Version == 147)
    {
        Title32("Black Ops 2", 8, 3, ZoneAssetTypes::BlackOps2, std::size(ZoneAssetTypes::BlackOps2));
        Layout.HasDepends = true;
        return Layout;
    }

    return Layout;
}

bool FastFile::Parse32BitZone(const std::vector<uint8_t>& Zone, const ZoneLayout& Layout, ZoneInfo& Out, std::string& Error)
{
    Out = {};
    Error.clear();

    if (!Layout.Known)
    {
        Error = "no zone layout is known for this title";
        return false;
    }

    const auto Content = (size_t)Layout.ContentOffset;
    const auto ListSize = Layout.HasDepends ? 24u : 16u;

    if (Zone.size() < Content + ListSize)
    {
        Error = "zone is too small to hold a header and an asset list";
        return false;
    }

    // The zone states how much follows its own header - checking that first catches a wrong
    // layout before any field is trusted, rather than after producing a plausible asset list.
    const auto Declared = ReadU32(Zone, 0);

    if (Declared != Zone.size() - Content)
    {
        Error = std::format("zone declares {} bytes of content but {} follow a {} byte header",
            Declared, (unsigned long long)(Zone.size() - Content), (unsigned long long)Content);
        return false;
    }

    const auto StringCount = ReadU32(Zone, Content);
    const auto StringsPtr = ReadU32(Zone, Content + 4);
    const auto DependCount = Layout.HasDepends ? ReadU32(Zone, Content + 8) : 0u;
    const auto DependsPtr = Layout.HasDepends ? ReadU32(Zone, Content + 12) : 0u;
    const auto AssetCount = ReadU32(Zone, Content + (Layout.HasDepends ? 16 : 8));
    const auto AssetsPtr = ReadU32(Zone, Content + (Layout.HasDepends ? 20 : 12));

    Out.StringCount = StringCount;
    Out.AssetCount = AssetCount;

    size_t At = Content + ListSize;

    // The strings, when they are there at all
    if (StringsPtr == T6FollowsInline)
    {
        const auto SlotsEnd = At + (uint64_t)StringCount * 4;

        if (SlotsEnd > Zone.size())
        {
            Error = std::format("string count {} runs past the end of the zone", StringCount);
            return false;
        }

        auto Slots = At;
        At = (size_t)SlotsEnd;
        Out.StringTableOffset = At;

        for (uint32_t i = 0; i < StringCount; i++)
        {
            if (ReadU32(Zone, Slots + (size_t)i * 4) != T6FollowsInline)
            {
                continue;
            }

            Out.StringsInline++;

            const auto Start = At;
            while (At < Zone.size() && Zone[At] != 0)
            {
                At++;
            }

            if (At >= Zone.size())
            {
                Error = std::format("string {} of {} runs off the end of the zone", i, StringCount);
                return false;
            }

            Out.Strings.emplace_back((const char*)Zone.data() + Start, At - Start);
            At++;
        }
    }

    // Then the names of the zones this one depends on
    if (DependsPtr == T6FollowsInline)
    {
        At += (size_t)DependCount * 4;

        for (uint32_t i = 0; i < DependCount && At < Zone.size(); i++)
        {
            while (At < Zone.size() && Zone[At] != 0)
            {
                At++;
            }
            At++;
        }
    }

    if (AssetsPtr != T6FollowsInline)
    {
        Out.AssetListOffset = At;
        return true;
    }

    if (At + (uint64_t)AssetCount * 8 > Zone.size())
    {
        Error = std::format("asset count {} needs {} bytes at 0x{:x}, past the end of a {} byte zone",
            AssetCount, (unsigned long long)((uint64_t)AssetCount * 8),
            (unsigned long long)At, (unsigned long long)Zone.size());
        return false;
    }

    Out.AssetListOffset = At;
    Out.Assets.reserve(AssetCount);

    for (uint32_t i = 0; i < AssetCount; i++)
    {
        const auto Type = ReadU32(Zone, At + (size_t)i * 8);
        const auto Header = ReadU32(Zone, At + (size_t)i * 8 + 4);

        ZoneAsset Asset;
        Asset.Type = Type;
        Asset.Inline = Header == T6FollowsInline;
        Asset.Block = Asset.Inline ? 0 : (Header & ((1u << Layout.BlockBits) - 1u));
        Out.Assets.push_back(Asset);
    }

    return true;
}


std::vector<std::pair<uint64_t, std::string>> FastFile::ScanStrings(const std::vector<uint8_t>& Zone, size_t MinimumLength)
{
    std::vector<std::pair<uint64_t, std::string>> Found;

    std::string Current;
    Current.reserve(128);

    for (size_t i = 0; i < Zone.size(); i++)
    {
        const auto Byte = Zone[i];

        // Printable ASCII only. Asset and bone names are all in this range, and letting
        // anything else in turns the output into noise.
        if (Byte >= 0x20 && Byte < 0x7F)
        {
            Current.push_back((char)Byte);
            continue;
        }

        // Only a null terminator makes it a string rather than a run of bytes that
        // happened to look like text
        if (Byte == 0 && Current.size() >= MinimumLength)
        {
            Found.emplace_back((uint64_t)(i - Current.size()), Current);
        }

        Current.clear();
    }

    return Found;
}


// Decrypts a signed WWII fast file to a temporary and reads that back as an ordinary
// unsigned container. The decryptor rewrites the magic to the unsigned spelling, so the
// second pass lands on the plain Sledgehammer path rather than back here.
static bool DecryptAndDecompressWorldWar2(const std::string& FilePath,
    std::vector<uint8_t>& Zone, std::string& Error)
{
    std::vector<uint8_t> Plain;

    if (!WorldWar2Crypto::Decrypt(FilePath, Plain, Error))
    {
        return false;
    }

    char TempPath[MAX_PATH] = {};
    char TempFile[MAX_PATH] = {};

    if (GetTempPathA(MAX_PATH, TempPath) == 0 ||
        GetTempFileNameA(TempPath, "ghw", 0, TempFile) == 0)
    {
        Error = "could not make a temporary file for the decrypted fast file";
        return false;
    }

    {
        FILE* Raw = nullptr;
        if (fopen_s(&Raw, TempFile, "wb") != 0 || Raw == nullptr)
        {
            Error = "could not write the decrypted fast file";
            return false;
        }

        auto Owned = std::unique_ptr<FILE, decltype(&fclose)>(Raw, fclose);
        fwrite(Plain.data(), 1, Plain.size(), Owned.get());
    }

    const auto Ok = FastFile::Decompress(TempFile, Zone, Error);

    if (!Ok)
    {
        Error += std::format(" [decrypted {} bytes, kept at {}]",
            (unsigned long long)Plain.size(), TempFile);
    }
    else
    {
        remove(TempFile);
    }

    return Ok;
}

bool FastFile::Decompress(const std::string& FilePath, std::vector<uint8_t>& Zone, std::string& Error)
{
    Zone.clear();
    Error.clear();

    const auto Info = Identify(FilePath);

    if (Info.Format == FastFileFormat::Unknown)
    {
        Error = std::format("unrecognised container, magic '{}'", Info.Magic.c_str());
        return false;
    }

    if (Info.Format == FastFileFormat::TreyarchFlexible)
    {
        return DecompressTreyarchT9(FilePath, Zone, Error);
    }

    if (Info.Format == FastFileFormat::Registered)
    {
        std::vector<uint8_t> File;
        return ReadWholeFile(FilePath, File, Error) &&
               DecodeRegisteredFastFile(Info.Magic, Info.Version, FilePath, File, Zone, Error);
    }

    if (Info.Format == FastFileFormat::TreyarchChunked)
    {
        if (IsBO4TreyarchVersion(Info.Version))
        {
            return DecompressTreyarchT8(FilePath, Info, Zone, Error);
        }
        return DecompressTreyarch(FilePath, Info, Zone, Error);
    }

    // A signed WWII fast file is encrypted; decrypting produces an ordinary unsigned container,
    // so it is written out and read back through the normal path instead of teaching the block
    // walker about ciphertext. Must sit ahead of the Sledgehammer dispatch below, or that would
    // hand the ciphertext straight to the block walker.
    if (Info.Format == FastFileFormat::SledgehammerLZ4 && Info.Version == WorldWar2Version &&
        Info.Magic == "S1ff0100")
    {
        return DecryptAndDecompressWorldWar2(FilePath, Zone, Error);
    }

    if (Info.Format == FastFileFormat::SledgehammerLZ4)
    {
        return DecompressSledgehammer(FilePath, Info, Zone, Error);
    }

    if (Info.Format == FastFileFormat::TreyarchEncrypted)
    {
        return DecompressTreyarchT6(FilePath, Info, Zone, Error);
    }

    if (Info.Format == FastFileFormat::InfinityWardBlocked)
    {
        return DecompressInfinityWardBlocked(FilePath, Info, Zone, Error);
    }

    if (Info.Format != FastFileFormat::InfinityWardZLib && Info.Format != FastFileFormat::InfinityWardAuthedZLib)
    {
        // Deliberately refused rather than half attempted: producing a truncated zone
        // silently would be worse than saying so.
        Error = std::format("container '{}' (version {}) is not supported yet", Info.Magic.c_str(), Info.Version);
        return false;
    }

    auto Handle = std::unique_ptr<FILE, decltype(&fclose)>(nullptr, fclose);
    {
        FILE* Raw = nullptr;
        if (fopen_s(&Raw, FilePath.c_str(), "rb") != 0 || Raw == nullptr)
        {
            Error = "could not open the file";
            return false;
        }
        Handle.reset(Raw);
    }

    // Skip the header, everything after it is one zlib stream. HeaderExtraBytes is the 9
    // bytes the 64-bit rebuilds of MW2/MW3 wedge in front of theirs; zero for every other title.
    if (_fseeki64(Handle.get(), (int64_t)(HeaderSize + Info.HeaderExtraBytes), SEEK_SET) != 0)
    {
        Error = "file is shorter than its header";
        return false;
    }

    // The zone size is not in the header, so inflate as a stream and grow as we go
    mz_stream Stream = {};
    if (mz_inflateInit(&Stream) != MZ_OK)
    {
        Error = "could not start the inflater";
        return false;
    }

    std::vector<uint8_t> InputChunk(256 * 1024);
    std::vector<uint8_t> OutputChunk(1024 * 1024);
    bool Finished = false;

    uint64_t BytesIntoGroup = 0;

    while (!Finished)
    {
        // Never read across a hash block: stop at the group boundary, step over the block, go on.
        size_t Want = InputChunk.size();
        if (Info.AuthedGroupBytes > 0)
        {
            const uint64_t Left = Info.AuthedGroupBytes - BytesIntoGroup;
            if (Left < (uint64_t)Want)
                Want = (size_t)Left;
        }

        const auto Read = fread(InputChunk.data(), 1, Want, Handle.get());

        if (Info.AuthedGroupBytes > 0 && Read > 0)
        {
            BytesIntoGroup += (uint64_t)Read;
            if (BytesIntoGroup >= Info.AuthedGroupBytes)
            {
                BytesIntoGroup = 0;
                if (_fseeki64(Handle.get(), (int64_t)Info.AuthedSkipBytes, SEEK_CUR) != 0)
                {
                    Error = "file ended inside a hash block";
                    mz_inflateEnd(&Stream);
                    return false;
                }
            }
        }

        if (Read == 0 && Stream.avail_in == 0)
        {
            Error = "stream ended before the zone was complete";
            mz_inflateEnd(&Stream);
            return false;
        }

        Stream.next_in = InputChunk.data();
        Stream.avail_in = (unsigned int)Read;

        // Drain everything this input chunk can produce
        while (Stream.avail_in > 0 || Stream.avail_out == 0)
        {
            Stream.next_out = OutputChunk.data();
            Stream.avail_out = (unsigned int)OutputChunk.size();

            const auto Result = mz_inflate(&Stream, MZ_NO_FLUSH);
            const auto Produced = OutputChunk.size() - Stream.avail_out;

            if (Produced > 0)
            {
                Zone.insert(Zone.end(), OutputChunk.begin(), OutputChunk.begin() + Produced);
            }

            if (Result == MZ_STREAM_END)
            {
                Finished = true;
                break;
            }

            if (Result != MZ_OK)
            {
                Error = std::format("inflate failed with {} after {} bytes",
                    Result, (unsigned long long)Zone.size());
                mz_inflateEnd(&Stream);
                return false;
            }

            // No progress and nothing left to feed it, go read more input
            if (Produced == 0 && Stream.avail_in == 0)
            {
                break;
            }
        }
    }

    mz_inflateEnd(&Stream);

    if (Zone.empty())
    {
        Error = "the zone came out empty";
        return false;
    }

    return true;
}
