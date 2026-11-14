#include "stdafx.h"

#include "MaterialDescription.h"

// Advanced Warfare materials (MaterialDescription.h): the semantic hashes the live
// reader names (GameAdvancedWarfare::ReadXMaterial): colour, normal, and the specular map,
// rgb the specular colour and alpha the gloss; the occlusion map (6001F931, "occlusionMap": the
// images named "_occ", grey, mean 182/255 over mp_blackbox's 19, measured 2026-11-12) in its r, and
// the emissive map (34614347, the images named "_ems").
namespace
{
    std::vector<Materials::SemanticRole> Table()
    {
        std::vector<Materials::SemanticRole> R = Materials::SpecularGlossTable();
        R.push_back({ 0x6001F931, Materials::Role::Occlusion, 0 });
        R.push_back({ 0x34614347, Materials::Role::Emissive });
        return R;
    }
    // the gloss's specular power, exp2(16 g), then GGX's alpha^2 = 2 / (power + 2) (the lit pixel
    // shaders, mp_blackbox, 207 of 600): Black Ops III's gloss scale (log2 of the power), glossRange 0..16
    class AdvancedWarfareMaterials final : public Materials::SemanticAdapter
    {
    public:
        AdvancedWarfareMaterials() : SemanticAdapter("aw", Table()) {}
    protected:
        void Refine(const Materials::Source&, Materials::Description& D, bool) const override
        {
            if (!D.Roughness.Empty() && D.Roughness.Invert) D.GlossRange[0] = 0.0f, D.GlossRange[1] = 16.0f;
        }
    };
    const AdvancedWarfareMaterials Adapter;
    const Materials::AdapterRegistration Registered("S1", Adapter);
}
