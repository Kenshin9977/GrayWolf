#include "stdafx.h"
#include "ByteView.h"

#include "ZoneFormat.h"

// Modern Warfare 2 (IW4) and Modern Warfare 3 (IW5), the 64-bit rebuild.
// The XFile (IW4 0x28, IW5 0x2c), then the 0x20-byte XAssetList {u32 stringCount, u64 strings,
// u32 assetCount, u64 assets}, both raw-read; the stream starts after it (profile zone_header.size,
// Rules.HeaderSize). Under push(3): the script-string slots (strings tested != 0, the cursor
// aligned to 4) and the text of every -1 slot; then the asset list aligned to 4, 16 bytes an entry.
// Start: block 3's cursor once the list is read, replaying the loader's own alignments.
namespace
{
    int64_t Align(int64_t V, int64_t A) { return (V + A - 1) / A * A; }

    class Iw4ZoneFormat final : public ZoneTables::IZoneFormat
    {
    public:
        const char* Name() const override { return "iw4"; }

        ZoneTables::ZoneHead Read(ByteView Buf, const ZoneTables::StreamRules& Rules) const override
        {
            const ZoneTables::HeaderReader H(Buf);
            const int64_t Start = Rules.HeaderSize, List = Start - 0x20;
            if (List < 0 || H.Size() < Start) throw std::runtime_error("shorter than a header");
            const uint32_t StringsCount = H.U32(List);
            const uint64_t StringsPtr = H.U64(List + 8);
            const uint32_t AssetCount = H.U32(List + 0x10);
            const uint64_t AssetsPtr = H.U64(List + 0x18);
            int64_t Pos = Start;
            int64_t Block3 = 0;
            if (StringsPtr != 0)
            {
                const int64_t End = H.SkipStringSlots(Pos, (int64_t)StringsCount, Rules.Marker);
                Block3 = Align(Block3, 4) + (End - Pos);
                Pos = End;
            }
            ZoneTables::ZoneHead Z;
            if (AssetsPtr != 0)
            {
                if (Pos + 16 * (int64_t)AssetCount > H.Size()) throw std::runtime_error("asset list past the end");
                for (uint32_t K = 0; K < AssetCount; K++)
                {
                    const int64_t Entry = Pos + (int64_t)K * 16;
                    Z.Assets.push_back({ H.I32(Entry), H.U64(Entry + 8) });
                }
                Pos += (int64_t)AssetCount * 16;
                Block3 = Align(Block3, 4) + 16 * (int64_t)AssetCount;
            }
            Z.BodyStart = Pos;
            Z.Start = Block3;
            return Z;
        }
    };

    const Iw4ZoneFormat Format;
    const ZoneTables::ZoneFormatRegistration Registered(Format);
}
