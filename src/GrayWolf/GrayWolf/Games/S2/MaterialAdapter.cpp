#include "stdafx.h"

#include "MaterialDescription.h"

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
    using Materials::Role;
    // The lit pixel shaders (aachen, 968 of 1331 with >= 3 samples, 2026-11-14) read the specular map
    // as .xzy: v = r^2, metal = saturate((v - 0.1) / 0.9), diffuse = colour * (1 - metal), F0 = metal *
    // colour + min(v, 0.1) (the description's Reflectance encoding); the gloss b as the specular power
    // exp2(18 g) (GGX alpha^2 = 2 / (power + 1)): Black Ops III's gloss scale stops at 17.
    class WW2Materials final : public Materials::SemanticAdapter
    {
    public:
        WW2Materials()
            : SemanticAdapter("ww2", {
                  { 0xA0AB1041, Role::Albedo },
                  { 0x59D30D0F, Role::Normal },
                  { 0x34ECCCB3, Role::Metalness, 0 },
                  { 0x34ECCCB3, Role::Gloss, 2 },
                  { 0x6001F931, Role::Occlusion, 0 },
              }) {}
    protected:
        void Refine(const Materials::Source&, Materials::Description& D, bool) const override
        {
            if (!D.Metalness.Empty()) D.MetalEnc = Materials::Description::MetalEncoding::Reflectance;
            if (!D.Roughness.Empty() && D.Roughness.Invert) D.GlossRange[0] = 0.0f, D.GlossRange[1] = 17.0f;
        }
    };
    const WW2Materials Adapter;
    const Materials::AdapterRegistration Registered("S2", Adapter);
}
