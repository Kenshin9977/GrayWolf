#pragma once

#include "ByteView.h"

#include <cstdint>
#include <string>

#include <vector>

// The zone's script strings indexed by slot (empty for slots with no inline string). Bone and
// notetrack names are slot indices.
std::vector<std::string> Bo3ZoneSlotStrings(ByteView Zone);

