#pragma once

#include "ByteView.h"

#include "ZoneFormat.h"

// The IW3-family zone layout: 32-bit, every
// pointer 4 bytes. XFile (size, external size, one size per block: 0x2c for CoD4's 9 blocks, 0x24
// for World at War's and Black Ops' 7, 0x28 for 8), then XAssetList {stringCount, strings,
// [dependCount, depends,] assetCount, assets}; the stream after it: when `strings != 0` count x 4
// string slots and the text of every inline slot, the same for `depends` (Black Ops II), then when
// `assets != 0` the array of asset entries (8 bytes {u32 type, u32 header}; Call of Duty Online's
// 16 bytes {u32 type, u32, u32 header, u32}). The list's block is current for it: its cursor when
// the first asset loads is each slot array aligned 4 with its inline text, then the entries.
namespace Iw3Family
{
    class ZoneFormat final : public ZoneTables::IZoneFormat
    {
    public:
        ZoneFormat(const char* Name, int64_t XFileSize, bool Depends = false, int64_t EntrySize = 8, int64_t EntryHeader = 4)
            : FormatName(Name), List(XFileSize), Depends(Depends), EntrySize(EntrySize), EntryHeader(EntryHeader)
        {
        }

        const char* Name() const override { return FormatName; }

        ZoneTables::ZoneHead Read(ByteView Buf, const ZoneTables::StreamRules& Rules) const override
        {
            const ZoneTables::HeaderReader H(Buf);
            const int64_t ListSize = Depends ? 0x18 : 0x10;
            if (H.Size() < List + ListSize) throw std::runtime_error("shorter than a header");
            const uint32_t StringsCount = H.U32(List), StringsPtr = H.U32(List + 4);
            const uint32_t AssetCount = H.U32(List + ListSize - 8), AssetsPtr = H.U32(List + ListSize - 4);
            int64_t Pos = List + ListSize, Cursor = 0;
            const auto Names = [&](uint32_t Count) {
                std::vector<uint32_t> Slots;
                for (uint32_t K = 0; K < Count; K++) Slots.push_back(H.U32(Pos + 4 * (int64_t)K));
                Pos += 4 * (int64_t)Count;
                Cursor = (Cursor + 3) / 4 * 4 + 4 * (int64_t)Count;
                for (uint32_t Slot : Slots)
                    if (Slot == (uint32_t)Rules.Marker)
                    {
                        const int64_t End = H.SkipString(Pos);
                        Cursor += End - Pos;
                        Pos = End;
                    }
            };
            if (StringsPtr != 0) Names(StringsCount);
            if (Depends && H.U32(List + 12) != 0) Names(H.U32(List + 8));
            ZoneTables::ZoneHead Z;
            if (AssetsPtr != 0)
            {
                for (uint32_t K = 0; K < AssetCount; K++)
                {
                    const int64_t Entry = Pos + EntrySize * (int64_t)K;
                    Z.Assets.push_back({ H.I32(Entry), H.U32(Entry + EntryHeader) });
                }
                Pos += EntrySize * (int64_t)AssetCount;
                Cursor = (Cursor + 3) / 4 * 4 + EntrySize * (int64_t)AssetCount;
            }
            if (Pos > H.Size()) throw std::runtime_error("asset list past the end");
            Z.BodyStart = Pos;
            Z.Start = Cursor;
            return Z;
        }

    private:
        const char* FormatName;
        int64_t List;                   // where the XAssetList starts: the XFile's size
        bool Depends;                   // a depends list between the strings and the assets
        int64_t EntrySize, EntryHeader; // an asset entry's size, and where its header is in it
    };
}
