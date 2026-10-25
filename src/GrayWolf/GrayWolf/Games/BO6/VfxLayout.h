#pragma once

#include "Games/IW/Vfx.h"

#include <vector>

namespace BO6
{
    // Black Ops 6's particlesystem (0x80) and its 78 module types
    const Vfx::Layout& VfxLayout();

    // The particle system as Black Ops 6 and 7 share it: the levels, the emitter's fields and each
    // module's data by its name; Names: the game's module names by type id (its exe's table),
    // AssetSize its linked asset entry. Black Ops 7 builds its own from it.
    Vfx::Layout BlackOpsVfxLayout(const char* Game, const std::vector<const char*>& Names, int64_t AssetSize);
}
