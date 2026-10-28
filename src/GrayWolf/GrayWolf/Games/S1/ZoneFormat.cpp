#include "stdafx.h"
#include "ByteView.h"

#include "ZoneFormat.h"

// Advanced Warfare (S1): XFile 0x48, XAssetList at 0x48, the stream at
// 0x70: string slots and their text, XGlobals (one pointer to a 0x68-byte XGfxGlobals and five
// arrays), then the asset array. Every XGlobals pointer is tested != 0 by the game (0x25c500).
// Start: block 3's cursor once the list is read, replaying the loader's own alignments.
namespace
{
    int64_t Align(int64_t V, int64_t A) { return (V + A - 1) / A * A; }

    class S1ZoneFormat final : public ZoneTables::IZoneFormat
    {
    public:
        const char* Name() const override { return "s1"; }

        ZoneTables::ZoneHead Read(ByteView Buf, const ZoneTables::StreamRules& Rules) const override
        {
            const ZoneTables::HeaderReader H(Buf);
            if (H.Size() < 0x70) throw std::runtime_error("shorter than a header");
            const uint64_t StringsCount = H.U64(0x48), StringsPtr = H.U64(0x50);
            const uint64_t AssetCount = H.U64(0x58), AssetsPtr = H.U64(0x60), GlobalsPtr = H.U64(0x68);
            int64_t Pos = 0x70;
            int64_t Block3 = 0;
            if (StringsPtr != 0 && StringsCount != 0)
            {
                const int64_t End = H.SkipStringSlots(Pos, (int64_t)StringsCount, Rules.Marker);
                Block3 = Align(Block3, 8) + (End - Pos);
                Pos = End;
            }
            if (GlobalsPtr != 0)
            {
                const uint64_t Gfx = H.U64(Pos);
                Pos += 8;
                Block3 = Align(Block3, 4) + 8;
                if (Gfx != 0)
                {
                    const int64_t Base = Pos;
                    const int64_t At[5] = { 0x08, 0x10, 0x38, 0x40, 0x48 };
                    const uint32_t Counts[5] = { H.U32(Base), H.U32(Base + 4), H.U32(Base + 0x28), H.U32(Base + 0x2C), H.U32(Base + 0x30) };
                    const int64_t Width[5] = { 8, 12, 4, 4, 4 };
                    Pos = Base + 0x68;
                    Block3 = Align(Block3, 4) + 0x68;
                    for (int K = 0; K < 5; K++)
                    {
                        if (H.U64(Base + At[K]) == 0) continue;
                        Pos += (int64_t)Counts[K] * Width[K];
                        Block3 = Align(Block3, 4) + (int64_t)Counts[K] * Width[K];
                    }
                }
            }
            ZoneTables::ZoneHead Z;
            if (AssetsPtr != 0)
            {
                for (uint64_t K = 0; K < AssetCount; K++)
                {
                    const int64_t Entry = Pos + (int64_t)K * 16;
                    Z.Assets.push_back({ H.I32(Entry), H.U64(Entry + 8) });
                }
                Pos += (int64_t)AssetCount * 16;
                Block3 = Align(Block3, 4) + 16 * (int64_t)AssetCount;
            }
            if (Pos > H.Size()) throw std::runtime_error("asset list past the end");
            Z.BodyStart = Pos;
            Z.Start = Block3;
            return Z;
        }
    };

    const S1ZoneFormat Format;
    const ZoneTables::ZoneFormatRegistration Registered(Format);
}
