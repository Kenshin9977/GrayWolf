#include "stdafx.h"

// Treyarch containers: Black Ops 2's encrypted blocks, the chunked format Black Ops 3 and 4
// share, and their zone parser.

#include "stdafx.h"

#include "FastFile.h"

#include <miniz/miniz.h>
#include "Compression.h"
#include "Salsa20.h"
#include "Siren.h"
#include "VCDiff.h"

#include <fstream>
#include <filesystem>
#include <format>
#include "FastFile.Internal.h"

using namespace FastFileInternal;

bool FastFile::DecompressTreyarchT6(const std::string& FilePath, const FastFileInfo& Info, std::vector<uint8_t>& Zone, std::string& Error)
{
    std::vector<uint8_t> File;
    if (!ReadWholeFile(FilePath, File, Error))
    {
        return false;
    }

    // An unsigned zone ("TAffu100", OpenAssetTools' Linker's) has no auth header: the blocks follow
    // the magic and version, and the IV table is built from the file's name (OAT's zone name, the
    // file name without its extension)
    const bool Unsigned = Info.Magic == "TAffu100";
    const size_t HeaderSize = Unsigned ? 12 : T6HeaderSize;
    if (File.size() <= HeaderSize)
    {
        Error = "file is shorter than its header";
        return false;
    }

    // The name the IV table is built from is the one in the header, not the one on disk
    const auto* NameBytes = (const char*)File.data() + T6NameOffset;
    const std::string Name = Unsigned ? std::filesystem::path(FilePath).stem().string() : std::string(NameBytes, strnlen(NameBytes, T6NameSize));

    if (Name.empty())
    {
        Error = "the header carries no fast file name to build the key stream from";
        return false;
    }

    std::vector<uint8_t> Table(T6IVTableSize, 0);
    FillIVTable(Table.data(), Table.size(), Name);

    // Blocks are handed to four streams in rotation, and each stream keeps its own place
    // in the hash table
    size_t HashIndex[T6Streams] = { 0, 0, 0, 0 };
    size_t BlockNumber = 0;
    size_t Position = HeaderSize;
    std::vector<uint8_t> Block;

    // The table is indexed by hash block first, then by stream
    const auto EntryFor = [](size_t Index, size_t Stream)
    {
        return (Index * T6Streams + Stream) * T6IVEntrySize;
    };

    while (Position + sizeof(int32_t) <= File.size())
    {
        // The game reads the file through a 0x80000-byte buffer: a block size that would straddle
        // its end starts the next buffer instead, the bytes left before it are padding (OAT's
        // vanilla buffer; mp_downhill.ff is the install's one zone where it happens)
        if (Position % T6VanillaBuffer + sizeof(int32_t) > T6VanillaBuffer)
            Position += T6VanillaBuffer - Position % T6VanillaBuffer;
        if (Position + sizeof(int32_t) > File.size())
            break;
        int32_t Size = 0;
        std::memcpy(&Size, File.data() + Position, sizeof(Size));
        Position += sizeof(Size);

        // A zero size closes the list
        if (Size == 0)
        {
            break;
        }

        if (Size < 0 || Position + (size_t)Size > File.size())
        {
            Error = std::format("block {} declares {} bytes, which runs past the end of the file",
                (unsigned long long)BlockNumber, Size);
            return false;
        }

        Block.assign(File.begin() + Position, File.begin() + Position + Size);
        Position += Size;

        const auto Stream = BlockNumber % T6Streams;

        // The nonce is the first eight bytes of this stream's current hash block
        uint8_t Nonce[8] = {};
        std::memcpy(Nonce, Table.data() + EntryFor(HashIndex[Stream], Stream), sizeof(Nonce));

        uint8_t Key[sizeof(T6Key)];
        std::memcpy(Key, T6Key, sizeof(Key));

        if (s20_crypt(Key, S20_KEYLEN_256, Nonce, 0, Block.data(), (uint32_t)Block.size()) != S20_SUCCESS)
        {
            Error = std::format("could not decrypt block {}", (unsigned long long)BlockNumber);
            return false;
        }

        // The digest is of the still-compressed cipher output. The stream then moves to its
        // NEXT hash block and XORs the digest into that one (not the one just used) - getting
        // either wrong desyncs at the fifth block, the first to read a block feedback touched.
        uint8_t Digest[T6IVEntrySize] = {};
        Sha1Raw(Block.data(), Block.size(), Digest);

        HashIndex[Stream] = (HashIndex[Stream] + 1) % T6HashBlockCount;

        auto* Next = Table.data() + EntryFor(HashIndex[Stream], Stream);

        for (size_t i = 0; i < sizeof(Digest); i++)
        {
            Next[i] ^= Digest[i];
        }

        // What comes out is deflate compressed, and the zone grows as we go since the
        // header does not say how large it will be
        if (!InflateInto(Block, Zone, Error, true))
        {
            Error = std::format("block at 0x{:x} did not inflate after decryption ({}), first bytes {:02X} {:02X} {:02X} {:02X}",
                (unsigned long long)(Position - Size - sizeof(Size)), Error.c_str(),
                Block[0], Block.size() > 1 ? Block[1] : 0,
                Block.size() > 2 ? Block[2] : 0, Block.size() > 3 ? Block[3] : 0);
            return false;
        }

        BlockNumber++;
    }

    if (Zone.empty())
    {
        Error = "no blocks were decompressed";
        return false;
    }

    return true;
}

bool FastFile::DecompressTreyarch(const std::string& FilePath, const FastFileInfo& Info, std::vector<uint8_t>& Zone, std::string& Error)
{
    if (Info.Encrypted)
    {
        // Decrypting needs the per fast file AES key, which we do not have
        Error = "the fast file is encrypted";
        return false;
    }

    const bool IsZLib = Info.Compression == FastFileCompression::ZLib ||
                        Info.Compression == FastFileCompression::ZLibHighCompression;
    const bool IsLZ4 = Info.Compression == FastFileCompression::LZ4 ||
                       Info.Compression == FastFileCompression::LZ4HighCompression;
    const bool IsNone = Info.Compression == FastFileCompression::None;

    if (!IsZLib && !IsLZ4 && !IsNone)
    {
        Error = std::format("block compression {} is not supported yet", (uint32_t)Info.Compression);
        return false;
    }

    // Blocks reference absolute positions, so the whole file is easiest to work from
    std::vector<uint8_t> File;
    {
        FILE* Raw = nullptr;
        if (fopen_s(&Raw, FilePath.c_str(), "rb") != 0 || Raw == nullptr)
        {
            Error = "could not open the file";
            return false;
        }

        auto Owned = std::unique_ptr<FILE, decltype(&fclose)>(Raw, fclose);

        if (_fseeki64(Owned.get(), 0, SEEK_END) != 0)
        {
            Error = "could not measure the file";
            return false;
        }

        const auto Length = _ftelli64(Owned.get());
        if (Length <= 0)
        {
            Error = "the file is empty";
            return false;
        }

        File.resize((size_t)Length);
        _fseeki64(Owned.get(), 0, SEEK_SET);

        if (fread(File.data(), 1, File.size(), Owned.get()) != File.size())
        {
            Error = "could not read the whole file";
            return false;
        }
    }

    const size_t HeaderBytes = (Info.Version > TreyarchAlphaVersion) ? TreyarchHeaderSize : TreyarchHeaderAlpha;

    if (File.size() <= HeaderBytes)
    {
        Error = "file is shorter than its header";
        return false;
    }

    if (Info.DeclaredZoneSize > 0)
    {
        ReserveZone(Zone, Info.DeclaredZoneSize, File.size());
    }

    size_t Position = HeaderBytes;

    while (Position + sizeof(TreyarchBlockHeader) <= File.size())
    {
        TreyarchBlockHeader Block = {};
        std::memcpy(&Block, File.data() + Position, sizeof(Block));

        // Each block records where it starts, which is a cheap way to notice we have
        // drifted out of step rather than decompressing rubbish
        if (Block.Offset != Position)
        {
            Error = std::format("block at 0x{:x} claims to be at 0x{:x}",
                (unsigned long long)Position, Block.Offset);
            return false;
        }

        Position += sizeof(TreyarchBlockHeader);

        // An empty block is padding up to the next alignment boundary
        if (Block.UncompressedSize == 0)
        {
            const auto Aligned = ((Position + TreyarchBlockAlignment - 1) / TreyarchBlockAlignment) * TreyarchBlockAlignment;
            if (Aligned >= File.size())
            {
                break;
            }
            Position = (size_t)Aligned;
            continue;
        }

        // the bytes the block is read from (all of them when stored) lie within the file
        const uint64_t Stored = IsNone ? Block.UncompressedSize : Block.CompressedSize;
        if (Position + Block.AlignedSize > File.size() || Position + Stored > File.size())
        {
            Error = std::format("block at 0x{:x} runs past the end of the file", Block.Offset);
            return false;
        }
        if (!IsNone && !InflatesWithin(Block.UncompressedSize, Block.CompressedSize))
        {
            Error = std::format("block at 0x{:x} claims {} bytes from {}", Block.Offset, Block.UncompressedSize, Block.CompressedSize);
            return false;
        }

        const auto* BlockData = File.data() + Position;
        const auto ZoneOffset = Zone.size();
        Zone.resize(ZoneOffset + Block.UncompressedSize);

        if (IsNone)
        {
            std::memcpy(Zone.data() + ZoneOffset, BlockData, Block.UncompressedSize);
        }
        else if (IsLZ4)
        {
            const auto Produced = Compression::DecompressLZ4Block(
                (const int8_t*)BlockData, (int8_t*)(Zone.data() + ZoneOffset),
                (int32_t)Block.CompressedSize, (int32_t)Block.UncompressedSize);

            if (Produced != Block.UncompressedSize)
            {
                Error = std::format("block at 0x{:x} gave {} bytes instead of {}",
                    Block.Offset, Produced, Block.UncompressedSize);
                return false;
            }
        }
        else
        {
            // The blocks carry a normal zlib header, they just declare a smaller window
            // than usual, so the default wrapper aware init is what we want
            mz_stream Stream = {};
            if (mz_inflateInit(&Stream) != MZ_OK)
            {
                Error = "could not start the inflater";
                return false;
            }

            Stream.next_in = BlockData;
            Stream.avail_in = Block.CompressedSize;
            Stream.next_out = Zone.data() + ZoneOffset;
            Stream.avail_out = Block.UncompressedSize;

            const auto Result = mz_inflate(&Stream, MZ_FINISH);
            const auto Produced = Stream.total_out;
            mz_inflateEnd(&Stream);

            if (Result != MZ_STREAM_END || Produced != Block.UncompressedSize)
            {
                Error = std::format("block at 0x{:x} failed to inflate ({}, {} of {} bytes)",
                    Block.Offset, Result, (unsigned long long)Produced, Block.UncompressedSize);
                return false;
            }
        }

        Position += Block.AlignedSize;

        // The header tells us how big the zone is, so stop rather than run off the end
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

    return true;
}

// A Black Ops 4 .fd is a patch on the .ff of the same name: its own header (0x840), a copy of
// the .ff's header (0x840), a 0x28-byte patch header whose first u64 is where the zlib data
// starts, then zlib'd VCDIFF whose source is the whole decompressed .ff zone. The result is the
// zone the game's loader reads.
static bool ApplyBo4Patch(const std::string& FilePath, std::vector<uint8_t>& Zone, std::string& Error)
{
    std::vector<uint8_t> File;
    {
        std::ifstream In(FilePath, std::ios::binary | std::ios::ate);
        if (!In)
        {
            Error = "could not open the patch";
            return false;
        }
        File.resize((size_t)In.tellg());
        In.seekg(0);
        if (!In.read(reinterpret_cast<char*>(File.data()), (std::streamsize)File.size()))
        {
            Error = "could not read the whole patch";
            return false;
        }
    }
    constexpr size_t PatchHeader = 0x1080;
    if (File.size() < PatchHeader + 0x28)
    {
        Error = "the patch is shorter than its headers";
        return false;
    }
    uint64_t DataStart = 0;
    std::memcpy(&DataStart, File.data() + PatchHeader, 8);
    if (DataStart < PatchHeader + 0x28 || DataStart >= File.size())
    {
        Error = std::format("the patch data offset 0x{:x} is out of range", (unsigned long long)DataStart);
        return false;
    }

    // the source: the .ff of the same name, next to the .fd
    std::filesystem::path SourcePath(FilePath);
    SourcePath.replace_extension(".ff");
    if (!std::filesystem::exists(SourcePath))
    {
        Error = "the patch needs its base fast file next to it: " + SourcePath.string();
        return false;
    }
    std::vector<uint8_t> Source;
    std::string SourceError;
    if (!FastFile::Decompress(SourcePath.string(), Source, SourceError))
    {
        Error = "the base fast file did not decompress: " + SourceError;
        return false;
    }

    std::vector<uint8_t> Delta;
    if (!FastFileInternal::InflateZLibStreams(File.data() + DataStart, File.size() - (size_t)DataStart, Delta, Error))
    {
        return false;
    }

    std::string VCDiffError;
    if (!VCDiff::Decode(Source, Delta.data(), Delta.size(), Zone, VCDiffError))
    {
        Error = "VCDIFF decode failed: " + VCDiffError;
        return false;
    }
    return true;
}

bool FastFileInternal::InflateZLibStreams(const uint8_t* Data, const size_t Size, std::vector<uint8_t>& Delta,
                                          std::string& Error)
{
    size_t In = 0;
    while (In < Size && Data[In] == 0x78)
    {
        mz_stream S = {};
        if (mz_inflateInit(&S) != MZ_OK)
        {
            Error = "could not start the inflater";
            return false;
        }
        S.next_in = Data + In;
        S.avail_in = (unsigned int)(Size - In);
        int Result = MZ_OK;
        while (Result == MZ_OK)
        {
            const size_t Had = Delta.size();
            Delta.resize(Had + 0x400000);
            S.next_out = Delta.data() + Had;
            S.avail_out = 0x400000;
            Result = mz_inflate(&S, MZ_NO_FLUSH);
            Delta.resize(Had + (0x400000 - S.avail_out));
        }
        const size_t Used = (size_t)S.total_in;
        mz_inflateEnd(&S);
        if (Result != MZ_STREAM_END)
        {
            Error = std::format("the patch failed to inflate ({}) at 0x{:x}", Result, (unsigned long long)In);
            return false;
        }
        In += Used;
    }
    return true;
}

// Walks the block list of a Black Ops 4 (T8) fast file. Same outer shape as
// DecompressTreyarch's Black Ops 3 container, different fixed header, Oodle joining zlib/LZ4/none.
bool FastFile::DecompressTreyarchT8(const std::string& FilePath, const FastFileInfo& Info, std::vector<uint8_t>& Zone, std::string& Error)
{
    if (Info.Encrypted)
    {
        // Deliberately not implemented: acts' per-zone AES-256 keys (data/keys/keys_aes.csv, mostly
        // *_ffotd_*) are an external filename-keyed table that appears nowhere in the game's files, and
        // this fork's policy is that a decryption key must be recoverable from the game's own files
        // (see Bo3MovieDecrypt.cpp for the pattern that IS allowed).
        Error = "the fast file is encrypted (Black Ops 4 zone AES keys are not recoverable from "
                "the game's own files)";
        return false;
    }

    const bool IsZLib = Info.Compression == FastFileCompression::ZLib ||
                        Info.Compression == FastFileCompression::ZLibHighCompression;
    const bool IsLZ4 = Info.Compression == FastFileCompression::LZ4 ||
                       Info.Compression == FastFileCompression::LZ4HighCompression;
    const bool IsNone = Info.Compression == FastFileCompression::None;
    const bool IsOodle = Info.Compression == FastFileCompression::OodleKraken ||
                         Info.Compression == FastFileCompression::OodleMermaid ||
                         Info.Compression == FastFileCompression::OodleSelkie ||
                         Info.Compression == FastFileCompression::OodleLZNA;
    const bool IsBDeltaZLib = Info.Compression == FastFileCompression::BDeltaZLib;

    if (!IsZLib && !IsLZ4 && !IsNone && !IsOodle && !IsBDeltaZLib)
    {
        Error = std::format("block compression {} is not supported yet", (uint32_t)Info.Compression);
        return false;
    }

    if (IsBDeltaZLib)
    {
        return ApplyBo4Patch(FilePath, Zone, Error);
    }

    if (IsOodle)
    {
        // Siren wraps a real Oodle DLL loaded from disk, as XPAKCache and XSUBCache do.
        // BO4 uses oo2core_6_win64.dll.
        static const bool SirenReady = Siren::Initialize(L"oo2core_6_win64.dll") == SirenStatus::Success;
        if (!SirenReady)
        {
            Error = "could not load an Oodle dll for Black Ops 4 block decompression";
            return false;
        }
    }

    // Blocks reference absolute positions, so the whole file is easiest to work from
    std::vector<uint8_t> File;
    {
        FILE* Raw = nullptr;
        if (fopen_s(&Raw, FilePath.c_str(), "rb") != 0 || Raw == nullptr)
        {
            Error = "could not open the file";
            return false;
        }

        auto Owned = std::unique_ptr<FILE, decltype(&fclose)>(Raw, fclose);

        if (_fseeki64(Owned.get(), 0, SEEK_END) != 0)
        {
            Error = "could not measure the file";
            return false;
        }

        const auto Length = _ftelli64(Owned.get());
        if (Length <= 0)
        {
            Error = "the file is empty";
            return false;
        }

        File.resize((size_t)Length);
        _fseeki64(Owned.get(), 0, SEEK_SET);

        if (fread(File.data(), 1, File.size(), Owned.get()) != File.size())
        {
            Error = "could not read the whole file";
            return false;
        }
    }

    const size_t HeaderBytes = (Info.Version == BO4DevVersion) ? BO4DevHeaderSize : BO4RetailHeaderSize;

    if (File.size() <= HeaderBytes)
    {
        Error = "file is shorter than its header";
        return false;
    }

    if (Info.DeclaredZoneSize > 0)
    {
        ReserveZone(Zone, Info.DeclaredZoneSize, File.size());
    }

    size_t Position = HeaderBytes;

    while (Position + sizeof(TreyarchBlockHeader) <= File.size())
    {
        TreyarchBlockHeader Block = {};
        std::memcpy(&Block, File.data() + Position, sizeof(Block));

        // Each block records where it starts, which is a cheap way to notice we have
        // drifted out of step rather than decompressing rubbish
        if (Block.Offset != Position)
        {
            Error = std::format("block at 0x{:x} claims to be at 0x{:x}",
                (unsigned long long)Position, Block.Offset);
            return false;
        }

        Position += sizeof(TreyarchBlockHeader);

        // An empty block is padding up to the next alignment boundary
        if (Block.UncompressedSize == 0)
        {
            const auto Aligned = ((Position + TreyarchBlockAlignment - 1) / TreyarchBlockAlignment) * TreyarchBlockAlignment;
            if (Aligned >= File.size())
            {
                break;
            }
            Position = (size_t)Aligned;
            continue;
        }

        // the bytes the block is read from (all of them when stored) lie within the file
        const uint64_t Stored = IsNone ? Block.UncompressedSize : Block.CompressedSize;
        if (Position + Block.AlignedSize > File.size() || Position + Stored > File.size())
        {
            Error = std::format("block at 0x{:x} runs past the end of the file", Block.Offset);
            return false;
        }
        if (!IsNone && !(IsOodle ? OodleInflatesWithin(Block.UncompressedSize, Block.CompressedSize)
                                 : InflatesWithin(Block.UncompressedSize, Block.CompressedSize)))
        {
            Error = std::format("block at 0x{:x} claims {} bytes from {}", Block.Offset, Block.UncompressedSize, Block.CompressedSize);
            return false;
        }

        const auto* BlockData = File.data() + Position;

        const auto ZoneOffset = Zone.size();
        Zone.resize(ZoneOffset + Block.UncompressedSize);

        if (IsNone)
        {
            std::memcpy(Zone.data() + ZoneOffset, BlockData, Block.UncompressedSize);
        }
        else if (IsLZ4)
        {
            const auto Produced = Compression::DecompressLZ4Block(
                (const int8_t*)BlockData, (int8_t*)(Zone.data() + ZoneOffset),
                (int32_t)Block.CompressedSize, (int32_t)Block.UncompressedSize);

            if (Produced != Block.UncompressedSize)
            {
                Error = std::format("block at 0x{:x} gave {} bytes instead of {}",
                    Block.Offset, Produced, Block.UncompressedSize);
                return false;
            }
        }
        else if (IsOodle)
        {
            const auto Produced = Siren::Decompress(BlockData, Block.CompressedSize,
                Zone.data() + ZoneOffset, Block.UncompressedSize);

            if (Produced != Block.UncompressedSize)
            {
                Error = std::format("block at 0x{:x} gave {} bytes instead of {}",
                    Block.Offset, (unsigned long long)Produced, Block.UncompressedSize);
                return false;
            }
        }
        else
        {
            // Same zlib-wrapped format T7 blocks use (default wrapper-aware inflate init), as acts' own T8
            // decompressor does through plain uncompress2().
            mz_stream Stream = {};
            if (mz_inflateInit(&Stream) != MZ_OK)
            {
                Error = "could not start the inflater";
                return false;
            }

            Stream.next_in = BlockData;
            Stream.avail_in = Block.CompressedSize;
            Stream.next_out = Zone.data() + ZoneOffset;
            Stream.avail_out = Block.UncompressedSize;

            const auto Result = mz_inflate(&Stream, MZ_FINISH);
            const auto Produced = Stream.total_out;
            mz_inflateEnd(&Stream);

            if (Result != MZ_STREAM_END || Produced != Block.UncompressedSize)
            {
                Error = std::format("block at 0x{:x} failed to inflate ({}, {} of {} bytes)",
                    Block.Offset, Result, (unsigned long long)Produced, Block.UncompressedSize);
                return false;
            }
        }

        Position += Block.AlignedSize;

        // The header tells us how big the zone is, so stop rather than run off the end
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

    return true;
}

bool FastFile::ParseTreyarchZone(const std::vector<uint8_t>& Zone, ZoneInfo& Out, std::string& Error)
{
    Out = {};
    Error.clear();

    // Where the per string pointer slots begin
    constexpr size_t PointerArrayOffset = 0x30;

    if (Zone.size() < PointerArrayOffset)
    {
        Error = "zone is too small to hold a header";
        return false;
    }

    uint32_t Count = 0;
    std::memcpy(&Count, Zone.data(), sizeof(Count));

    // A count large enough to run past the zone means we misread the header rather than
    // that the zone really holds that many strings
    const auto TableOffset = PointerArrayOffset + (uint64_t)Count * 8;

    // Equal is fine: a zone holding neither strings nor assets is all header and nothing
    // else, which several of the small patch fast files genuinely are.
    if (TableOffset > Zone.size())
    {
        Error = std::format("string count {} puts the table at 0x{:x}, past the end of a {} byte zone",
            Count, (unsigned long long)TableOffset, (unsigned long long)Zone.size());
        return false;
    }

    Out.StringCount = Count;
    Out.StringTableOffset = TableOffset;
    Out.Strings.reserve(Count);

    size_t Position = (size_t)TableOffset;

    for (uint32_t i = 0; i < Count; i++)
    {
        // A slot only has a string behind it when the engine was told to fill it in from the
        // stream - reading one for every slot regardless walks off the string data and into
        // the asset list.
        uint64_t Slot = 0;
        std::memcpy(&Slot, Zone.data() + PointerArrayOffset + (size_t)i * 8, sizeof(Slot));

        if (Slot != FollowsInline)
        {
            continue;
        }

        Out.StringsInline++;

        const auto Start = Position;

        while (Position < Zone.size() && Zone[Position] != 0)
        {
            Position++;
        }

        if (Position >= Zone.size())
        {
            Error = std::format("string {} of {} runs off the end of the zone", i, Count);
            return false;
        }

        Out.Strings.emplace_back((const char*)Zone.data() + Start, Position - Start);
        Position++;
    }

    // The asset list follows the strings. Its length is stated in the header rather than
    // having to be guessed at, which is what makes this a parse rather than a scan.
    uint32_t AssetCount = 0;
    std::memcpy(&AssetCount, Zone.data() + AssetCountOffset, sizeof(AssetCount));

    Out.AssetCount = AssetCount;
    Out.AssetListOffset = Position;

    if (Position + (uint64_t)AssetCount * sizeof(ZoneAssetEntry) > Zone.size())
    {
        Error = std::format("asset count {} needs 0x{:x} bytes at 0x{:x}, past the end of a {} byte zone",
            AssetCount, (unsigned long long)((uint64_t)AssetCount * sizeof(ZoneAssetEntry)),
            (unsigned long long)Position, (unsigned long long)Zone.size());
        return false;
    }

    Out.Assets.reserve(AssetCount);

    for (uint32_t i = 0; i < AssetCount; i++)
    {
        ZoneAssetEntry Entry = {};
        std::memcpy(&Entry, Zone.data() + Position + (size_t)i * sizeof(Entry), sizeof(Entry));

        // Every entry is a type and a pointer the engine fixes up: either the follows-inline
        // marker, or a block+offset packed together - anything else is not an asset list.
        const auto Block = (uint32_t)(Entry.Header >> BlockPointerShift);

        if (Entry.Padding != 0 || (Entry.Header != FollowsInline && Block > MaxXFileBlock))
        {
            Error = std::format("asset {} of {} has type {} and pointer 0x{:x}, which is not an asset entry",
                i, AssetCount, Entry.Type, (unsigned long long)Entry.Header);
            return false;
        }

        ZoneAsset Asset;
        Asset.Type = Entry.Type;
        Asset.Inline = Entry.Header == FollowsInline;
        Asset.Block = Asset.Inline ? 0 : Block;
        Out.Assets.push_back(Asset);
    }

    return true;
}
