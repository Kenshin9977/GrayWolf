#pragma once

#include "ByteView.h"

#include <cstdint>
#include <string>
#include <vector>

// The asset list of a decompressed Ghosts (IW6) zone: the script strings, then what the zone holds.

namespace Iw6
{
    constexpr uint32_t AssetTypeCount = 63;

    struct ListedAsset
    {
        uint32_t Type = 0;
        uint64_t Header = 0;
        // Defined in another zone: it is named here but carries no content, so extraction has to
        // skip it rather than count the same asset twice.
        bool IsReference = false;
    };

    struct AssetList
    {
        std::vector<std::string> Strings;
        std::vector<ListedAsset> Assets;
        uint64_t ArrayOffset = 0;
    };

    // The engine's own name for a type id, nullptr past the end of the enum.
    const char* AssetTypeName(uint32_t Type);

    bool ReadAssetList(ByteView Zone, AssetList& Out, std::string& Error);

    // Walking every asset's own bytes is the zonekit iw6 walker's job (ZoneTables::WalkZone,
    // "iw6" profile) - see Games/IW6/ZoneSource.cpp.
}
