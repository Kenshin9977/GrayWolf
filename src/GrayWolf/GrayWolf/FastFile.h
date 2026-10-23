#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The container variant a Fast File uses. The asset layout inside the zone differs per
// title as well, but the outer container only comes in a handful of shapes.
enum class FastFileFormat
{
    // Not a Fast File, or a variant we do not recognise
    Unknown,
    // 12 byte header followed by one zlib stream. Used by CoD4, World at War and the
    // titles around them, magic "IWffu100" or "IWff0100".
    InfinityWardZLib,
    // The same zlib stream, but with a hash block wedged in every couple of megabytes: the
    // signed variant Ghosts ships ("IWff0100", version 565). Identical to the above once those
    // blocks are skipped, which is what AuthedGroupBytes/AuthedSkipBytes describe.
    InfinityWardAuthedZLib,
    // Treyarch's chunked container, magic "TAff0000". A fixed header followed by a run of
    // blocks, each introduced by its own compressed and decompressed sizes. Shared as-is by
    // Black Ops 3 (T7) and Black Ops 4 (T8) - same magic, same per-block shape, told apart only
    // by the version field (BO4 is 0x265/0x27E/0x27F) which also picks a different, larger fixed
    // header and adds Oodle as a possible per-block algorithm (FastFile.Treyarch.cpp's
    // DecompressTreyarchT8).
    TreyarchChunked,
    // Sledgehammer's container, magic "S1ffu100". A 0x38 byte header followed by LZ4
    // blocks, each introduced by its two sizes and starting on a four byte boundary.
    SledgehammerLZ4,
    // Black Ops 2, magic "TAff0100". A 0x138 byte header followed by Salsa20 encrypted
    // blocks, each introduced by its size alone. Unsigned, "TAffu100" (OAT's Linker): the
    // blocks right after the 12-byte header, keyed by the file's name.
    TreyarchEncrypted,
    // Infinite Warfare, magic "IWff0100"/1619. Not encrypted despite looking it: an
    // authentication header of hashes precedes LZ4 blocks, with a fresh hash block spliced in every 8 MB.
    InfinityWardBlocked,
    // Black Ops Cold War, magic "TAFF" (or signed "TAFA"): typed header sections, then BO4's
    // block headers; its .fd update patch is applied when it sits beside the .ff
    // (FastFile.T9.cpp).
    TreyarchFlexible,
    // A container a game brings itself (FastFileContainerRegistration), matched by magic and version.
    Registered,
};

// A game's own container, registered from Games/<game>/ by a static instance: the whole file in,
// the zone out.
using FastFileContainer = bool (*)(const std::vector<uint8_t>& File, std::vector<uint8_t>& Zone, std::string& Error);
// One that also reads files beside it (an update patch next to the fast file) gets its path too.
using FastFilePathContainer = bool (*)(const std::string& FilePath, const std::vector<uint8_t>& File, std::vector<uint8_t>& Zone, std::string& Error);
struct FastFileContainerRegistration
{
    FastFileContainerRegistration(const char* Magic, uint32_t Version, FastFileContainer Decode);
    FastFileContainerRegistration(const char* Magic, uint32_t Version, FastFilePathContainer Decode);
};
// Whether a game registered that magic and version
bool HasFastFileContainer(const std::string& Magic, uint32_t Version);
// The registered container's decode of FilePath (whose bytes are File)
bool DecodeRegisteredFastFile(const std::string& Magic, uint32_t Version, const std::string& FilePath,
    const std::vector<uint8_t>& File, std::vector<uint8_t>& Zone, std::string& Error);

// How the blocks of a Treyarch fast file are compressed. Values are the game's own.
enum class FastFileCompression : uint8_t
{
    None = 0,
    ZLib = 1,
    ZLibHighCompression = 2,
    LZ4 = 3,
    LZ4HighCompression = 4,
    BDeltaUncompressed = 5,
    BDeltaZLib = 6,
    BDeltaLZMA = 7,
    OodleKraken = 8,
    OodleMermaid = 9,
    OodleSelkie = 10,
    OodleLZNA = 11,
};

// What a Fast File header told us before any decompression happened
struct FastFileInfo
{
    FastFileFormat Format = FastFileFormat::Unknown;
    // The eight magic bytes, as text, for reporting
    std::string Magic;
    // Version field that follows the magic. Identifies the title more precisely than the
    // magic does: 5 is CoD4, 387 is World at War, 473 is Black Ops, 593 is Black Ops 3.
    uint32_t Version = 0;

    // Only meaningful for the Treyarch container
    FastFileCompression Compression = FastFileCompression::None;
    bool Encrypted = false;
    // Total size of the zone the blocks add up to, as the header claims it
    uint64_t DeclaredZoneSize = 0;
    // Where the container's own header starts. Zero except for the signed Sledgehammer
    // variant, which puts a signature and a hash table in front of it.
    uint64_t DataOffset = 0;
    // Bytes inserted between the 12-byte header and the payload. Zero everywhere except the
    // 64-bit rebuilds of MW2/MW3, which put 9 bytes - a flag and a build stamp -
    // in front of their zlib stream.
    uint64_t HeaderExtraBytes = 0;

    // Signed Infinity Ward zones interleave a hash block into the compressed stream: every
    // AuthedGroupBytes of payload is followed by AuthedSkipBytes of hashes that are not part of
    // the stream at all. Ghosts writes 2 MB groups with an 8 KB block between them (the same
    // 0x2000 authed chunk size the earlier titles use, 256 chunks to a group). Zero means the
    // stream is continuous, which is every other title here.
    uint64_t AuthedGroupBytes = 0;
    uint64_t AuthedSkipBytes = 0;

    // Bytes of table spliced into the header before its last fields. WWII only.
    uint64_t SplicedTableBytes = 0;
};

// What differs between one title's zone and the next, up to Black Ops 3: where the content
// starts, whether depend-zones are listed, the block-index pointer bit width, and the asset type
// names, renumbered on almost every title.
struct ZoneLayout
{
    // Whether this title's zone shape is known at all
    bool Known = false;
    // Where the content starts: the two sizes, then one size per block
    uint64_t ContentOffset = 0;
    // Black Ops 2 alone lists the zones it depends on, between the strings and the assets
    bool HasDepends = false;
    // Pointer width of the zone's CONTENT. Four everywhere except the 64-bit rebuilds of
    // MW2/MW3, whose header stayed 32-bit while everything behind it went to eight.
    uint32_t PointerSize = 4;

    // How many low bits of a pointer hold the block index
    uint32_t BlockBits = 3;

    // This title's asset type names, so a zone can be reported as what it holds rather
    // than as a list of numbers
    const char* const* TypeNames = nullptr;
    size_t TypeNameCount = 0;

    // For reporting
    std::string Title;
};

// One entry of a zone's asset list
struct ZoneAsset
{
    // The game's own asset type id. GrayWolf relies on five of these reading Black Ops 3 out
    // of memory: 3 xanim, 4 xmodel, 9 image, 0x26 fx, 0x2F rawfile.
    uint32_t Type = 0;
    // Whether the asset's header follows inline in the stream, or lives in one of the
    // blocks the engine allocates up front
    bool Inline = false;
    // Which block it lives in, when it is not inline
    uint32_t Block = 0;
};

// What could be read out of a zone without replaying the engine's pointer fixup
struct ZoneInfo
{
    // Number of entries in the zone's string table
    uint32_t StringCount = 0;
    // How many of those entries actually have a string behind them. Slot zero never does,
    // so this is normally one less than the count.
    uint32_t StringsInline = 0;
    // Where the string data begins, just past the pointer array
    uint64_t StringTableOffset = 0;
    // The strings themselves, in table order
    std::vector<std::string> Strings;

    // Number of assets the zone declares
    uint32_t AssetCount = 0;
    // Where the asset list begins, just past the string data
    uint64_t AssetListOffset = 0;
    // The list itself, in order
    std::vector<ZoneAsset> Assets;
};

// Reads Fast Files. Only the container is handled here: the result is the decompressed
// zone, which still has to be parsed per title to reach the assets inside it.
class FastFile
{
public:

    // Reads just the header, without decompressing anything
    static FastFileInfo Identify(const std::string& FilePath);

    // Decompresses a Fast File into its zone. Returns false and fills Error on failure.
    static bool Decompress(const std::string& FilePath, std::vector<uint8_t>& Zone, std::string& Error);

    // Pulls the null terminated strings out of a zone.
    //
    // This is a heuristic, not a parse. A zone is a memory image the engine fixes up at
    // load time, and reaching its assets properly means replaying that fixup per title.
    // Until then, the strings are what identify what a zone holds - bone names, material
    // names, asset names - so scanning for them is a genuinely useful first look, as long
    // as nobody mistakes it for having understood the layout.
    static std::vector<std::pair<uint64_t, std::string>> ScanStrings(const std::vector<uint8_t>& Zone, size_t MinimumLength = 4);

    // Reads the string table of a Black Ops 3 zone properly, rather than scanning for
    // anything that looks like text.
    //
    // A Treyarch zone opens with the number of strings it holds, then one eight byte slot
    // per string - all 0xFFFFFFFFFFFFFFFF, since the engine fills them in at load time -
    // and then the string data itself. So the table starts at 0x30 + count * 8.
    //
    // This is the string table only. The asset list that follows still needs the pointer
    // fixup replayed to make sense of, which is not done.
    static bool ParseTreyarchZone(const std::vector<uint8_t>& Zone, ZoneInfo& Out, std::string& Error);

    // Everything before Black Ops 3 keeps its zone in 32 bits, and the four titles here
    // differ only in a handful of numbers. Rather than four near identical parsers, the
    // numbers are the parameter.
    //
    // Layouts follow Laupetin/OpenAssetTools, which is GPL-3.0 like this project.
    static ZoneLayout LayoutFor(const FastFileInfo& Info);

    // Reads the string table and asset list of a 32 bit zone
    static bool Parse32BitZone(const std::vector<uint8_t>& Zone, const ZoneLayout& Layout, ZoneInfo& Out, std::string& Error);

    // The same, for the 64-bit rebuilds of MW2/MW3: a 32-bit zone header followed by a
    // 64-bit, padded asset list. Separate from Parse32BitZone rather than a flag inside it,
    // because every offset and every stride in the two differs.
    static bool ParseIWZone(const std::vector<uint8_t>& Zone, const ZoneLayout& Layout, ZoneInfo& Out, std::string& Error);

    // Reads the string table and asset list of an Advanced Warfare zone.
    //
    // 64 bit, and unlike every other title here the marker meaning "follows inline" is
    // 0xFDFDFDFFFFFFFFFF rather than all ones - the top bytes are a tag the engine puts on
    // every zone pointer. Layout from Joelrau/x64-zt.
    static bool ParseSledgehammerZone(const std::vector<uint8_t>& Zone, ZoneInfo& Out, std::string& Error);

private:

    // Walks the block list of a Treyarch fast file
    static bool DecompressTreyarch(const std::string& FilePath, const FastFileInfo& Info, std::vector<uint8_t>& Zone, std::string& Error);

    // Walks the block list of a Black Ops 4 (T8) fast file - same container shape as
    // DecompressTreyarch above (magic "TAff0000", self-describing per-block headers), a different
    // fixed header and compression set. Ported from acts' decompressor_t78.cpp.
    static bool DecompressTreyarchT8(const std::string& FilePath, const FastFileInfo& Info, std::vector<uint8_t>& Zone, std::string& Error);
    // Black Ops Cold War (FastFile.T9.cpp): the .ff's blocks, its .fd applied when beside it
    static bool DecompressTreyarchT9(const std::string& FilePath, std::vector<uint8_t>& Zone, std::string& Error);

    // Walks the block list of a Sledgehammer fast file
    static bool DecompressSledgehammer(const std::string& FilePath, const FastFileInfo& Info, std::vector<uint8_t>& Zone, std::string& Error);

    // Decrypts and walks the block list of a Black Ops 2 fast file
    static bool DecompressTreyarchT6(const std::string& FilePath, const FastFileInfo& Info, std::vector<uint8_t>& Zone, std::string& Error);

    // Walks the block list of an Infinite Warfare fast file
    static bool DecompressInfinityWardBlocked(const std::string& FilePath, const FastFileInfo& Info, std::vector<uint8_t>& Zone, std::string& Error);
};
