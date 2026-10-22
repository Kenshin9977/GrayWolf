#pragma once

// ZoneTableWalker - walks a decompressed zone with reader tables transcribed from the game's
// own loaders. The game comes from a zonekit profile (zonekit/profiles/<game>): pointer markers,
// zone format, type names, tables.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ZoneTables
{
    struct Tables;   // compiled tables (opaque)

    // `### name` tables from the files, in order; a later file overrides a same-named table.
    Tables* LoadTables(const std::vector<std::string>& Paths);
    void FreeTables(Tables* T);

    // The stream conventions of one game (profile.json "stream" / "zone_format").
    struct StreamRules
    {
        uint64_t Marker = 0xFFFFFFFFFFFFFFFFull;    // the pointer is followed inline
        uint64_t Resident = 0xFFFFFFFFFFFFFFFEull;  // ... or resident (assets, `*` actions)
        std::vector<uint64_t> Other;                // further asset-slot markers (IW7's -3)
        // the value whose data insert_ptr gives a slot (resident_slot.marker; IW7 -3)
        uint64_t SlotMarker = 0xFFFFFFFFFFFFFFFEull;
        bool Block32 = false;                       // backref_style "block32": block << 32 | offset + 1
        // stream.backref_asset_bit (IW8: 36): a block32 reference with this bit set counts its offset
        // from where the zone-level asset holding it began in that block (0x2d88c10's frame 0)
        int AssetBit = 0;
        bool AssetRelative(uint64_t V) const { return AssetBit > 0 && ((V >> AssetBit) & 1) != 0; }
        // stream.pointer_ifs: pointers transcribed as IF chains on their field, one arm per marker
        bool PointerIfs = false;
        // stream.keyed_refs (Black Ops 6/7): a pointer is streamed inline when bit InlineBit is set
        // (the IF arm `(u64@X >> 61) & 1`), and unless bit NoKeyBit is set too the game files its
        // data under the key value & KeyMask (ResolveStreamPositionEx); a non-null pointer without
        // the inline bit is a back reference by that key, an asset pointer (its chain tests `& Asset
        // mask`) one by the asset's own name hash (value & AssetHashMask). 0: off.
        int InlineBit = 0, NoKeyBit = 0;
        uint64_t KeyMask = 0, AssetHashMask = 0;
        // stream.rewind_prepare (Black Ops 7): pointers DB_RewindPrepare decides, linked by their
        // top bits and the rewind table's keys, and asset pointers by the asset's hash (PointerIf)
        bool RewindRefs = false;
        uint64_t BackrefMaxNibble = 0xA;            // top nibble of a packed back reference
        bool Iw6Refs = false;                       // IW6: 0xFDFDFDF<block><offset + 1> instead
        // export.both_marker_assets: a SUB* row (a pointer taking both markers, the resident one
        // only an asset loader inserts) is an asset pointer, its back reference resolved with
        // resolve_deref, for a profile whose every such reader calls it
        bool BothMarkerAssets = false;
        // profile "arch": a pointer field's width, 8, or 4 on x86, whose packed
        // back reference is block = (v - 1) >> BackrefShift in the low dword, offset the bits below
        // (stream.backref_shift: 28 for CoD4, 29 for World at War's and Black Ops' 7 blocks)
        int PointerSize = 8;
        int BackrefShift = 28;
        // backref_style "block28" (MW2/MW3 64-bit): v - 1 = block << 28 | offset in the low dword,
        // the block one of the stream's; 0 otherwise. x86 packs the same way (CoD4), bounded by the
        // nibble above instead.
        int RefShift = 0;
        int StreamBlocks = 0;
        // a back reference's block and offset, by the style; false when V is not one
        bool DecodeRef(uint64_t V, int& Block, int64_t& Off) const
        {
            if (V == 0 || V == Marker || V == Resident) return false;
            if (Block32)
            {
                if (AssetRelative(V)) V &= ~(1ull << AssetBit);
                if ((int64_t)(V >> 32) >= StreamBlocks || (V & 0xFFFFFFFFull) == 0) return false;
                Block = (int)(V >> 32);
                Off = (int64_t)(V & 0xFFFFFFFFull) - 1;
                return true;
            }
            if (Iw6Refs)
            {
                if ((V >> 36) != 0xFDFDFDFull) return false;
                Block = (int)((V >> 32) & 0xF);
                Off = (int64_t)(V & 0xFFFFFFFFull) - 1;
                return true;
            }
            if (RefShift || PointerSize == 4)
            {
                const int Shift = RefShift ? RefShift : BackrefShift;
                const uint64_t W = (V - 1) & 0xFFFFFFFFull;   // the low dword only
                const uint64_t B = W >> Shift;
                if (RefShift ? (int64_t)B >= StreamBlocks : B > BackrefMaxNibble) return false;
                Block = (int)B;
                Off = (int64_t)(W & ((1ull << Shift) - 1));
                return true;
            }
            if ((V >> 60) > BackrefMaxNibble) return false;
            Block = (int)(V >> 60);
            Off = (int64_t)((V - 1) & 0x0FFFFFFFFFFFFFFFull);
            return true;
        }
        std::string ZoneFormat = "t8";              // the zone header parser
        // profile "asset_type_map" (Black Ops 6/7): the asset list names a type by the FNV-1a 32
        // of its name; type id by that hash (empty for the other profiles)
        std::unordered_map<uint32_t, int> TypeByNameHash;
        // profile "zone_load": its numbers by key ("unk_list.ptr_at" for a nested one), for a
        // format's own options
        std::unordered_map<std::string, int64_t> ZoneLoad;
        int64_t ZoneLoadValue(const std::string& Key, int64_t Default) const
        {
            const auto It = ZoneLoad.find(Key);
            return It == ZoneLoad.end() ? Default : It->second;
        }
        // profile "zone_header": the Treyarch header's size and where its
        // asset count and asset list pointer sit (the string count and pointer are at 0 and 8)
        int64_t HeaderSize = 0x20, HeaderAssetCount = 0x10, HeaderAssets = 0x18;

        // Layout of the stream memory (profile "stream" + blocks.txt), for back references:
        // the blocks that only grow, the slot a resident (-2) asset reserves, and per table
        // action its block and alignment ("table|index", or "caller|index|entered" for a SELF).
        std::vector<int> GrowingBlocks{4, 5};
        int ResidentBlock = 4, ResidentSize = 8, ResidentAlign = 8;
        int StartBlock = 4;                         // current when the first asset loads
        // where in the start block the zone's own reads begin: what the game loaded there first
        // (Infinite Warfare: a zone's shared stream, IW7Zone::LayoutOffset)
        int64_t StartOffset = 0;
        // zone_load.list_refs: a back reference into the asset list names an asset through its
        // header slot (resolve_deref), which the loader points at the loaded asset (WWII)
        bool ListRefs = false;
        // zone_load.prefix_table: the reader table of the zone prefix before the asset list
        // (IW7's XGlobals), walked by the format through PrefixWalk
        std::string PrefixTable;
        const std::unordered_map<std::string, std::pair<int, int>>* Blocks = nullptr;
        // empties.txt: actions whose count-0 allocation still aligns the cursor
        const std::unordered_set<std::string>* Empties = nullptr;
    };

    struct Profile
    {
        std::string Name;
        std::string Dir;
        StreamRules Rules;
        std::vector<std::string> TypeNames;   // [type id] = table name, "" when none
        std::vector<std::string> TablePaths;  // generated, then hand (hand overrides)
        std::unordered_map<std::string, std::pair<int, int>> Blocks;   // blocks.txt
        std::unordered_set<std::string> Empties;                       // empties.txt
    };

    // A profile by directory, or by name looked up in `<exe dir>/zonekit/profiles/<name>`
    // and then in `zonekit/profiles/<name>` of any parent of the exe (a dev build).
    bool LoadProfile(const std::string& DirOrName, Profile& Out, std::string& Error);

    struct ZoneResult
    {
        std::vector<int64_t> Reads;           // sizes; strings negative
        std::vector<uint16_t> Owners;         // per read: index into OwnerNames
        std::vector<std::string> OwnerNames;  // the asset instances reads belong to
        std::string Why;                      // why the walk ended
        bool ParseFailed = false;             // the zone header itself did not parse

        // With Layout: where each read is in the zone, and every asset instance walked (the
        // zone's own assets and the ones inlined inside them), for extraction.
        struct Instance
        {
            uint16_t Owner = 0;       // index into OwnerNames (the table / type name)
            int32_t Parent = -1;      // enclosing instance, -1 for a zone-level asset
            int64_t Pos = 0;          // where its struct starts in the zone buffer
            int64_t End = 0;          // where the walk was when it ended
            uint32_t FirstRead = 0;   // its reads (children included): [FirstRead, EndRead)
            uint32_t EndRead = 0;
        };
        std::vector<int64_t> ReadPos;         // per read: its zone buffer offset
        // per read: block << 40 | offset in the stream memory, -1 outside a growing block
        std::vector<int64_t> ReadMem;
        // pointer fields holding a back reference (block << 60 | offset + 1): field -> value
        std::vector<std::pair<int64_t, uint64_t>> BackRefs;
        // per back reference: it is an asset pointer (ASSET/ASSETS), which the game resolves by
        // reading the pointer at the referenced place (IW6 resolve_deref), not by taking that place
        std::vector<bool> BackRefIsAsset;
        std::vector<uint64_t> AssetHeads;     // the zone's asset list pointers, in list order
        // stream.keyed_refs: each key an inline pointer files its data under -> where that data is
        std::vector<std::pair<uint64_t, int64_t>> Keys;
        // stream.keyed_refs: an asset reference's field -> the type (owner id) its chain reads inline
        std::unordered_map<int64_t, uint16_t> RefTypes;
        // where block 4 stands when the first asset loads: the string table (slots, then the
        // strings at their file length) aligned to 8, then the asset list (16 bytes each). The
        // list is at [Start4 - 16 * assets, Start4).
        // IW6: block 3, align4(list file offset - 0x70) + 16 * assets.
        int64_t Start4 = 0;
        // With Layout: each growing block's cursor where the walk ended (block -> offset)
        std::unordered_map<int, int64_t> BlockEnds;
        std::vector<Instance> Instances;
        // pointer field position -> where the data it points to was read (inline pointers only;
        // a back reference into an earlier block is not resolved here)
        std::vector<std::pair<int64_t, int64_t>> Links;
    };

    // TypeNames[tid] = the table name of asset type tid.
    ZoneResult WalkZone(const std::vector<uint8_t>& Buf, const Tables* T,
                        const std::vector<std::string>& TypeNames,
                        const StreamRules& Rules = StreamRules(), bool Layout = false);

    // A layout walk's reads by stream memory position: a back reference's (block, offset) back
    // to the zone position of the byte the game had placed there.
    class BlockMap
    {
    public:
        explicit BlockMap(const ZoneResult& R);
        // false when no read filled that byte (a gap, or a block that does not grow)
        bool ZonePos(int Block, int64_t Offset, int64_t& Pos) const;

    private:
        const ZoneResult& R;
        std::unordered_map<int, std::vector<std::pair<int64_t, size_t>>> ByBlock;   // offset, read
    };
}
