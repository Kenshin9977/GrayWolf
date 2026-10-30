#include "stdafx.h"

#include "MaterialDescription.h"
#include "Games/Treyarch/TreyarchMaterials.h"

// Black Ops Cold War materials: Treyarch's semantics (Games/Treyarch/TreyarchMaterials.h); a material
// they do not match is read by its names.
namespace
{
    const Materials::SemanticAdapter Adapter("coldwar", TreyarchMaterials::Table(), Materials::SemanticAdapter::Unmatched::Names);
    const Materials::AdapterRegistration Registered("T9", Adapter);
}
