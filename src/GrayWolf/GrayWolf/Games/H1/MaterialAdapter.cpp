#include "stdafx.h"

#include "MaterialDescription.h"

// Modern Warfare Remastered materials (MaterialDescription.h): the semantics the live
// reader names (GameModernWarfareRM::ReadXMaterial): colour, normal (BC5, x,y), the specular map,
// rgb the specular colour and alpha the gloss, as Advanced Warfare's: its images are packed
// "<x>_spc-rgb&<x>_cos-l" (the specular in rgb, the cosine power map's luminance in alpha; alpha
// mean 73/255, spread 31 over mp_shipment's 38, measured 2026-11-12), and the occlusion map
// (6001F931, the images named "_occ", grey, mean 198/255) in its r.
namespace
{
    std::vector<Materials::SemanticRole> Table()
    {
        std::vector<Materials::SemanticRole> R = Materials::SpecularGlossTable();
        R.push_back({ 0x6001F931, Materials::Role::Occlusion, 0 });
        return R;
    }
    // the gloss's specular power, exp2(16 g), then GGX's alpha^2 = 2 / (power + 2) (the lit pixel
    // shaders, code_post_gfx_mp, as Advanced Warfare's): Black Ops III's gloss scale (log2 of the power), glossRange 0..16
    class RemasteredMaterials final : public Materials::SemanticAdapter
    {
    public:
        RemasteredMaterials() : SemanticAdapter("mwr", Table()) {}
    protected:
        void Refine(const Materials::Source&, Materials::Description& D, bool) const override
        {
            if (!D.Roughness.Empty() && D.Roughness.Invert) D.GlossRange[0] = 0.0f, D.GlossRange[1] = 16.0f;
        }
    };
    const RemasteredMaterials Adapter;
    const Materials::AdapterRegistration Registered("H1", Adapter);
}
