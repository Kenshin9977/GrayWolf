#pragma once

// Vanguard's live readers (LiveGame.cpp) as its disk game reaches them: the material reader (a
// model's, a map's) and the lookup a name-only material header's images come from.

#include "GameReaders.h"

namespace S4Readers
{
    // the images the techsets_ zones' books give a material this zone names only (",name"):
    // set by the disk game while its zone is served
    extern thread_local GameReaders::ImagesElsewhereFn ImagesElsewhere;
    const XMaterial_t ReadXMaterial(uint64_t MaterialPointer);
}
