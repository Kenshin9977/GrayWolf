#pragma once

// The weaponstats kind of Black Ops 4 and Cold War (DiskGame::Kind::WeaponStats): one row per weapon
// asset, in user units, only the fields whose place and unit are established (README.txt says how).
// The values are read through the profile's gdt weapon layouts (Gdt::Type::Values: WeaponDef, the
// tunables and Cold War's style settings it points to), so nothing here names a game's offset. The
// guns and their display names sit in different zones (Black Ops 4: guns in core_ui, names in
// en_core_ui), so each zone writes what it holds as facts (<kind>/facts/<zone>.json) and the table is
// rebuilt from every facts file of the folder: weapon_stats.csv, weapon_stats.json, README.txt.

#include "DiskGame.h"

class ZonekitZone;

namespace Treyarch::WeaponStats
{
    // This zone's facts (its weapons through the gdt type "weapon", its localized strings), then the
    // table rebuilt once the run's zones are written (Context::After). Title: the game, for the README.
    void Export(const ZonekitZone& Zone, const DiskGame::Context& C, DiskGame::Totals& T, const char* Title);
}
