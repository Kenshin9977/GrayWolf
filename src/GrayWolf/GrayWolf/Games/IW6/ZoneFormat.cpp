#include "stdafx.h"
#include "ByteView.h"

#include "ZoneFormat.h"

// Ghosts (IW6), zonefile.ZoneIW6 (Iw6AssetList.cpp): XFile 0x48, XAssetList at 0x48, the
// stream at 0x70: string slots and their text, XGlobals, then the asset array. Block 3 when the
// first asset loads: align4(array offset - 0x70) + 16 x assets.
namespace
{
    class Iw6ZoneFormat final : public ZoneTables::IZoneFormat
    {
    public:
        const char* Name() const override { return "iw6"; }

        ZoneTables::ZoneHead Read(ByteView Buf, const ZoneTables::StreamRules& Rules) const override
        {
            const ZoneTables::HeaderReader H(Buf);
            if (H.Size() < 0x70) throw std::runtime_error("shorter than a header");
            const uint64_t StringsCount = H.U64(0x48), StringsPtr = H.U64(0x50);
            const uint64_t AssetCount = H.U64(0x58), AssetsPtr = H.U64(0x60);
            int64_t Pos = 0x70;
            if (StringsPtr == Rules.Marker && StringsCount != 0) Pos = H.SkipStringSlots(Pos, (int64_t)StringsCount, Rules.Marker);
            // XGlobals: one pointer, then when inline a 104-byte XGfxGlobals and five arrays
            const uint64_t Gfx = H.U64(Pos);
            Pos += 8;
            if (Gfx == Rules.Marker)
            {
                const int64_t Base = Pos;
                const uint32_t Counts[5] = { H.U32(Base), H.U32(Base + 4), H.U32(Base + 40), H.U32(Base + 44), H.U32(Base + 48) };
                const int64_t At[5] = { 8, 16, 56, 64, 72 };
                const int64_t Width[5] = { 8, 4, 4, 4, 4 };
                Pos = Base + 104;
                for (int K = 0; K < 5; K++)
                    if (H.U64(Base + At[K]) == Rules.Marker) Pos += (int64_t)Counts[K] * Width[K];
            }
            ZoneTables::ZoneHead Z;
            const int64_t ListAt = Pos;
            if (AssetsPtr != 0)
            {
                for (uint64_t K = 0; K < AssetCount; K++)
                {
                    const int64_t Entry = Pos + (int64_t)K * 16;
                    Z.Assets.push_back({ H.I32(Entry), H.U64(Entry + 8) });
                }
                Pos += (int64_t)AssetCount * 16;
            }
            if (Pos > H.Size()) throw std::runtime_error("asset list past the end");
            Z.BodyStart = Pos;
            if (AssetsPtr != 0) Z.Start = (ListAt - 0x70 + 3) / 4 * 4 + 16 * (int64_t)Z.Assets.size();
            return Z;
        }
    };

    const Iw6ZoneFormat Format;
    const ZoneTables::ZoneFormatRegistration Registered(Format);
}
