#include "stdafx.h"

#include "MaterialDescription.h"

// Advanced Warfare materials (MaterialDescription.h): the semantic hashes the live
// reader names (GameAdvancedWarfare::ReadXMaterial): colour, normal, and the specular map,
// rgb the specular colour and alpha the gloss.
namespace
{
    const Materials::SemanticAdapter Adapter("aw", Materials::SpecularGlossTable());
    const Materials::AdapterRegistration Registered("S1", Adapter);
}
