#include "stdafx.h"

#include "MaterialDescription.h"

// Black Ops II materials: colour, normal and a pre-PBR specular map (MaterialDescription.h, LegacySpecularAdapter),
// its alpha a gloss (the pixel shaders: specular power exp2(13 a), reflection mip 4 - 4 a, Lazarov's
// environment term of the gloss), its colour maps gamma 2.0 (the shaders square the colour and
// specular samples); the technique set's name says how the base layer draws.
namespace
{
    const Materials::AdapterRegistration Registered("T6", Materials::LegacySpecularAdapter(false, true));
}
