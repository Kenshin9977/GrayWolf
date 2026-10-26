#pragma once

// The anim owners of a Treyarch game of the GSC era (IDiskGame::AnimOwners, AnimOwners.h): each
// zone's documents read by GscAnimOwners, its own facts first, then those of the zones the game
// loads with it (the game says which; a <level>_patch zone's level first), each of those opened
// once per process.

#include "DiskGame.h"
#include "GscAnimOwners.h"

#include <filesystem>
#include <functional>
#include <vector>

namespace TreyarchGscOwners
{
    struct Setup
    {
        GscAnimOwners::Rules Rules;
        GscAnimOwners::Options Options;
        // the zones loaded with Zone, the one overriding the others first (those missing or Zone
        // itself are skipped)
        std::function<std::vector<std::filesystem::path>(const std::filesystem::path& Zone)> Shared;
    };

    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, AnimOwners::Book& Out, const Setup& S);

    // the first zone (sorted by name) of Dir whose stem fits Pattern ('*', no case), "" when none
    std::filesystem::path FirstZone(const std::filesystem::path& Dir, const char* Pattern);
}
