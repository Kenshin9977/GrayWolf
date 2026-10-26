#pragma once

// Black Ops III's anim owners (IDiskGame::AnimOwners), read from the zone's assets with the gdt
// kind's layouts and converters: aitype:<n> (its .ai_am tables' xanims on its first character's
// body, head, hat, gear), zbarrier:<z>/<n>, scene:<bundle>/<object>, weapon:<w> (gun model),
// character:<player body type> (its playeranim_* poses). The exported zone first; what it lacks from
// the zones loaded with it (<zone>_patch, <mode>_patch, <mode>_common, core_patch, core_common),
// their facts kept beside the install index.

#include "DiskGame.h"

#include <string>
#include <utility>
#include <vector>

class ZonekitZone;

namespace Bo3AnimOwners
{
    // the document's name patterns (IDiskGame::AnimOwnerDocuments)
    std::vector<std::string> Documents();
    // Z's facts as a document (IDiskZone::Documents): ("animowners/<zone>.json", JSON), text "" when none
    std::pair<std::string, std::string> ZoneFacts(const ZonekitZone& Z);
    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, AnimOwners::Book& Out);
}
