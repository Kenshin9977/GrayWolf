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

    // how a field's text becomes the column's number
    enum class Unit { Count, Ms, Inches, InchesPerSecond, Scale };

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
    };
    extern const char* const AnimPackageField;
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
