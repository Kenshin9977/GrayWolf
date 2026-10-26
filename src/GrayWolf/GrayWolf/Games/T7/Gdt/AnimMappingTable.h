#pragma once

// BO3 animmappingtable (76), as HydraX's AnimationMappingTable.cs (github.com/Scobalula/HydraX,
// GPL-3.0): header {name 0, maps 8, count 0x10}; a map (0x18) {alias script string 0, animations 8
// (script strings), count 0x10}. One reader for the gdt kind's .ai_am file (animmappingtable.cpp)
// and the anim owners (Bo3AnimOwners.cpp): an AI type's aliases and the xanims each plays.

#include "Gdt.h"

#include <string>
#include <utility>
#include <vector>

namespace T7Gdt
{
    // (alias, its animations) in the table's order
    using AnimMap = std::vector<std::pair<std::string, std::vector<std::string>>>;
    AnimMap ReadAnimMappingTable(const Gdt::Memory& M, int64_t Pos);
}
