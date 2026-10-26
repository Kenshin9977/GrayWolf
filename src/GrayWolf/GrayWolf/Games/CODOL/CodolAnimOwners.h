#pragma once

// Call of Duty Online's anim owners (IDiskGame::AnimOwners, AnimOwners.h): weapon:<w> from the weapon
// assets and their weaponattach overrides, on the view arms (mp/cocharitems_fja, _han: the first
// item's view model) and the view gun of mp/coweapmodels; character:<cha> for the player trees
// (multiplayer.atr, player_3rd_person.atr),
// on its marketplace parts (mp/cocharitemscha). The zone's documents first, then common_mp and the
// marketplace zones. Detail: graywolf-tools/docs/games/CODOL.md, "Anim owners".

#include "DiskGame.h"

#include <string>
#include <vector>

namespace CodolAnimOwners
{
    // the documents read (IDiskGame::AnimOwnerDocuments)
    std::vector<std::string> Documents();
    // a weapon asset's (0x101c) xanim name field: three blocks of 61 (both hands, the right, the left)
    bool AnimField(int64_t Field);
    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, AnimOwners::Book& Out);
}
