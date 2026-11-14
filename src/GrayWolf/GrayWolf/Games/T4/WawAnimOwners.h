#pragma once

// World at War's anim owners (IDiskGame::AnimOwners, AnimOwners.h): its zones' GSC source and
// animtrees read by the Treyarch family's helper (Games/Treyarch/GscAnimOwners.h), the zone's own
// first, then the zones the game loads with it (<zone>_patch, patch, common; patch_mp, common_mp).

#include "DiskGame.h"
#include "Games/Treyarch/TreyarchGscOwners.h"

#include <string>
#include <vector>

namespace WawAnimOwners
{
    // the documents read (IDiskGame::AnimOwnerDocuments)
    std::vector<std::string> Documents();
    // its rules and the zones it loads with a zone (TreyarchGscOwners.h)
    TreyarchGscOwners::Setup Setup();
    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, AnimOwners::Book& Out);
}
