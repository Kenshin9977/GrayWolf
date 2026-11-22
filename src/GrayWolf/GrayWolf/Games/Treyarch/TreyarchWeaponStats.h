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
    // Where a game keeps a weapon's attachments and what the exe reads in them (offsets from the
    // asset; -1: not read). The game's weapon file says each one's proof (Rifling games/<game>.md).
    struct AttachmentLayout
    {
        int64_t List = -1;           // WeaponDef: the 64 attachment pointers (an id's 6-bit indices)
        int64_t Uniques = -1;        // WeaponDef: the 128 attachment unique pointers (unique i: attachment i's)
        // the attachment
        int64_t RangeScale = -1;     // float, the global range scale
        int64_t DamageScales = -1;   // float[6], each stop's damage scale
        int64_t RangeScales = -1;    // float[6], each stop's range scale
        int64_t FireTimeScale = -1;  // float
        int64_t ClipScale = -1;      // float
        int64_t ShotCount = -1;      // int, pellets (set when > 0)
        int64_t Locations = -1;      // float[21] by hit location, multiplied in (Cold War)
        bool SumOfDeviations = false;   // damage and range scales: 1 + sum(s - 1) (Cold War), else the product
        // the attachment unique
        int64_t UniqueLocations = -1;   // float[21]* by hit location, replacing the weapon's where it differs from unique 0's
        int64_t UniqueDamage = -1;      // int[6], replacing the weapon's table when [0] != 0
        int64_t UniqueRanges = -1;      // float[6], with it
        int64_t UniqueClip = -1;        // int, replacing the clip when > 0
        const char* Notes = "";         // the README's account of these offsets
    };

    // Where a game keeps what times a weapon's shots besides its fire time (offsets; -1: not read): the
    // fire delay, the bolt-action rechamber, the pause after a burst, the charge, the intro shots. The
    // game's weapon file says each one's proof (Rifling games/<game>.md, "Shot cycle").
    struct CycleLayout
    {
        int64_t Tunables = -1;              // WeaponDef: the tunables pointer
        // the tunables
        int64_t WeaponType = -1;            // int: 1 and 6 take the shot start's other path (grenades)
        int64_t FireDelay = -1;             // int ms, the weapon delay a shot starts with
        int64_t DelayInCycle = -1;          // u8: the fire delay added to the shot's weapon time
        int64_t BoltAction = -1;            // u8: the rechamber runs (gate of the rechamber start)
        int64_t ShotsBeforeRechamber = -1;  // int: 0 every shot, else every n shots
        int64_t NoRechamberEachShot = -1;   // u8: with 0 shots, the shot does not ask for the rechamber
        int64_t RechamberTime = -1;         // int ms, the rechamber state's time
        int64_t RechamberBoltTime = -1;     // int ms, its second timer (an event inside it)
        int64_t BurstDelay = -1;            // int ms, the weapon time after a burst's last fire time
        int64_t BurstDelayStep = -1;        // int ms taken off it per burst fired in a row
        int64_t BurstDelayMin = -1;         // int ms, the least it goes with that step
        int64_t IntroLength = -1;           // int: the first n shots of a trigger pull use the intro fire time
        int64_t IntroFireTime = -1;         // int ms
        int64_t ChargeHold = -1;            // int ms held before the first charge level
        int64_t ChargeLevels = -1;          // int: levels (shots) a full charge
        int64_t ChargeTime = -1;            // int ms for all the levels (one level: time / levels)
        // the attachment
        int64_t AttRechamberScale = -1;     // float, x rechamber time when > 0
        int64_t AttRechamberBoltScale = -1; // float, x rechamber bolt time when > 0
        int64_t AttBurstDelayScale = -1;    // float, x burst delay (every attachment)
        int64_t AttBurstDelayStep = -1;     // int, replaces the step when not 0
        int64_t AttBurstDelayMin = -1;      // int, replaces the minimum when not 0
        int64_t AttShotsBeforeRechamber = -1;  // int, replaces shotsBeforeRechamber when not 0
        int64_t AttIntroLengthScale = -1;   // float, x intro length (every attachment)
        int64_t AttIntroFireTimeScale = -1; // float, x intro fire time (every attachment)
        int64_t AttFireTypeSet = -1;        // u8: the attachment sets the fire type
        int64_t AttFireType = -1;           // int, that fire type
        int64_t AttBurstCount = -1;         // int, replaces the burst count (Burst, Auto Burst) when not 0
        const char* const* FireTypes = nullptr;   // the exe's fire type names, by value
        int FireTypeCount = 0;
        const char* Notes = "";             // the README's account of these offsets
    };

    // This zone's facts (its weapons through the gdt type "weapon", its localized strings, and with a
    // layout their attachments), then the table rebuilt once the run's zones are written
    // (Context::After). Title: the game, for the README.
    void Export(const ZonekitZone& Zone, const DiskGame::Context& C, DiskGame::Totals& T, const char* Title,
                const AttachmentLayout* Attachments = nullptr, const CycleLayout* Cycle = nullptr);
}
