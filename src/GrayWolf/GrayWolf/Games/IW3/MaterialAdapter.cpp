#include "stdafx.h"

#include "MaterialDescription.h"
#include "Games/IW3/StoredNormals.h"

// Call of Duty 4 materials: colour, normal and a pre-PBR specular map (MaterialDescription.h, LegacySpecularAdapter),
// its alpha a gloss (the pixel shaders: specular power exp2(9.38 a), reflection mip 6 - 8 a), its colour
// maps sRGB (the shaders light them as sampled, no square); the technique set's name says how the
// base layer draws. The normal maps as the image decoder leaves them (Games/IW3/StoredNormals.h).
namespace
{
    const StoredNormals::Adapter Adapter(Materials::LegacySpecularAdapter(false, false));
    const Materials::AdapterRegistration Registered("IW3", Adapter);
}
