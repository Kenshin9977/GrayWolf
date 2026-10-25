#include "stdafx.h"

#include "MaterialDescription.h"
#include "CoDAssets.h"

// WWII materials (MaterialDescription.h): the semantics the live reader names. Unlike AW's, the
// specular map (34ECCCB3) is not a specular colour with gloss in alpha; measured over the 733
// sidecars of a WWII export (2026-10-25): alpha is 255 everywhere, r is 0.19 on non-metals (wood,
// brick, cloth, lens, lashes) and 0.65-0.86 on gun metal and gold, following the colour map (r with
// its luminance +0.6..+0.95, with its saturation +1.0 on gold: the colour map holds the metal's
// colour, a metal workflow), b the gloss (glass 0.59, gold 0.46, gun metal 0.3, cloth 0.15, brick
// 0), g near 1 (a cavity term, not the occlusion map: r +0.1..+0.3 with it). Read as metalness r,
// gloss b; the occlusion is its own map (6001F931, the images named "_occ").
namespace
{
    class S2Materials final : public Materials::IAdapter
    {
    public:
        const char* Name() const override { return "ww2"; }
        Materials::Description Describe(const Materials::Source& M) const override
        {
            using Materials::Role;
            return Materials::DescribeBySemantic(M, {
                { 0xA0AB1041, Role::Albedo },
                { 0x59D30D0F, Role::Normal },
                { 0x34ECCCB3, Role::Metalness, 0 },
                { 0x34ECCCB3, Role::Gloss, 2 },
                { 0x6001F931, Role::Occlusion, 0 },
            });
        }
    };
    const S2Materials Adapter;
    const Materials::AdapterRegistration Registered("S2", Adapter);
}
