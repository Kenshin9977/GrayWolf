#pragma once

// The weapon file of the Infinity Ward engine's games up to Black Ops II (CoD1, CoD2 in their
// archives; CoD4 to Black Ops II as OpenAssetTools' dumpers write them): an InfoString,
// WEAPONFILE\<field>\<value>\..., the field names the game's weapon field table (those its weapon
// files are parsed by), so a column's meaning and unit come with its name. The weaponstats kind of
// those games: each zone writes the fields the table reads as facts (weaponstats/facts/<zone>.json),
// and the table is rebuilt from every facts file of the folder: weapon_stats.csv, weapon_stats.json,
// README.txt (as Games/Treyarch/TreyarchWeaponStats.h does), so the display names can come from
// another zone.

#include "DiskGame.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace WeaponFiles
{
    // Its fields, by name (a value never holds a backslash: the game's parser splits on it)
    std::map<std::string, std::string> Parse(const std::string& Text);
    // a value as a weapon or attachment file writes it: a backslash (the parser splits on it) or a
    // line break made a space
    std::string Clean(std::string Value);
    bool IsWeaponFile(const std::string& Text);
    // Black Ops II's attachment file (ATTACHMENTFILE\<field>\<value>...: OAT's attachment/<name>)
    bool IsAttachmentFile(const std::string& Text);

    struct Weapon
    {
        std::string Name;   // the asset's (ak47_mp, thompson); an attachment's (acog)
        std::string Text;   // its weapon file, or an attachment file
        std::string Mode;   // mp, sp... when the game says it apart from the name (CoD1's weapons/sp/)
    };

    // how a field's text becomes the column's number: Text, the text itself; ChargeShots, the shots a full
    // charge meter pays for, the field a cost per shot (Infinite Warfare's: a meter of 4095, the cost
    // (int)(cost x 4095), a shot fired while the meter holds the cost: 4095 / cost shots, rounded down);
    // BurstMs, a time in milliseconds (an f32 the game rounds: (int)floor(x + 0.5)) written only when the
    // weapon's fireType is Burst; BoltMs, an Ms time written only when the weapon's boltAction is 1;
    // PerShot, a count read as 1 when it is below 1 (Infinite Warfare's ammo used per shot, 0x737530)
    enum class Unit { Count, Ms, Inches, InchesPerSecond, Scale, Text, ChargeShots, BurstMs, BoltMs, PerShot };

    // a number column: the weapon file's field, its unit
    struct Column
    {
        const char* Name;
        const char* Field;
        Unit How;
    };

    // What a game's README says apart from the columns
    struct Notes
    {
        std::string Title;    // the game, the README's first line
        std::string Names;    // where the display names come from
        std::string Source;   // where the weapon files come from, and their units
        // the game's own columns, after the shared ones (before zones), and what the README says of them
        std::vector<Column> Extra = {};
        std::string ExtraNotes = {};
        // the weapon files name an animation package (AnimPackageField) whose times another zone may
        // hold: the zones' package files (ANIMPACKAGE\<field>\<value>..., by the package's name) fill
        // the times a weapon file has not
        bool AnimPackages = false;
        // the game's parser truncates a time's seconds x 1000 to its milliseconds (CoD1's, (int) of
        // atof x 1000); else they are rounded
        bool TruncateMs = false;
        // WWII's and Advanced Warfare's shot cycle columns (fire_delay_ms, bolt_action, rechamber_ms,
        // rechamber_bolt_ms, burst_delay_ms, ammo_per_shot, shot_interval_ms, shot_rpm) after Extra, from the
        // weapon file's fireDelay, boltAction, rechamberTime, rechamberBoltTime, burstFireCooldown and
        // ammoUsedPerShot; the game's ExtraNotes say them
        bool ShotCycle = false;
    };
    extern const char* const AnimPackageField;
    // Infinite Warfare's charged fire columns (its weapon_stats.csv extras and weapon_attachments.csv's)
    const std::vector<Column>& Iw7ChargeColumns();
    // Infinite Warfare's burst pause and bolt action columns (the same two tables)
    const std::vector<Column>& Iw7BurstBoltColumns();
    // Infinite Warfare's ammo per shot column (the same two tables)
    const std::vector<Column>& Iw7AmmoColumns();
    bool IsAnimPackageFile(const std::string& Text);

    // The weaponstats kind: this zone's weapons (those Context::Wants), attachment files and strings
    // (lower-cased key, UTF-8 text: the localized strings the display names are looked up in) as facts,
    // then the table rebuilt once the run's zones are written (Context::After). Attachments (Black Ops
    // II, MW3, WWII): weapon_attachments.csv beside the table, each weapon with each attachment its file
    // lists; MW3, WWII: weapon_attachment_combos.csv, the class builder's combinations, from the combination
    // table (Tables: string tables by lower-case name, "mp/attachmentcombos.csv", kept in the facts).
    void ExportStats(const std::vector<Weapon>& Weapons, const std::vector<std::pair<std::string, std::string>>& Strings,
                     const std::string& Zone, const DiskGame::Context& C, DiskGame::Totals& T, const Notes& N,
                     const std::map<std::string, std::string>& Tables = {});
}
