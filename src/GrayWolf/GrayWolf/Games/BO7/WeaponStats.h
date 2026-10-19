#pragma once

// Black Ops 7's weapon stats (DiskGame::Kind::WeaponStats): one row per weapon and tunables set, in
// user units, only fields whose meaning the exe proves (graywolf-tools fields.md). The values sit in
// several zones (guns in global_stream_mp, their animation packages in ingame_shared_mp, the suits
// and game-mode settings in global_shared_mp, the names in ens_global), so each zone writes what it
// holds as facts (<kind>/facts/<zone>.json) and Merge joins every facts file of the folder into
// weapon_stats.csv, weapon_stats.json and README.txt.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ZoneTableWalker.h"

namespace BO7Stats
{
    // What a zone gives the facts reader
    struct ZoneView
    {
        const std::vector<uint8_t>* Image = nullptr;
        const ZoneTables::ZoneResult* Walk = nullptr;
        std::function<int64_t(uint64_t)> Off;              // a relocated pointer's zone offset, -1 elsewhere
        std::function<std::string(int64_t)> Text;          // the decrypted text of the string pointer field at a zone offset
        std::function<std::string(int64_t)> LocalizeText;  // a localize asset's decrypted text, by its position
    };
    struct FactsCount
    {
        size_t Weapons = 0, Tunables = 0, AnimPkgs = 0, Suits = 0, Settings = 0, Strings = 0, Attachments = 0;
    };
    // The zone's facts as JSON text; WantsWeapon filters the weapons by name (the rest is kept)
    std::string Facts(const ZoneView& V, const std::string& Zone, const std::function<bool(const std::string&)>& WantsWeapon, FactsCount& N);
    // Joins every <Dir>/facts/*.json into <Dir>/weapon_stats.csv, .json and README.txt
    bool Merge(const std::string& Dir, size_t& Rows, std::string& Error);
}
