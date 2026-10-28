#include "stdafx.h"
#include "ByteView.h"

#include "ZoneFormat.h"

// Zones whose loaders take a load context (Black Ops 6 and Black Ops 7, header version 25 fast
// files): one parser for both titles' profiles (zone_format
// "iwctx"), its numbers from the profile's zone_load.
//
// The zone starts with the 0x30-byte XAssetList {u32 stringCount, strings* 8, u32 assetCount 0x10,
// unk* 0x18, u32 unkCount 0x20, assets* 0x28}; then the string slots (8 bytes each) and the text of
// each slot whose bit `strings_inline_bit` (61) is set, NUL-terminated, byte-packed (DB_LoadStringList,
// Black Ops 6 0x285da20); the unk list (unkCount elements of `unk_list.elem` bytes when its pointer is
// set; Black Ops 7 has none in the file); then the asset array, {u32 FNV-1a 32 of the type name, u32,
// u64 header} per asset, the type mapped by the profile's asset_type_map.
namespace
{
    class IwctxZoneFormat final : public ZoneTables::IZoneFormat
    {
    public:
        const char* Name() const override { return "iwctx"; }

        ZoneTables::ZoneHead Read(ByteView Buf, const ZoneTables::StreamRules& Rules) const override
        {
            const ZoneTables::HeaderReader H(Buf);
            const int64_t List = Rules.ZoneLoadValue("asset_list_size", 0x30);
            if (H.Size() < List) throw std::runtime_error("shorter than an asset list");
            const uint32_t StringsCount = H.U32(0);
            const uint64_t StringsPtr = H.U64(8);
            const uint32_t AssetCount = H.U32(0x10);
            const uint64_t AssetsPtr = H.U64(0x28);
            const int Bit = (int)Rules.ZoneLoadValue("strings_inline_bit", 61);
            int64_t Pos = List;
            if (StringsPtr != 0 && StringsCount > 0)
            {
                std::vector<uint64_t> Slots;
                for (uint32_t K = 0; K < StringsCount; K++) Slots.push_back(H.U64(Pos + (int64_t)K * 8));
                Pos += (int64_t)StringsCount * 8;
                for (const uint64_t Slot : Slots)
                    if (Slot != 0 && ((Slot >> Bit) & 1) != 0) Pos = H.SkipString(Pos);
            }
            const int64_t PtrAt = Rules.ZoneLoadValue("unk_list.ptr_at", -1);
            if (PtrAt >= 0 && H.U64(PtrAt) != 0)
                Pos += (int64_t)H.U32(Rules.ZoneLoadValue("unk_list.count_at", 0x20)) *
                       Rules.ZoneLoadValue("unk_list.elem", 4);
            ZoneTables::ZoneHead Z;
            if (AssetsPtr != 0)
            {
                for (uint32_t K = 0; K < AssetCount; K++)
                {
                    const int64_t Entry = Pos + (int64_t)K * 16;
                    const auto It = Rules.TypeByNameHash.find(H.U32(Entry));
                    Z.Assets.push_back({ It == Rules.TypeByNameHash.end() ? -1 : It->second, H.U64(Entry + 8) });
                }
                Pos += (int64_t)AssetCount * 16;
            }
            if (Pos > H.Size()) throw std::runtime_error("asset list past the end");
            Z.BodyStart = Pos;
            Z.Start = 0;   // the layout is not derived yet
            return Z;
        }
    };

    const IwctxZoneFormat Format;
    const ZoneTables::ZoneFormatRegistration Registered(Format);
}
