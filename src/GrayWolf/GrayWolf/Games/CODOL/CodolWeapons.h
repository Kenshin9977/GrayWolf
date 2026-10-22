#pragma once

// Call of Duty Online's weapons and weaponstats kinds: its weapon asset is one 0x101c WeaponDef (MW2's
// WeaponDef and WeaponCompleteDef fields in one struct, moved and extended), read by the shared
// IwWeaponDefs layout (each field's proof in WeaponLayout's notes); its weaponattach assets (lists of
// field overrides) written as attachment files for WeaponFiles' Call of Duty Online rules.

#include "Games/IW/IwWeaponDefs.h"

#include <string>
#include <vector>

namespace Codol
{
    // The weapon fields read, where, and the README's notes (the exe's proofs)
    IwWeaponDefs::Layout WeaponLayout();
    // The zone's weaponattach assets as attachment files (ATTACHMENTFILE\codol.format\1\codol.overrides\
    // <type op 0x<offset> value;...>, the overrides of a number field)
    std::vector<WeaponFiles::Weapon> AttachmentFiles(const IwWeaponStats::ZoneView& V);
    // The string table naming each weapon's attachments (WeaponFiles reads it by this lower-case name)
    extern const char* const AttachmentTable;
}
