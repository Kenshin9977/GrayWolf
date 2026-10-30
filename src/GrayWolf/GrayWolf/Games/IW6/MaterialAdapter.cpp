#include "stdafx.h"

#include "MaterialDescription.h"

// Ghosts materials (MaterialDescription.h): colour (BC3; its alpha is often not a mask,
// so the cutout is only automatic when that alpha is two-valued), normal (BC5 signed, x,y), and
// the specular map, rgb the specular colour and alpha the gloss. EB529B4D is a detail map.
namespace
{
    const Materials::SemanticAdapter Adapter("ghosts", Materials::SpecularGlossTable());
    const Materials::AdapterRegistration Registered("IW6", Adapter);
}
