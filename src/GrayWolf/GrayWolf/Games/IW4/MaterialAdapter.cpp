#include "stdafx.h"

#include "MaterialDescription.h"
#include "Games/IW3/StoredNormals.h"

// Modern Warfare 2 materials: colour, normal and a pre-PBR specular map (MaterialDescription.h, LegacySpecularAdapter),
// its alpha a gloss (the pixel shaders: specular power exp2(9.38 a), reflection mip 6 - 8 a), its colour
// maps gamma 2.0 (the shaders square the colour and specular samples, "fogColorLinear"); the technique
// set's name says how the base layer draws. The normal maps as the image decoder leaves them
// (Games/IW3/StoredNormals.h).
namespace
{
    const StoredNormals::Adapter Adapter(Materials::LegacySpecularAdapter(false, true));
    const Materials::AdapterRegistration Registered("IW4", Adapter);
    const Materials::AdapterRegistration Original("IW4x86", Adapter);   // the original 32-bit build: the same shaders
}
