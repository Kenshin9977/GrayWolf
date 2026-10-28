#include "stdafx.h"
#include "ByteView.h"

#include "ZoneFormat.h"

// Black Ops III (T7): the zone starts with the 0x30-byte
// XAssetList {i32 stringCount, strings, i32 dependCount, depends, i32 assetCount, assets},
// raw-read by DB_LoadXFile; the stream follows at 0x30 under block 5: the script-string slots
// and their text, the depend slots and their text, then the asset array. Every list pointer is
// tested != 0; a string slot is read when it is the marker.
namespace
{
    class T7ZoneFormat final : public ZoneTables::IZoneFormat
    {
    public:
        const char* Name() const override { return "t7"; }

        ZoneTables::ZoneHead Read(ByteView Buf, const ZoneTables::StreamRules& Rules) const override
        {
            const ZoneTables::HeaderReader H(Buf);
            if (H.Size() < 0x30) throw std::runtime_error("shorter than a header");
            const int32_t StringsCount = H.I32(0), DependsCount = H.I32(0x10), AssetCount = H.I32(0x20);
            const uint64_t StringsPtr = H.U64(8), DependsPtr = H.U64(0x18), AssetsPtr = H.U64(0x28);
            int64_t Pos = 0x30;
            // block 5's cursor as the loader places these
            int64_t Cur = 0;
            auto Align = [](int64_t V, int64_t A) { return (V + A - 1) / A * A; };
            auto StringArray = [&](int32_t Count, int64_t Alignment)
            {
                const int64_t End = H.SkipStringSlots(Pos, Count, Rules.Marker);
                Cur = Align(Cur, Alignment) + (End - Pos);
                Pos = End;
            };
            if (StringsPtr != 0 && StringsCount > 0) StringArray(StringsCount, 8);
            if (DependsPtr != 0 && DependsCount > 0) StringArray(DependsCount, 4);
            ZoneTables::ZoneHead Z;
            if (AssetsPtr != 0 && AssetCount > 0)
            {
                for (int32_t K = 0; K < AssetCount; K++)
                {
                    const int64_t Entry = Pos + (int64_t)K * 16;
                    Z.Assets.push_back({ (int)H.U32(Entry), H.U64(Entry + 8) });
                }
                Pos += (int64_t)AssetCount * 16;
            }
            if (Pos > H.Size()) throw std::runtime_error("asset list past the end");
            Z.BodyStart = Pos;
            Z.Start = Z.Assets.empty() ? Cur : Align(Cur, 8) + 16 * (int64_t)Z.Assets.size();
            return Z;
        }
    };

    const T7ZoneFormat Format;
    const ZoneTables::ZoneFormatRegistration Registered(Format);
}
