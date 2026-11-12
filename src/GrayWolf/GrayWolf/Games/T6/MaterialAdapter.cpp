#include "stdafx.h"

#include "MaterialDescription.h"

// Black Ops II materials: colour, normal and a pre-PBR specular map (MaterialDescription.h, LegacySpecularAdapter),
// its colour maps gamma 2.0.
namespace
{
    const Materials::AdapterRegistration Registered("T6", Materials::LegacySpecularAdapter(false, true));
}
