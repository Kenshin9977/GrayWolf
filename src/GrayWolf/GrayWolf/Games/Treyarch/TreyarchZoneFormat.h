#pragma once

#include "ByteView.h"

#include "ZoneFormat.h"

// The Treyarch zone header (Black Ops 4, Cold War): read outside the
// stream, i32 string count at 0, u64 strings at 8, then the asset count and the asset list pointer
// where the profile's "zone_header" says (StreamRules: T8 0x10/0x18 in 0x20 bytes, T9 0x18/0x20 in
// 0x28, the string ids between being a dead block); then the string slots and their inline text,
// then (type byte, header) pairs of 16 bytes. Block 4 when the first asset loads: the string table
// aligned to 8, then the asset list. Each title registers it under its own format name.
class TreyarchZoneFormat final : public ZoneTables::IZoneFormat
{
public:
    explicit TreyarchZoneFormat(const char* FormatName) : FormatName(FormatName) {}

    const char* Name() const override { return FormatName; }

    ZoneTables::ZoneHead Read(ByteView Buf, const ZoneTables::StreamRules& Rules) const override
    {
        const int64_t Header = Rules.HeaderSize;
        const ZoneTables::HeaderReader H(Buf);
        if (H.Size() < Header) throw std::runtime_error("shorter than a header");
        const int32_t StringsCount = H.I32(0);
        const uint64_t StringsPtr = H.U64(8);
        const int32_t AssetCount = H.I32(Rules.HeaderAssetCount);
        const uint64_t AssetsPtr = H.U64(Rules.HeaderAssets);
        int64_t Pos = Header;
        if (StringsPtr != 0) Pos = H.SkipStringSlots(Pos, StringsCount, Rules.Marker);
        const int64_t StringsEnd = StringsPtr != 0 ? Pos : 0;
        ZoneTables::ZoneHead Z;
        if (AssetsPtr != 0)
        {
            for (int32_t K = 0; K < AssetCount; K++)
            {
                const int64_t At = Pos + (int64_t)K * 16;
                if (At < 0 || At >= H.Size()) throw std::runtime_error("asset list past the end");
                Z.Assets.push_back({ H.U8(At), H.U64(At + 8) });
            }
            Pos += (int64_t)AssetCount * 16;
        }
        Z.BodyStart = Pos;
        const int64_t Strings = StringsEnd > Header ? StringsEnd - Header : 0;
        Z.Start = (Strings + 7) / 8 * 8 + 16 * (int64_t)Z.Assets.size();
        return Z;
    }

private:
    const char* FormatName;
};
