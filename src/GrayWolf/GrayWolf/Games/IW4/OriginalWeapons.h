#pragma once

// The weapons and weaponstats kinds of MW2's and MW3's original 32-bit builds: OpenAssetTools' x64
// build reads the rebuild's zones only, so these are read by the zonekit walk (IWZone::
// ExportsIwWeapons): the weapon asset is the WeaponCompleteDef, its WeaponDef behind a pointer, each
// field at the offset OAT's IW4/IW5_Assets.h give it built for x86 (the structs' sizes are the
// iw4x86 / iw5x86 tables'), written as the weapon file those fields make.

#include "Games/IW/IwWeaponDefs.h"

namespace Iw64
{
    const IwWeaponDefs::Layout& Mw2x86Weapons();
    const IwWeaponDefs::Layout& Mw3x86Weapons();
}
