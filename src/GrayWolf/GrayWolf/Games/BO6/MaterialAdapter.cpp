#include "stdafx.h"

#include "MaterialDescription.h"
#include "Games/IW/IwMaterialAlpha.h"
#include "Games/IW/IwTerrain.h"

// Black Ops 6 materials: a slot's semantic is a byte. Four layers of 14 slots from 82: colour (sRGB), "nog" (gloss in r, normal
// y in g, occlusion in b, normal x in a), emissive, opacity, reveal, tint mask; slot 10 of a layer
// after the first is its blend mask. 44/45 the proxy's colour and nog, 5 the thermal map, 10 the
// damage map (both left out). The alpha mode from the sort key (Games/IW/IwMaterialAlpha.h), never the
// colour map's alpha; a reader that does not give the sort key: cutout by the opacity map.
namespace
{
    constexpr uint32_t Base = 82, Stride = 14;
    enum Slot { Color = 0, Nog = 1, Emissive = 2, OpacityMap = 3, LayerMask = 10 };

    const Materials::Source::Image* At(const Materials::Source& M, uint32_t Layer, uint32_t S)
    {
        return M.BySemantic(Base + Layer * Stride + S);
    }

    class BlackOps6Materials final : public Materials::IAdapter
    {
    public:
        const char* Name() const override { return "bo6"; }
        Materials::Description Describe(const Materials::Source& M) const override
        {
            using Materials::Role;
            Materials::Description D = Materials::DescribeBySemantic(M, {
                { Base + Color, Role::Albedo },     { 44, Role::Albedo },
                { Base + Nog, Role::Normal },       { 45, Role::Normal },
                { Base + Nog, Role::Gloss, 0 },     { 45, Role::Gloss, 0 },
                { Base + Nog, Role::Occlusion, 2 }, { 45, Role::Occlusion, 2 },
                { Base + Emissive, Role::Emissive },
            });
            D.NormalEnc = Materials::NormalEncoding::AG;
            if (!IwMaterialAlpha::Apply(M, At(M, 0, OpacityMap), D))
                if (const auto* Op = At(M, 0, OpacityMap))
                {
                    D.AlphaMap.Image = { Op->Path, 0, false };
                    D.Alpha = Materials::AlphaMode::Cutout;
                }
            // the layers after the first: colour, gloss and the mask covering what is below
            for (uint32_t L = 1; L < 4; L++)
            {
                const auto* C = At(M, L, Color);
                const auto* N = At(M, L, Nog);
                const auto* Mask = At(M, L, LayerMask);
                if ((C == nullptr && N == nullptr) || Mask == nullptr) continue;   // no mask: not a blended layer
                if (D.Layers.empty())
                {
                    Materials::Layer First;
                    First.Albedo = D.Albedo;
                    First.Roughness = D.Roughness;
                    D.Layers.push_back(First);
                }
                Materials::Layer Layer;
                if (C) Layer.Albedo = { C->Path, 0, false };
                if (N) Layer.Roughness = { N->Path, 0, true };
                Layer.Mask = { Mask->Path, 0, false };
                D.Layers.push_back(Layer);
            }
            // a terrain's material layers (Games/IW/IwTerrain)
            IwTerrain::DescribeLayers(M, D);
            return D;
        }
    };
    const BlackOps6Materials Adapter;
    const Materials::AdapterRegistration Registered("BO6", Adapter);
    const Materials::AdapterRegistration RegisteredCampaign("BO6SP", Adapter);   // the campaign's materials: the same slots
}
