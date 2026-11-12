#include "stdafx.h"

#include "MaterialDescription.h"
#include "Games/IW3/StoredNormals.h"

// World at War materials: colour, normal and a pre-PBR specular map (MaterialDescription.h, LegacySpecularAdapter),
// its alpha a gloss as in CoD4 (the pixel shaders: specular power exp2(9.38 a), reflection mip
// 6 - 8 a; unlike Black Ops'), its colour maps sRGB (lit as sampled, no square); the technique set's
// name says how the base layer draws. The normal maps as the image decoder leaves them
// (Games/IW3/StoredNormals.h).
namespace
{
    const StoredNormals::Adapter Adapter(Materials::LegacySpecularAdapter(false, false));
    const Materials::AdapterRegistration Registered("T4", Adapter);
}
