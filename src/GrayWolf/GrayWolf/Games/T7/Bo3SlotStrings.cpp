#include "stdafx.h"
#include "ByteView.h"

#include "Bo3SlotStrings.h"

#include <cstring>

std::vector<std::string> Bo3ZoneSlotStrings(ByteView Zone)
{
    // Same header walk as FastFile::ParseTreyarchZone, but keeping empty slots so the vector
    // index is the slot index.
    std::vector<std::string> Out;
    constexpr size_t PointerArrayOffset = 0x30;
    if (Zone.size() < PointerArrayOffset) return Out;
    uint32_t Count = 0;
    std::memcpy(&Count, Zone.data(), 4);
    size_t Pos = PointerArrayOffset + (size_t)Count * 8;
    if (Pos > Zone.size()) return Out;
    Out.resize(Count);
    for (uint32_t i = 0; i < Count && Pos < Zone.size(); i++)
    {
        uint64_t Slot = 0;
        std::memcpy(&Slot, Zone.data() + PointerArrayOffset + (size_t)i * 8, 8);
        if (Slot != 0xFFFFFFFFFFFFFFFFull) continue;
        const size_t Start = Pos;
        while (Pos < Zone.size() && Zone[Pos] != 0) Pos++;
        Out[i].assign((const char*)Zone.data() + Start, Pos - Start);
        Pos++;
    }
    return Out;
}
