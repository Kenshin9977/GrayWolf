#include "stdafx.h"
#include "ByteView.h"

#include "ZoneFormat.h"

// Modern Warfare 2019 (IW8): the zone starts with the
// 0x20-byte XAssetList {u32 stringCount, strings, u32 assetCount, assets}; the stream follows at
// 0x20: the script string slots and their text (inline for -2 and -1, 0x2ddc1b0), then the asset
// array of (u32 type, u32 pad, u64 header).
namespace
{
    class IW8ZoneFormat final : public ZoneTables::IZoneFormat
    {
    public:
        const char* Name() const override { return "iw8"; }

        ZoneTables::ZoneHead Read(ByteView Buf, const ZoneTables::StreamRules& Rules) const override
        {
            const ZoneTables::HeaderReader H(Buf);
            constexpr int64_t List = 0x20;
            if (H.Size() < List) throw std::runtime_error("shorter than an asset list");
            const uint32_t StringsCount = H.U32(0);
            const uint64_t StringsPtr = H.U64(8);
            const uint32_t AssetCount = H.U32(0x10);
            const uint64_t AssetsPtr = H.U64(0x18);
            // zone_load.strings_inline_bit (Black Ops 6's campaign, 61): the list's pointers and the
            // slots are inline when that bit is set (0x25b7850), not when they equal a marker
            const int64_t Bit = Rules.ZoneLoadValue("strings_inline_bit", -1);
            const auto Inline = [&](uint64_t V) {
                return Bit >= 0 ? V != 0 && ((V >> Bit) & 1) != 0 : V == Rules.Marker || V == Rules.Resident;
            };
            int64_t Pos = List;
            if (Inline(StringsPtr) && StringsCount > 0)
            {
                std::vector<uint64_t> Slots;
                for (uint32_t K = 0; K < StringsCount; K++) Slots.push_back(H.U64(Pos + (int64_t)K * 8));
                Pos += (int64_t)StringsCount * 8;
                for (uint64_t Slot : Slots)
                    if (Inline(Slot)) Pos = H.SkipString(Pos);
            }
            const int64_t StringsEnd = Pos;
            ZoneTables::ZoneHead Z;
            if (Inline(AssetsPtr) && AssetCount > 0)
            {
                for (uint32_t K = 0; K < AssetCount; K++)
                {
                    const int64_t Entry = Pos + (int64_t)K * 16;
                    Z.Assets.push_back({ (int)H.U32(Entry), H.U64(Entry + 8) });
                }
                Pos += (int64_t)AssetCount * 16;
            }
            if (Pos > H.Size()) throw std::runtime_error("asset list past the end");
            Z.BodyStart = Pos;
            // block 5's cursor when the first asset loads: the slots and their text from an
            // 8-aligned start, the array allocated aligned to 8
            const auto A8 = [](int64_t V) { return (V + 7) / 8 * 8; };
            Z.Start = A8(StringsEnd - List) + 16 * (int64_t)Z.Assets.size();
            return Z;
        }
    };

    const IW8ZoneFormat Format;
    const ZoneTables::ZoneFormatRegistration Registered(Format);
}
