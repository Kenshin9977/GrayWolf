#include "stdafx.h"

#include "MaterialDescription.h"

// Advanced Warfare materials (MaterialDescription.h): the semantic hashes the live
// reader names (GameAdvancedWarfare::ReadXMaterial): colour, normal, and the specular map,
// rgb the specular colour and alpha the gloss; the occlusion map (6001F931, "occlusionMap": the
// images named "_occ", grey, mean 182/255 over mp_blackbox's 19, measured 2026-11-12) in its r, and
// the emissive map (34614347, the images named "_ems").
namespace
{
    std::vector<Materials::SemanticRole> Rows()
    {
        std::vector<Materials::SemanticRole> R = Materials::SpecularGlossTable();
        R.push_back({ 0x6001F931, Materials::Role::Occlusion, 0 });
        R.push_back({ 0x34614347, Materials::Role::Emissive });
        return R;
    }
    const Materials::SemanticAdapter Adapter("aw", Rows());
    const Materials::AdapterRegistration Registered("S1", Adapter);
}
