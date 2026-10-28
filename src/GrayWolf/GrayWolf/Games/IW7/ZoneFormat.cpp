#include "stdafx.h"
#include "ByteView.h"

#include "ZoneFormat.h"

// Infinite Warfare (IW7): the zone starts with the 0x28-byte
// XAssetList {u32 stringCount, strings, u32 assetCount, assets, globals}; the stream follows at
// 0x28: string slots and their text, XGlobals, then the asset array. XGlobals is walked with its
// reader table (profile zone_load.prefix_table), as the Python format does: nothing of its
// layout is written here.
namespace
{
    class IW7ZoneFormat final : public ZoneTables::IZoneFormat
    {
    public:
        const char* Name() const override { return "iw7"; }

        ZoneTables::ZoneHead Read(ByteView Buf, const ZoneTables::StreamRules& Rules,
                                  const ZoneTables::PrefixWalk& Walk) const override
        {
            const ZoneTables::HeaderReader H(Buf);
            if (H.Size() < 0x28) throw std::runtime_error("shorter than a header");
            const uint32_t StringsCount = H.U32(0);
            const uint64_t StringsPtr = H.U64(8);
            const uint32_t AssetCount = H.U32(0x10);
            const uint64_t AssetsPtr = H.U64(0x18), GlobalsPtr = H.U64(0x20);
            int64_t Pos = 0x28;
            if (StringsPtr == Rules.Marker && StringsCount > 0)
                Pos = H.SkipStringSlots(Pos, (int64_t)StringsCount, Rules.Marker);
            const int64_t StringsEnd = Pos;
            if (GlobalsPtr == Rules.Marker)
            {
                if (Rules.PrefixTable.empty()) throw std::runtime_error("profile has no zone_load.prefix_table");
                Pos = Walk(Rules.PrefixTable, Pos);
            }
            ZoneTables::ZoneHead Z;
            const int64_t ArrayStart = Pos;
            if (AssetsPtr == Rules.Marker && AssetCount > 0)
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
            // block 8's cursor when the first asset loads: the slots and their text, XGlobals
            // aligned to 8, the array allocated aligned to 8
            const auto A8 = [](int64_t V) { return (V + 7) / 8 * 8; };
            Z.Start = A8(A8(StringsEnd - 0x28) + (ArrayStart - StringsEnd)) + 16 * (int64_t)Z.Assets.size();
            return Z;
        }
    };

    const IW7ZoneFormat Format;
    const ZoneTables::ZoneFormatRegistration Registered(Format);
}
