#include "stdafx.h"
#include "ByteView.h"

#include "ZoneFormat.h"

// WWII (S2): the container holds the XFile header, so the zone
// starts with the 0x28-byte XAssetList {i32 stringCount, strings, i32 assetCount, assets,
// globals}; the stream follows at 0x28: string slots and their text, XGlobals (one XGfxGlobals
// pointer, and when set a 0x78-byte XGfxGlobals and six arrays), then the asset array. Every
// pointer here is tested != 0 by its reader; a string slot is read when it is the marker.
namespace
{
    class S2ZoneFormat final : public ZoneTables::IZoneFormat
    {
    public:
        const char* Name() const override { return "s2"; }

        ZoneTables::ZoneHead Read(ByteView Buf, const ZoneTables::StreamRules& Rules) const override
        {
            const ZoneTables::HeaderReader H(Buf);
            if (H.Size() < 0x28) throw std::runtime_error("shorter than a header");
            const int32_t StringsCount = H.I32(0);
            const uint64_t StringsPtr = H.U64(8);
            const int32_t AssetCount = H.I32(0x10);
            const uint64_t AssetsPtr = H.U64(0x18), GlobalsPtr = H.U64(0x20);
            int64_t Pos = 0x28;
            // block 0's cursor as the loader places these
            int64_t Cur = 0;
            auto A8 = [](int64_t V) { return (V + 7) & ~(int64_t)7; };
            auto A4 = [](int64_t V) { return (V + 3) & ~(int64_t)3; };
            if (StringsPtr != 0 && StringsCount > 0)
            {
                const int64_t End = H.SkipStringSlots(Pos, StringsCount, Rules.Marker);
                Cur = A8(Cur) + (End - Pos);
                Pos = End;
            }
            if (GlobalsPtr != 0)
            {
                const uint64_t Gfx = H.U64(Pos);
                Pos += 8;
                Cur = A8(Cur) + 8;
                if (Gfx != 0)
                {
                    // (pointer, count, element size) of the XGfxGlobals arrays in the file; the
                    // others are pushed to block 2, which reads nothing (0x4c6530)
                    static const int64_t Arrays[6][3] = {
                        { 0x08, 0x00, 8 }, { 0x10, 0x04, 12 }, { 0x38, 0x28, 4 },
                        { 0x40, 0x2C, 4 }, { 0x48, 0x30, 4 }, { 0x50, 0x34, 4 } };
                    const int64_t Base = Pos;
                    Pos = Base + 0x78;
                    Cur = A8(Cur) + 0x78;
                    for (const auto& A : Arrays)
                        if (H.U64(Base + A[0]) != 0)
                        {
                            Pos += (int64_t)H.U32(Base + A[1]) * A[2];
                            Cur = A4(Cur) + (int64_t)H.U32(Base + A[1]) * A[2];
                        }
                }
            }
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
            Z.Start = Z.Assets.empty() ? Cur : A8(Cur) + 16 * (int64_t)Z.Assets.size();
            return Z;
        }
    };

    const S2ZoneFormat Format;
    const ZoneTables::ZoneFormatRegistration Registered(Format);
}
