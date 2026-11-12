#include "stdafx.h"

#include "MaterialDescription.h"

// Modern Warfare (2019) materials: a slot's semantic is a u32 (docs/games/IW8.md, the material
// books). 0 the colour ("_c&_s": the specular packed in its alpha, not an opacity), 9 the
// "_n&_g" / "_n&_o&_g" map packed as MWIII's "nog" (gloss r, normal y g, occlusion b, normal x a:
// "ag"), 0x12 the emissive ("_e"). Measured 2026-11-12 over mp_shipment's props and characters
// (493 materials) and mp_av_acog_00 (90): 0 and 9 in every one. The other slots (thermal 0x2d,
// masks 0x80, layers 1-2 / 10-11, 0x1b-0x1d, 0x44) left out. Without these: the names and hints.
namespace
{
    constexpr uint32_t Color = 0, Nog = 9, Emissive = 0x12;

    using Materials::Role;
    class ModernWarfare2019Materials final : public Materials::SemanticAdapter
    {
    public:
        ModernWarfare2019Materials()
            : SemanticAdapter("mw2019", {
                  { Color, Role::Albedo },
                  { Nog, Role::Normal },
                  { Nog, Role::Gloss, 0 },
                  { Nog, Role::Occlusion, 2 },
                  { Emissive, Role::Emissive },
              }, Unmatched::Names) {}
    protected:
        // the colour's semantic is 0, also the one an image without a semantic has: a material
        // whose images all have 0 is read by its names
        std::vector<Materials::SemanticRole> TableFor(const Materials::Source& M) const override
        {
            for (const auto& I : M.Images)
                if (I.Semantic != 0) return SemanticAdapter::TableFor(M);
            return {};
        }
        void Refine(const Materials::Source&, Materials::Description& D, bool BySemantic) const override
        {
            if (!BySemantic) return;
            if (!D.Normal.Empty()) D.NormalEnc = Materials::NormalEncoding::AG;
            // the colour's alpha is the packed specular (cloth 1-30/255, brass 190): never a cutout
            D.Alpha = Materials::AlphaMode::Opaque;
        }
    };
    const ModernWarfare2019Materials Adapter;
    const Materials::AdapterRegistration Registered("IW8", Adapter);
}
