#include "stdafx.h"

// Sledgehammer containers: Advanced Warfare and WWII, LZ4 blocks.

#include "stdafx.h"

#include "FastFile.h"

#include "Compression.h"

#include "FastFile.Internal.h"

#include <format>

using namespace FastFileInternal;

// DB_LoadXFile (0x140242200, AW MP dump) reads magic, version, four flag bytes and two u32
// (24 bytes), then a u32 count (at most 0xD780) and that many 24-byte records, then two u64:
// 44 + count * 24 bytes before the signature block or the stream.
uint64_t FastFileInternal::SledgehammerTableBytes(FILE* Handle)
{
    constexpr int64_t CountOffset = 24;
    constexpr uint64_t EntrySize = 24;
    constexpr uint32_t MaxEntries = 0xD780;

    uint32_t Count = 0;
    if (_fseeki64(Handle, CountOffset, SEEK_SET) != 0 ||
        fread(&Count, 1, sizeof(Count), Handle) != sizeof(Count) || Count > MaxEntries)
    {
        return 0;
    }
    return (uint64_t)Count * EntrySize;
}

bool FastFile::DecompressSledgehammer(const std::string& FilePath, const FastFileInfo& Info, std::vector<uint8_t>& Zone, std::string& Error)
{
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

    if (_fseeki64(Handle.get(), 0, SEEK_END) != 0)
    {
        Error = "could not measure the file";
        return false;
    }

    const auto Length = (uint64_t)_ftelli64(Handle.get());
    const auto HeaderEnd = Info.DataOffset + Info.SplicedTableBytes +
        (Info.Version == WorldWar2Version ? WorldWar2HeaderSize : SledgehammerHeaderSize);

    if (Length <= HeaderEnd)
    {
        Error = "file is shorter than its header";
        return false;
    }

    // Advanced Warfare ships fast files past half a gigabyte, so the blocks are streamed
    // one at a time rather than the whole file being read in the way Treyarch's is
    if (Info.DeclaredZoneSize > 0)
    {
        ReserveZone(Zone, Info.DeclaredZoneSize, Length);
    }

    // A signed Advanced Warfare file interleaves a 0x4000-byte hash block after every 0x800000
    // bytes of data, counted from where the stream starts (its zone size field). Positions
    // below are logical (hash blocks removed); Physical() maps one to the file.
    const bool Authed = Info.Version != WorldWar2Version && Info.Magic == "S1ff0100" &&
                        Info.DataOffset == SledgehammerSignedDataOffset;
    constexpr uint64_t AuthGroup = 0x800000;
    constexpr uint64_t AuthHole = 0x4000;
    const uint64_t AuthStart = Info.DataOffset + Info.SplicedTableBytes + SledgehammerZoneSizeOffset;

    const auto Physical = [&](uint64_t Logical)
    {
        if (!Authed || Logical < AuthStart)
        {
            return Logical;
        }
        return Logical + ((Logical - AuthStart) / AuthGroup) * AuthHole;
    };

    // Reads Count logical bytes, stepping over any hash block in between
    const auto ReadLogical = [&](uint64_t Logical, uint8_t* Into, uint64_t Count)
    {
        while (Count > 0)
        {
            auto Chunk = Count;
            if (Authed && Logical >= AuthStart)
            {
                const auto Left = AuthGroup - (Logical - AuthStart) % AuthGroup;
                Chunk = std::min(Chunk, Left);
            }
            if (_fseeki64(Handle.get(), (int64_t)Physical(Logical), SEEK_SET) != 0 ||
                fread(Into, 1, (size_t)Chunk, Handle.get()) != Chunk)
            {
                return false;
            }
            Logical += Chunk;
            Into += Chunk;
            Count -= Chunk;
        }
        return true;
    };

    std::vector<uint8_t> Block;
    uint64_t Position = HeaderEnd;

    while (Physical(Position) + sizeof(SledgehammerBlockHeader) <= Length)
    {
        SledgehammerBlockHeader Header = {};
        if (!ReadLogical(Position, (uint8_t*)&Header, sizeof(Header)))
        {
            Error = std::format("could not read the block header at 0x{:x}", (unsigned long long)Physical(Position));
            return false;
        }

        // The list ends by running out, not by a terminator, so a zero sized block is the
        // end of it rather than something to step over
        if (Header.CompressedSize == 0 || Header.UncompressedSize == 0)
        {
            break;
        }

        Position += sizeof(Header);

        if (Physical(Position + Header.CompressedSize) > Length)
        {
            Error = std::format("block at 0x{:x} runs past the end of the file",
                (unsigned long long)Physical(Position - sizeof(Header)));
            return false;
        }

        if (!InflatesWithin(Header.UncompressedSize, Header.CompressedSize))
        {
            Error = std::format("block at 0x{:x} claims {} bytes from {}",
                (unsigned long long)Physical(Position - sizeof(Header)), Header.UncompressedSize, Header.CompressedSize);
            return false;
        }

        Block.resize(Header.CompressedSize);
        if (!ReadLogical(Position, Block.data(), Block.size()))
        {
            Error = std::format("could not read the block at 0x{:x}", (unsigned long long)Physical(Position));
            return false;
        }

        const auto ZoneOffset = Zone.size();
        Zone.resize(ZoneOffset + Header.UncompressedSize);

        const auto Produced = Compression::DecompressLZ4Block(
            (const int8_t*)Block.data(), (int8_t*)(Zone.data() + ZoneOffset),
            (int32_t)Header.CompressedSize, (int32_t)Header.UncompressedSize);

        if (Produced != Header.UncompressedSize)
        {
            Error = std::format("block at 0x{:x} gave {} bytes instead of {}",
                (unsigned long long)Physical(Position - sizeof(Header)), Produced, Header.UncompressedSize);
            return false;
        }

        Position = (Position + Header.CompressedSize + SledgehammerBlockAlignment - 1) &
                   ~(uint64_t)(SledgehammerBlockAlignment - 1);

        // The block list is not terminated - what follows the last block parses as an enormous
        // filler block, so the declared size is what says when to stop, not the stream itself.
        if (Info.DeclaredZoneSize > 0 && Zone.size() >= Info.DeclaredZoneSize)
        {
            break;
        }
    }

    if (Zone.empty())
    {
        Error = "no blocks were decompressed";
        return false;
    }

    // The header said how big the zone would be; disagreeing means the blocks were misread
    // somewhere, and a plausible-looking short zone is worse than saying so.
    if (Info.DeclaredZoneSize > 0 && Zone.size() != Info.DeclaredZoneSize)
    {
        Error = std::format("zone came to {} bytes, header declared {}",
            (unsigned long long)Zone.size(), (unsigned long long)Info.DeclaredZoneSize);
        return false;
    }

    return true;
}

bool FastFile::ParseSledgehammerZone(const std::vector<uint8_t>& Zone, ZoneInfo& Out, std::string& Error)
{
    Out = {};
    Error.clear();

    // The zone opens with its own size, an external size, and one size per block. Seven
    // blocks, all 64 bit, so the content starts at 0x48.
    constexpr size_t Header = 8 + 8 + 7 * 8;
    constexpr size_t ListSize = 0x28;

    // Every pointer carries a tag in its top bytes. All ones below the tag means the data
    // follows; the value one less means it was already loaded and nothing follows.
    constexpr uint64_t Marker = 0xFDFDFDFFFFFFFFFFull;

    if (Zone.size() < Header + ListSize)
    {
        Error = "zone is too small to hold a header and an asset list";
        return false;
    }

    uint64_t Declared = 0;
    std::memcpy(&Declared, Zone.data(), sizeof(Declared));

    if (Declared != Zone.size() - Header)
    {
        Error = std::format("zone declares {} bytes of content but {} follow its header",
            (unsigned long long)Declared, (unsigned long long)(Zone.size() - Header));
        return false;
    }

    const auto Read64 = [&Zone](size_t At)
    {
        uint64_t Value = 0;
        std::memcpy(&Value, Zone.data() + At, sizeof(Value));
        return Value;
    };

    const auto StringCount = (uint32_t)Read64(Header);
    const auto Strings = Read64(Header + 8);
    const auto AssetCount = (uint32_t)Read64(Header + 0x10);
    const auto Assets = Read64(Header + 0x18);
    const auto Globals = Read64(Header + 0x20);

    Out.StringCount = StringCount;
    Out.AssetCount = AssetCount;

    size_t At = Header + ListSize;

    if (Strings == Marker && !ReadStringList(Zone, 8, StringCount, Marker, At, Out, Error))
    {
        return false;
    }

    if (Assets != Marker)
    {
        Out.AssetListOffset = At;
        return true;
    }

    // The globals sit between the strings and the asset array - misleading, since they are
    // the field list's last member. Footprint is a wrapper pointer, the struct, and three
    // counted arrays; the trap is three MORE arrays behind those living in the runtime block,
    // whose pointer fields still carry the inline marker but contribute nothing to the file -
    // a reader that consumes on the marker alone desyncs here.
    if (Globals == Marker)
    {
        constexpr size_t WrapperSize = 8;
        constexpr size_t GlobalsSize = 104;

        if (At + WrapperSize + GlobalsSize > Zone.size())
        {
            Error = "the graphics globals run past the end of the zone";
            return false;
        }

        // The wrapper is one pointer at the front; the struct follows it
        const auto GlobalsAt = At + WrapperSize;

        const auto Count32 = [&Zone](size_t Where)
        {
            uint32_t Value = 0;
            std::memcpy(&Value, Zone.data() + Where, sizeof(Value));
            return (uint64_t)Value;
        };

        const auto DepthStencil = Count32(GlobalsAt + 0x00);
        const auto Blend = Count32(GlobalsAt + 0x04);
        const auto PerPrim = Count32(GlobalsAt + 0x28);
        const auto PerObject = Count32(GlobalsAt + 0x2C);
        const auto Stable = Count32(GlobalsAt + 0x30);

        // Eight bytes a depth stencil entry, twelve a blend entry, four each for the three
        // constant buffer size arrays. The buffers themselves are runtime and cost nothing.
        const auto Trailing =
            DepthStencil * 8 + Blend * 12 + (PerPrim + PerObject + Stable) * 4;

        const auto End = At + WrapperSize + GlobalsSize + Trailing;

        if (End > Zone.size())
        {
            Error = std::format("graphics globals need {} bytes at 0x{:x}, past the end of a {} byte zone",
                (unsigned long long)(WrapperSize + GlobalsSize + Trailing),
                (unsigned long long)At, (unsigned long long)Zone.size());
            return false;
        }

        At = (size_t)End;
    }

    // Where the globals are absent or land somewhere unexpected, fall back to finding the
    // asset array by its shape: a run of exactly the declared count of type+pointer entries
    // is not something arbitrary bytes produce - same reasoning the string table earns.
    const auto Plausible = [&](size_t Candidate)
    {
        if (Candidate + (uint64_t)AssetCount * 16 > Zone.size())
        {
            return false;
        }

        for (uint32_t i = 0; i < AssetCount; i++)
        {
            uint32_t Type = 0;
            std::memcpy(&Type, Zone.data() + Candidate + (size_t)i * 16, sizeof(Type));

            uint32_t Padding = 0;
            std::memcpy(&Padding, Zone.data() + Candidate + (size_t)i * 16 + 4, sizeof(Padding));

            const auto Pointer = Read64(Candidate + (size_t)i * 16 + 8);

            // Types run to a couple of hundred, the four bytes behind them are padding,
            // and the pointer is either the marker or carries the engine's tag
            if (Type > 0x200 || Padding != 0)
            {
                return false;
            }

            if (Pointer != Marker && (Pointer >> 40) != (Marker >> 40))
            {
                return false;
            }
        }

        return true;
    };

    size_t Found = 0;
    bool Located = false;

    for (auto Candidate = At; Candidate + 16 <= Zone.size(); Candidate += 8)
    {
        if (Plausible(Candidate))
        {
            Found = Candidate;
            Located = true;
            break;
        }
    }

    if (!Located)
    {
        Error = std::format("could not find a run of {} asset entries after 0x{:x}",
            AssetCount, (unsigned long long)At);
        return false;
    }

    At = Found;
    Out.AssetListOffset = At;
    Out.Assets.reserve(AssetCount);

    for (uint32_t i = 0; i < AssetCount; i++)
    {
        uint32_t Type = 0;
        std::memcpy(&Type, Zone.data() + At + (size_t)i * 16, sizeof(Type));

        const auto Pointer = Read64(At + (size_t)i * 16 + 8);

        ZoneAsset Asset;
        Asset.Type = Type;
        Asset.Inline = Pointer == Marker;
        // A block reference tags the block index into the low nibble of the top word
        Asset.Block = Asset.Inline ? 0 : (uint32_t)((Pointer >> 32) & 0xF);
        Out.Assets.push_back(Asset);
    }

    return true;
}
