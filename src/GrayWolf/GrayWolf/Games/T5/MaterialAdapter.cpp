#include "stdafx.h"

#include "MaterialDescription.h"
#include "Games/IW3/StoredNormals.h"

// Black Ops materials: colour, normal and a pre-PBR specular map (MaterialDescription.h, LegacySpecularAdapter),
// its alpha a roughness (the pixel shaders: specular power exp2(13 - 13 a), reflection mip 8 a), its
// colour maps gamma 2.0; the technique set's name says how the base layer draws. The normal maps as
// the image decoder leaves them (Games/IW3/StoredNormals.h).
namespace
{
    const StoredNormals::Adapter Adapter(Materials::LegacySpecularAdapter(true, true));
    const Materials::AdapterRegistration Registered("T5", Adapter);
}
