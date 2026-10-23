#pragma once

// Container constants, the block header structs, and the small readers every container
// needs, shared by the per-family FastFile.*.cpp files.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "FastFile.h"
#include <format>
#include <miniz/miniz.h>
#include "SHA1.h"

// The pointer fixup marker: "the thing I point at follows here in the stream" - 64 bit form
// for the Treyarch (Black Ops 3) and 64-bit Infinity Ward zones.
constexpr uint64_t FollowsInline = 0xFFFFFFFFFFFFFFFFull;

// The same marker, 32 bit, for the Black Ops 2 and older 32 bit zones.
constexpr uint32_t T6FollowsInline = 0xFFFFFFFFu;

// One entry of the asset list, as it sits in a Treyarch zone.
struct ZoneAssetEntry
{
    uint32_t Type;
    uint32_t Padding;
    uint64_t Header;
};
static_assert(sizeof(ZoneAssetEntry) == 0x10, "the game's asset entry is 16 bytes");

namespace FastFileInternal
{
    // The zlib streams from Data on, one after the other while one starts (0x78), appended to
    // Out: an update patch's VCDIFF (Black Ops 4 and Cold War .fd). FastFile.Treyarch.cpp.
    bool InflateZLibStreams(const uint8_t* Data, size_t Size, std::vector<uint8_t>& Out,
                            std::string& Error);


    // The most a zlib or LZ4 block inflates to (zlib's ~1032:1 bounds LZ4's ~255:1): a header
    // claiming more is corrupt, and must not size a zone of terabytes.
    constexpr uint64_t MaxInflateRatio = 1032;
    inline bool InflatesWithin(uint64_t Uncompressed, uint64_t Compressed)
    {
        return Uncompressed <= Compressed * MaxInflateRatio + 64;
    }
    // Oodle has no such ratio: a 256 KiB chunk of one repeated byte takes a few bytes (Black Ops 4's
    // zones hold 261856-byte blocks stored in 6). Each chunk still needs its 2-byte header.
    constexpr uint64_t OodleChunkSize = 0x40000;
    inline bool OodleInflatesWithin(uint64_t Uncompressed, uint64_t Compressed)
    {
        return Uncompressed <= (Compressed / 2) * OodleChunkSize;
    }
    // A declared zone size as a reserve hint, at most what FileBytes could inflate to
    inline void ReserveZone(std::vector<uint8_t>& Zone, uint64_t Declared, uint64_t FileBytes)
    {
        Zone.reserve((size_t)(Declared < FileBytes * MaxInflateRatio ? Declared : FileBytes * MaxInflateRatio));
    }

    // Every variant starts with eight magic bytes then a little endian version
    constexpr size_t HeaderSize = 12;

    // Treyarch's header is fixed size and grew once. Layout and sizes follow
    // ate47/atian-cod-tools, which is dual licensed GPL-3.0 or MIT.
    constexpr size_t TreyarchHeaderAlpha = 0x1F8;   // Black Ops 3 up to version 0x132
    constexpr size_t TreyarchHeaderSize = 0x248;    // Black Ops 3 after that
    constexpr uint32_t TreyarchAlphaVersion = 0x132;

    // Offset of the declared zone size inside the larger header: magic, version, four
    // flag bytes, timestamp, changelist, checksum, builder, metaVersion, mergeFastfile
    constexpr size_t TreyarchSizeOffset = 8 + 4 + 4 + 8 + 4 + 16 + 32 + 4 + 64;

    // Blocks that carry nothing move the reader on to the next multiple of this
    constexpr uint64_t TreyarchBlockAlignment = 0x800000;

    // Introduces every block of a Treyarch fast file. Also the real shape of Black Ops 4's own
    // `fastfile::DBStreamHeader` (acts: fastfile_data_tre.hpp) - same four uint32 fields, same
    // order (compressedSize, uncompressedSize, alignedSize, offset) - so DecompressTreyarchT8
    // below reuses this struct rather than declaring a second, identical one.
    struct TreyarchBlockHeader
    {
        uint32_t CompressedSize;
        uint32_t UncompressedSize;
        uint32_t AlignedSize;
        uint32_t Offset;
    };

    // Black Ops 4 (T8) versions, from acts' fastfile handler (decompressor_t78.cpp): same outer
    // magic "TAff0000" as Black Ops 3, told apart by version.
    constexpr uint32_t BO4DevVersion = 0x265;    // "Black Ops 4 Dev"
    constexpr uint32_t BO4OldVersion = 0x27E;    // "Black Ops 4 Old"
    constexpr uint32_t BO4RetailVersion = 0x27F; // "Black Ops 4"

    // sizeof(XFileBO4_Dev) / sizeof(XFileBO4_0x27F) in acts' fastfile_data_tre.hpp (static_assert'd
    // there).
    constexpr size_t BO4DevHeaderSize = 0x638;
    constexpr size_t BO4RetailHeaderSize = 0x840; // shared by 0x27E ("Old") and 0x27F ("retail")

    // Offset of the declared decompressed zone size (the `size` field). Identical between
    // XFileBO4_Dev and XFileBO4_0x27F despite their very different total sizes, because both put
    // a 16*64 byte missionFastFiles[] table immediately before it at the same offset (from
    // decompressor_t78.cpp's own numeric offsets).
    constexpr size_t BO4SizeOffset = 0x490;

    inline bool IsBO4TreyarchVersion(uint32_t Version)
    {
        return Version == BO4DevVersion || Version == BO4OldVersion || Version == BO4RetailVersion;
    }

    // Sledgehammer's header, worked out from the files rather than a reference (no published
    // description exists, and atian-cod-tools does not cover it). Everything after the magic
    // and version is sizes; the two that matter are the total uncompressed size at 0x2C and
    // the block list, which starts where the header ends.
    constexpr size_t SledgehammerHeaderSize = 0x38;
    constexpr size_t SledgehammerZoneSizeOffset = 0x2C;

    // WWII is the same container with a longer header: the same zone size / flag bytes /
    // block list shape, just moved (0x88, 0x90, 0x94 against Advanced Warfare's 0x2C, 0x34,
    // 0x38). Its flags end 0x03 where Advanced Warfare's end 0x04 - the per-block compression.
    constexpr size_t WorldWar2HeaderSize = 0x94;
    constexpr size_t WorldWar2ZoneSizeOffset = 0x88;
    constexpr uint32_t WorldWar2HeaderTrailer = 0x03010000;
    constexpr size_t WorldWar2TrailerOffset = 0x90;
    constexpr uint32_t WorldWar2Version = 5;

    // TWO counted tables are spliced into the header before those fields, sixteen bytes an
    // entry: a count at 0x20, its table, then a second count, then its table (Scobalula's
    // s2_ff_killer skips both). Reading only the second works here only because the first is
    // empty in every file checked - a coincidence, not a guarantee.
    constexpr size_t WorldWar2FirstCountOffset = 0x20;
    constexpr size_t WorldWar2EntrySize = 16;

    // The signed variant, "S1ff0100", is the same container with a signature and a hash table
    // pushed in front: a 32 byte digest and 256 bit signature after the "S1ffs100" sub magic,
    // the fast file name, then per-block hashes: 0x8000 bytes in all. In Advanced Warfare it
    // follows the header's counted table, and the stream (u64 zone size, u32 0x04010000, the
    // blocks) follows it: zone size at 0x8000 + table + 0x2C, blocks at 0x8000 + table + 0x38.
    constexpr uint64_t SledgehammerSignedDataOffset = 0x8000;

    // The four bytes that close the header, right before the block list. Constant across every
    // Advanced Warfare fast file here, so it checks whether the signed header landed as expected.
    constexpr uint32_t SledgehammerHeaderTrailer = 0x04010000;
    constexpr size_t SledgehammerTrailerOffset = 0x34;

    // Advanced Warfare: bytes of the counted 24-byte table between the fixed header and the
    // stream (FastFile.Sledgehammer.cpp). Zero when the file cannot be read there.
    uint64_t SledgehammerTableBytes(FILE* Handle);

    // Blocks start on a four byte boundary, so a block whose data ends mid word is padded
    constexpr size_t SledgehammerBlockAlignment = 4;

    // Introduces every block of a Sledgehammer fast file. Unlike Treyarch's, it does not repeat
    // its own offset, so the check is the sizes summing to the declared total and the walk
    // landing exactly on the end of the file.
    struct SledgehammerBlockHeader
    {
        uint32_t CompressedSize;
        uint32_t UncompressedSize;
    };

    // Black Ops 2. Header layout, key and nonce schedule follow ate47/atian-cod-tools (GPL-3.0
    // or MIT) - though its own implementation stops before decrypting, so the block loop below
    // is not a port of working code.
    constexpr size_t T6HeaderSize = 0x138;
    constexpr size_t T6NameOffset = 0x18;
    constexpr size_t T6NameSize = 0x20;
    // Blocks go to four streams in rotation, each keeping its own place in a table of 200 hash
    // blocks (200 * 4 * 20 = the 16000 byte table). Layout and the feedback below follow
    // Laupetin/OpenAssetTools (GPL-3.0) - atian's implementation does not have them.
    constexpr size_t T6Streams = 4;
    constexpr size_t T6VanillaBuffer = 0x80000;   // the file buffer a block size never straddles
    constexpr size_t T6HashBlockCount = 200;
    constexpr size_t T6IVEntrySize = 20;
    constexpr size_t T6IVTableSize = T6HashBlockCount * T6Streams * T6IVEntrySize;

    constexpr uint8_t T6Key[32] = {
        0x64, 0x1D, 0x8A, 0x2F, 0xE3, 0x1D, 0x3A, 0xA6, 0x36, 0x22, 0xBB, 0xC9, 0xCE, 0x85, 0x87, 0x22,
        0x9D, 0x42, 0xB0, 0xF8, 0xED, 0x9B, 0x92, 0x41, 0x30, 0xBF, 0x88, 0xB6, 0x5E, 0xDC, 0x50, 0xBE
    };

    // The table starts as the fast file's own name smeared across it, four bytes per character,
    // cycling. The name field is 32 bytes and holds a terminator, so at most 31 chars are used.
    inline void FillIVTable(uint8_t* Table, size_t Length, const std::string& Name)
    {
        const auto Cycle = std::min<size_t>(Name.size(), 31);

        if (Cycle == 0)
        {
            return;
        }

        size_t At = 0;

        for (size_t i = 0; i + 4 <= Length; i += 4)
        {
            std::memset(Table + i, (uint8_t)Name[At++], 4);
            At %= Cycle;
        }
    }

    // The project's SHA-1 hands back hex, and the table wants the twenty raw bytes
    inline void Sha1Raw(const uint8_t* Data, size_t Length, uint8_t* Digest)
    {
        SHA1 Hash;
        Hash.update(std::string((const char*)Data, Length));
        const auto Hex = Hash.final();

        for (size_t i = 0; i < 20 && (i * 2 + 1) < Hex.size(); i++)
        {
            const auto Nibble = [](char c) -> uint8_t
            {
                if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
                if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
                if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
                return 0;
            };

            Digest[i] = (uint8_t)((Nibble(Hex[i * 2]) << 4) | Nibble(Hex[i * 2 + 1]));
        }
    }

    // Appends one deflate stream to the zone, growing as it goes since nothing says how
    // large the result will be
    inline bool InflateInto(const std::vector<uint8_t>& Source, std::vector<uint8_t>& Zone, std::string& Error, bool Raw = false)
    {
        mz_stream Stream = {};

        // Black Ops 2's blocks carry no zlib wrapper, so the window size has to be given
        // rather than read from a header that is not there
        const auto Started = Raw
            ? mz_inflateInit2(&Stream, -MZ_DEFAULT_WINDOW_BITS)
            : mz_inflateInit(&Stream);

        if (Started != MZ_OK)
        {
            Error = "could not start the inflater";
            return false;
        }

        Stream.next_in = Source.data();
        Stream.avail_in = (unsigned int)Source.size();

        std::vector<uint8_t> Chunk(256 * 1024);
        int Result = MZ_OK;

        do
        {
            Stream.next_out = Chunk.data();
            Stream.avail_out = (unsigned int)Chunk.size();

            Result = mz_inflate(&Stream, MZ_NO_FLUSH);

            if (Result != MZ_OK && Result != MZ_STREAM_END && Result != MZ_BUF_ERROR)
            {
                Error = std::format("inflate returned {}", Result);
                mz_inflateEnd(&Stream);
                return false;
            }

            Zone.insert(Zone.end(), Chunk.data(), Chunk.data() + (Chunk.size() - Stream.avail_out));
        } while (Result != MZ_STREAM_END && Stream.avail_in > 0);

        mz_inflateEnd(&Stream);

        if (Result != MZ_STREAM_END)
        {
            Error = "the stream ended early";
            return false;
        }

        return true;
    }

    // Reads a whole file into memory
    inline bool ReadWholeFile(const std::string& FilePath, std::vector<uint8_t>& Out, std::string& Error)
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

        Out.resize((size_t)Length);
        _fseeki64(Owned.get(), 0, SEEK_SET);

        if (fread(Out.data(), 1, Out.size(), Owned.get()) != Out.size())
        {
            Error = "could not read the whole file";
            return false;
        }

        return true;
    }


    // Any other pointer packs the block it lives in into the top nibble and the offset into
    // the rest. There are ten blocks, so a larger index means this is not a pointer.
    constexpr uint32_t BlockPointerShift = 60;
    constexpr uint32_t MaxXFileBlock = 9;

    // The zone opens with an XAssetList: string count + pointer, two unused fields, then asset
    // count + pointer. Layout follows ate47/atian-cod-tools (GPL-3.0 or MIT).
    constexpr size_t AssetCountOffset = 0x20;


    // A zlib stream opens with a byte whose low nibble is 8 (deflate), and whose first two
    // bytes together are a multiple of 31. The high nibble is the window size, so it is not
    // always the 0x78 of a 32K window - Black Ops 3 packs with a 4K window and opens 0x48.
    inline bool LooksLikeZLib(const uint8_t* Bytes)
    {
        return (Bytes[0] & 0x0F) == 8 && ((((uint32_t)Bytes[0] << 8) | Bytes[1]) % 31u) == 0u;
    }

    // The 64-bit rebuilds of MW2 and MW3 keep the old magic and version but put
    // 9 bytes - a flag and what looks like a build stamp - between the header and the payload,
    // so the deflate stream starts at 21 rather than 12.
    inline bool LooksLikeZLibAt(const uint8_t* Bytes, size_t Offset)
    {
        return LooksLikeZLib(Bytes + Offset);
    }

    inline FastFileFormat FormatFromHeader(const std::string& Magic, uint32_t Version, const uint8_t* AfterHeader, bool HaveBody, size_t AfterHeaderBytes, uint64_t& HeaderExtraBytes)
    {
        HeaderExtraBytes = 0;

        // Ghosts (IW6) writes a table of 24 byte records between the header and the zlib stream, so
        // the stream does not start where every earlier Infinity Ward title put it. The record count
        // sits at absolute offset 24 and the stream at 44 + count * 24, plus a 16 KB signature block
        // on the signed variant ("IWff0100"); no retail zone inflates at the usual offset 12.
        // Version 565 is what separates these from the older titles, which share the magic. This has
        // to come before the plain "zlib right after the header" branch below, which would otherwise
        // match first and fail.
        if ((Magic == "IWffu100" || Magic == "IWff0100") && Version == 565 && AfterHeaderBytes >= 16)
        {
            uint32_t Count = 0;
            std::memcpy(&Count, AfterHeader + 12, sizeof(Count));

            // A corrupt count would push the skip past any real file; 2^24 records is already
            // 400 MB of table, well beyond anything the game ships.
            if (Count <= 0xFFFFFFu)
            {
                const uint64_t Signature = Magic == "IWff0100" ? 16384u : 0u;
                HeaderExtraBytes = 44 + (uint64_t)Count * 24 + Signature - 12;
                return Magic == "IWff0100" ? FastFileFormat::InfinityWardAuthedZLib
                                           : FastFileFormat::InfinityWardZLib;
            }
        }

        // The magic alone is not enough: Infinite Warfare ships "IWff0100" like the older
        // titles but with a completely different payload, so require a real zlib header too.
        if ((Magic == "IWffu100" || Magic == "IWff0100") && HaveBody && LooksLikeZLib(AfterHeader))
        {
            return FastFileFormat::InfinityWardZLib;
        }

        // Infinite Warfare. Same magic as the older titles, told apart by its version.
        if ((Magic == "IWff0100" || Magic == "IWffu100") && Version == 1619)
        {
            return FastFileFormat::InfinityWardBlocked;
        }

        // Black Ops Cold War: "TAFF"/"TAFA" then a section count, not an eight-byte magic
        if (Magic.compare(0, 4, "TAFF") == 0 || Magic.compare(0, 4, "TAFA") == 0)
        {
            return FastFileFormat::TreyarchFlexible;
        }

        // Treyarch, Black Ops 3 onwards
        if (Magic == "TAff0000")
        {
            return FastFileFormat::TreyarchChunked;
        }

        // Black Ops 2: "TAff0100", not Black Ops 3's "TAff0000"; or unsigned, "TAffu100" (the zones
        // OpenAssetTools' Linker builds, version 147: no auth header, the same encrypted blocks)
        if (Magic == "TAff0100" || (Magic == "TAffu100" && Version == 147))
        {
            return FastFileFormat::TreyarchEncrypted;
        }

        // Sledgehammer: Advanced Warfare and WWII, signed and unsigned. They differ by
        // version and by how long the header is, not by the container.
        if (Magic == "S1ffu100" || Magic == "S1ff0100")
        {
            return FastFileFormat::SledgehammerLZ4;
        }

        // Same magic and version as before the 64-bit rebuild, told apart only by where the
        // zlib stream actually begins. Kept last so every title that starts it at 12 is still
        // matched by the branches above first.
        if ((Magic == "IWffu100" || Magic == "IWff0100") && HaveBody && LooksLikeZLibAt(AfterHeader, 9))
        {
            HeaderExtraBytes = 9;
            return FastFileFormat::InfinityWardZLib;
        }

        // Their signed zones (MW2's multiplayer): the same 9 bytes, then the 0x2000-byte auth
        // chunk ("IWffs100") and the first group's 0x2000-byte master hash block before the
        // stream, which then breaks every 256 chunks of 0x2000 for the next group's hash block
        // (OpenAssetTools' ProcessorAuthedBlocks). The hashes are not checked.
        if (Magic == "IWff0100" && (Version == 276 || Version == 1) && AfterHeaderBytes >= 17 &&
            std::memcmp(AfterHeader + 9, "IWffs100", 8) == 0)
        {
            HeaderExtraBytes = 9 + 0x4000;
            return FastFileFormat::InfinityWardAuthedZLib;
        }

        return FastFileFormat::Unknown;
    }


    // Black Ops 2's zone opens with its own content size, an external size, and one size
    // per block. Eight blocks, all 32 bit, so the content starts at 0x28.
    constexpr size_t T6ZoneBlockCount = 8;
    constexpr size_t T6ZoneContentOffset = 8 + T6ZoneBlockCount * 4;


    // The type ids that are read. Black Ops 2 numbers them differently from Black Ops 3 -
    // 41 is a raw file here and 47 there - so nothing carries over.
    constexpr uint32_t T6TypeLocalize = 24;
    constexpr uint32_t T6TypeRawFile = 41;
    constexpr uint32_t T6TypeScriptParseTree = 48;
    constexpr uint32_t T6TypeKeyValuePairs = 49;

    inline uint32_t ReadU32(const std::vector<uint8_t>& Zone, size_t At)
    {
        uint32_t Value = 0;
        std::memcpy(&Value, Zone.data() + At, sizeof(Value));
        return Value;
    }

    // Same, for the 64-bit rebuilds of MW2/MW3: their zone content is all 8-byte pointers.
    inline uint64_t ReadU64(const std::vector<uint8_t>& Zone, size_t At)
    {
        uint64_t Value = 0;
        std::memcpy(&Value, Zone.data() + At, sizeof(Value));
        return Value;
    }


}
