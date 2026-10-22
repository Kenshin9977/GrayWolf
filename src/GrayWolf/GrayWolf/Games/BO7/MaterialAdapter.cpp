#include "stdafx.h"

#include "MaterialDescription.h"
#include "Games/IW/IwTerrain.h"

// Black Ops 7 materials: a slot's semantic is a byte. The global slots are below 0x47 (TechsetFormat);
// the layers' slots from 0x47, 16 per layer (colour 0x47, 0x57, 0x67...: the layered materials of the
// M15's parts and viewhands). Slot 0 the colour (sRGB), slot 1 the "nog" as Black Ops 6's (gloss in r,
// normal y in g, occlusion in b, normal x in a: its fill-in for an empty slot is (0, 127, 255, 128)).
// Only the first layer is described: what the other layers' slots and 0x61 / 0x71 (greyscale masks on
// most weapon materials) do is not established. The terrain's material layers added (Games/IW/IwTerrain).
namespace
{
    constexpr uint32_t Color = 0x47, Nog = 0x48;

    class BlackOps7Materials final : public Materials::IAdapter
    {
    public:
        const char* Name() const override { return "bo7"; }
        Materials::Description Describe(const Materials::Source& M) const override
        {
            using Materials::Role;
            Materials::Description D = Materials::DescribeBySemantic(M, {
                { Color, Role::Albedo },
                { Nog, Role::Normal },
                { Nog, Role::Gloss, 0 },
                { Nog, Role::Occlusion, 2 },
            });
            if (!D.Normal.Empty()) D.NormalEnc = Materials::NormalEncoding::AG;
            // a material without these slots (no semantics, a map's): the names and the readers' hints
            if (D.Albedo.Empty() && D.Normal.Empty()) D = Materials::DefaultAdapter().Describe(M);
            IwTerrain::DescribeLayers(M, D);
            return D;
        }
    };
    const BlackOps7Materials Adapter;
    const Materials::AdapterRegistration Registered("BO7", Adapter);
}
