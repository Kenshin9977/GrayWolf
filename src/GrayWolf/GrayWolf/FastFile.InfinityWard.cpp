#include "stdafx.h"

// Infinity Ward containers: the blocked variant Infinite Warfare uses, and the zone parser
// shared by CoD4 through Ghosts.

#include "stdafx.h"

#include "FastFile.h"

#include "Compression.h"

#include "FastFile.Internal.h"

#include <format>

using namespace FastFileInternal;

bool FastFile::DecompressInfinityWardBlocked(const std::string& FilePath, const FastFileInfo& Info, std::vector<uint8_t>& Zone, std::string& Error)
{
    std::vector<uint8_t> File;
    if (!ReadWholeFile(FilePath, File, Error))
    {
        return false;
    }

    // The header is a fixed 0xE8 with a table of streamed file records spliced into it,
    // one per shared and per image file, so where it ends depends on two counts inside it
    constexpr size_t CountsOffset = 0xC4;
    constexpr size_t ImageCountOffset = 0xCC;
    constexpr size_t BaseHeader = 0xE8;
    constexpr size_t StreamFileStride = 24;

    // Then an authentication header and hash block, and a fresh hash block every this many
    // bytes of stream. That splice is the whole difficulty: a compressed block runs straight
    // through one, so it cannot just be skipped between blocks.
    constexpr size_t AuthAndMaster = 0x8000;
    constexpr size_t SecureChunk = 0x800000;
    constexpr size_t HashBlock = 0x4000;

    // "IWC" behind a compressor byte of 2
    constexpr uint32_t CompressorMagic = 0x43574902;

    if (File.size() < BaseHeader + 16)
    {
        Error = "file is shorter than its header";
        return false;
    }

    uint32_t Shared = 0;
    uint32_t Images = 0;
    std::memcpy(&Shared, File.data() + CountsOffset, sizeof(Shared));
    std::memcpy(&Images, File.data() + ImageCountOffset, sizeof(Images));

    const auto HeaderEnd = BaseHeader + (uint64_t)(Shared + Images) * StreamFileStride;

    if (HeaderEnd + 4 > File.size())
    {
        Error = std::format("stream file table of {} entries runs past the end of the file",
            Shared + Images);
        return false;
    }

    // Signed files put the authentication header and the first hash block in front of the
    // stream; unsigned ones would not. Rather than assume, look for the compressor.
    uint64_t DataAt = 0;
    bool Located = false;

    for (const auto Candidate : { HeaderEnd, HeaderEnd + AuthAndMaster })
    {
        if (Candidate + 16 > File.size())
        {
            continue;
        }

        uint32_t Magic = 0;
        std::memcpy(&Magic, File.data() + Candidate, sizeof(Magic));

        if (Magic == CompressorMagic)
        {
            DataAt = Candidate;
            Located = true;
            break;
        }
    }

    if (!Located)
    {
        Error = std::format("no compressor header at 0x{:x} or 0x{:x}",
            (unsigned long long)HeaderEnd, (unsigned long long)(HeaderEnd + AuthAndMaster));
        return false;
    }

    // Flatten the stream by dropping the spliced hash blocks, so the block walk below can
    // read straight through what would otherwise be holes
    std::vector<uint8_t> Stream;
    Stream.reserve(File.size() - (size_t)DataAt);

    for (uint64_t Logical = 0; DataAt + Logical + (Logical / SecureChunk) * HashBlock < File.size(); )
    {
        const auto Physical = DataAt + Logical + (Logical / SecureChunk) * HashBlock;
        const auto UntilHole = SecureChunk - (Logical % SecureChunk);
        const auto Available = File.size() - (size_t)Physical;
        const auto Take = (size_t)std::min<uint64_t>(UntilHole, Available);

        Stream.insert(Stream.end(), File.data() + Physical, File.data() + Physical + Take);
        Logical += Take;

        if (Take < UntilHole)
        {
            break;
        }
    }

    if (Stream.size() < 16)
    {
        Error = "the compressed stream is too short to hold its header";
        return false;
    }

    uint64_t Declared = 0;
    std::memcpy(&Declared, Stream.data() + 4, sizeof(Declared));

    Zone.clear();
    FastFileInternal::ReserveZone(Zone, Declared, Stream.size());

    size_t At = 16;
    std::vector<uint8_t> Block;

    while (Zone.size() < Declared && At + 12 <= Stream.size())
    {
        uint32_t Compressed = 0;
        uint64_t Uncompressed = 0;
        std::memcpy(&Compressed, Stream.data() + At, sizeof(Compressed));
        std::memcpy(&Uncompressed, Stream.data() + At + 4, sizeof(Uncompressed));
        At += 12;

        if (Compressed == 0 || Uncompressed == 0 || At + Compressed > Stream.size() ||
            !FastFileInternal::InflatesWithin(Uncompressed, Compressed))
        {
            break;
        }

        const auto ZoneAt = Zone.size();
        Zone.resize(ZoneAt + (size_t)Uncompressed);

        const auto Produced = Compression::DecompressLZ4Block(
            (const int8_t*)(Stream.data() + At), (int8_t*)(Zone.data() + ZoneAt),
            (int32_t)Compressed, (int32_t)Uncompressed);

        if (Produced != (int32_t)Uncompressed)
        {
            Error = std::format("block at 0x{:x} gave {} bytes instead of {}",
                (unsigned long long)At, Produced, (unsigned long long)Uncompressed);
            return false;
        }

        // Each block's data is padded out to a four byte boundary
        At = (At + Compressed + 3) & ~(size_t)3;
    }

    if (Zone.empty())
    {
        Error = "no blocks were decompressed";
        return false;
    }

    if (Zone.size() != Declared)
    {
        Error = std::format("zone came to {} bytes, the stream declared {}",
            (unsigned long long)Zone.size(), (unsigned long long)Declared);
        return false;
    }

    return true;
}

bool FastFile::ParseIWZone(const std::vector<uint8_t>& Zone, const ZoneLayout& Layout, ZoneInfo& Out, std::string& Error)
{
    Out = {};
    Error.clear();

    if (!Layout.Known)
    {
        Error = "no zone layout is known for this title";
        return false;
    }

    const auto Content = (size_t)Layout.ContentOffset;
    const auto ListSize = 32u;

    if (Zone.size() < Content + ListSize)
    {
        Error = "zone is too small to hold a header and an asset list";
        return false;
    }

    if (!ContentMatches(Zone, Content, Error))
    {
        return false;
    }

    const auto StringCount = ReadU32(Zone, Content);
    const auto StringsPtr = ReadU64(Zone, Content + 8);
    const auto AssetCount = ReadU32(Zone, Content + 16);
    const auto AssetsPtr = ReadU64(Zone, Content + 24);

    Out.StringCount = StringCount;
    Out.AssetCount = AssetCount;

    size_t At = Content + ListSize;

    // The strings, when they are there at all
    if (StringsPtr == FollowsInline && !ReadStringList(Zone, 8, StringCount, FollowsInline, At, Out, Error))
    {
        return false;
    }

    if (AssetsPtr != FollowsInline)
    {
        Out.AssetListOffset = At;
        return true;
    }

    if (At + (uint64_t)AssetCount * 16 > Zone.size())
    {
        Error = std::format("asset count {} needs {} bytes at 0x{:x}, past the end of a {} byte zone",
            AssetCount, (unsigned long long)((uint64_t)AssetCount * 16),
            (unsigned long long)At, (unsigned long long)Zone.size());
        return false;
    }

    Out.AssetListOffset = At;
    Out.Assets.reserve(AssetCount);

    for (uint32_t i = 0; i < AssetCount; i++)
    {
        const auto Type = ReadU32(Zone, At + (size_t)i * 16);
        const auto Header = ReadU64(Zone, At + (size_t)i * 16 + 8);

        ZoneAsset Asset;
        Asset.Type = Type;
        Asset.Inline = Header == FollowsInline;
        Asset.Block = 0;
        Out.Assets.push_back(Asset);
    }

    return true;
}
