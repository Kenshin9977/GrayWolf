#pragma once

// The alpha mode of a Black Ops 6 / Black Ops 7 material, from its sort key (BO6 u8 0x19, BO7 u8
// 0x67) and its opacity map. Their colour map's alpha is not an opacity: on the M15's parts it
// marks the worn metal (mean 39/255, 88% of the texels under 128 on the receiver), and the depth
// and shadow passes of the weapons' techset sample no texture at all. The sort keys measured
// (BO7 global + mp_sat_homestead, BO6 global_stream_mp; the byte before them 0 / 1 / 2 with them):
//   2            opaque: every lit material with depth and shadow techniques, foliage and hair
//                among them (their cutout is the opacity map, BO6's slot 85, BO7's 0x4a)
//   3 .. 0x11    decals
//   0x12 .. 0x22 translucent: glass, lenses, refraction plastic, the HUD's icons
//   0x23 ..      effects (particles, distortion)

#include "MaterialDescription.h"

namespace IwMaterialAlpha
{
    // false when the sort key is unknown (D left as it is)
    inline bool Apply(const Materials::Source& M, const Materials::Source::Image* OpacityMap, Materials::Description& D)
    {
        if (M.SortKey < 0) return false;
        if (OpacityMap) D.AlphaMap.Image = { OpacityMap->Path, 0, false };
        if (M.SortKey > 2) D.Alpha = Materials::AlphaMode::Blend;
        else D.Alpha = OpacityMap ? Materials::AlphaMode::Cutout : Materials::AlphaMode::Opaque;
        return true;
    }
}
