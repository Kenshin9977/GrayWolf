#include "stdafx.h"

// Modern Warfare 2 and 3, the original 32-bit builds (zonekit profiles iw4x86, iw5x86): the XFile
// (MW2 0x28: size, external size, 8 block sizes; MW3 0x2c, 9), then the IW3-family list
// {stringCount, strings, assetCount, assets} and stream: when `strings != 0` count x 4 string slots
// and the text of every inline slot, then when `assets != 0` count x 8 {u32 type, u32 header}.
// Block 3 is current for the list: its cursor when the first asset loads is the slot array aligned
// 4 with its inline text, then the entries aligned 4 (iw4mp.exe's DB_LoadXFile tail, 0x4e073d;
// iw5mp.exe's, 0x4b85a1; graywolf-tools formats/iw4x86.py, iw5x86.py). Wolff's own IW3-family
// adapter has CoD4's name and Call of Duty Online's 16-byte entries: these are registered here,
// through Wolff's port (a caller may add its own format).

#include "ZoneFormat.h"

namespace
{
    class OriginalZoneFormat final : public wolff::ZoneTables::IZoneFormat
    {
    public:
        OriginalZoneFormat(const char* Name, int64_t XFileSize) : FormatName(Name), List(XFileSize) {}
        const char* Name() const override { return FormatName; }

        wolff::ZoneTables::ZoneHead Read(wolff::ByteView Buf, const wolff::ZoneTables::StreamRules& Rules) const override
        {
            constexpr int64_t ListSize = 0x10, Entry = 8;
            const wolff::ZoneTables::HeaderReader H(Buf);
            if (H.Size() < List + ListSize) throw std::runtime_error("shorter than a header");
            const uint32_t StringsCount = H.U32(List), StringsPtr = H.U32(List + 4);
            const uint32_t AssetCount = H.U32(List + 8), AssetsPtr = H.U32(List + 12);
            int64_t Pos = List + ListSize, Cursor = 0;
            if (StringsPtr != 0)
            {
                std::vector<uint32_t> Slots;
                for (uint32_t K = 0; K < StringsCount; K++) Slots.push_back(H.U32(Pos + 4 * (int64_t)K));
                Pos += 4 * (int64_t)StringsCount;
                Cursor = (Cursor + 3) / 4 * 4 + 4 * (int64_t)StringsCount;
                for (const uint32_t Slot : Slots)
                    if (Slot == (uint32_t)Rules.Marker)
                    {
                        const int64_t End = H.SkipString(Pos);
                        Cursor += End - Pos;
                        Pos = End;
                    }
            }
            wolff::ZoneTables::ZoneHead Z;
            if (AssetsPtr != 0)
            {
                if (Pos + Entry * (int64_t)AssetCount > H.Size()) throw std::runtime_error("asset list past the end");
                for (uint32_t K = 0; K < AssetCount; K++)
                    Z.Assets.push_back({ H.I32(Pos + Entry * (int64_t)K), H.U32(Pos + Entry * (int64_t)K + 4) });
                Pos += Entry * (int64_t)AssetCount;
                Cursor = (Cursor + 3) / 4 * 4 + Entry * (int64_t)AssetCount;
            }
            Z.BodyStart = Pos;
            Z.Start = Cursor;
            return Z;
        }

    private:
        const char* FormatName;
        int64_t List;   // the XFile's size: where the XAssetList starts
    };

    struct Registration
    {
        Registration()
        {
            static const OriginalZoneFormat Mw2("iw4x86", 0x28), Mw3("iw5x86", 0x2c);
            wolff::ZoneTables::RegisterZoneFormat(Mw2);
            wolff::ZoneTables::RegisterZoneFormat(Mw3);
        }
    };
    const Registration Registered;
}
