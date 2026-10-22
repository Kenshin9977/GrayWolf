#pragma once

// What the weaponstats kinds share (Games/Common/WeaponFiles.h, Games/Treyarch/TreyarchWeaponStats.h,
// Games/IW/IwWeaponStats.h): each zone writes what it holds as facts (<kind>/facts/<zone>.json), and
// the table is rebuilt from every facts file of the folder once the run's zones are written, one row
// per weapon in weapon_stats.csv and weapon_stats.json.

#include "DiskGame.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace WeaponStatsTable
{
    using Json = nlohmann::ordered_json;
    // the facts' weapons by name: the facts kept, the zones holding it
    using Weapons = std::map<std::string, std::pair<Json, std::vector<std::string>>>;

    // the game mode: the one the facts give ("mode": CoD1's weapons/sp/), else the name's last part
    // (ak47_mp, ar_accurate_t8_zm), when Modes holds it; else empty
    std::string Mode(const std::string& Name, const Json& Facts, std::initializer_list<const char*> Modes);
    // Pack-a-Punch: a whole other weapon <base>_upgraded_<mode>, its base when All holds it
    std::string UpgradeOf(const std::string& Name, const Weapons& All);
    // the zones holding a weapon, space-separated
    std::string ZoneList(const std::vector<std::string>& Zones);

    // a CSV line: the cells (ExportWriters::CsvField), comma-separated
    std::string Line(const std::vector<std::string>& Cells);
    // a row, in Head's order, to the CSV and as an object of the JSON array: its cells by column (Cells
    // holds Head's count at least), or by column name, a column the row has not empty
    void AddRow(const std::vector<std::string>& Head, const std::vector<std::string>& Cells, std::string& Csv, Json& Rows);
    void AddRow(const std::vector<std::string>& Head, const std::map<std::string, std::string>& Cells, std::string& Csv, Json& Rows);

    // This zone's facts (Text) in <kind>/facts/<zone>.json, then the table rebuilt once the run's
    // zones are written (Context::After): Merge(the kind's folder, its rows, why it failed).
    void WriteFacts(const std::string& Zone, const std::string& Text, const DiskGame::Context& C, DiskGame::Totals& T,
                    std::function<bool(const std::string& Dir, size_t& Rows, std::string& Error)> Merge);
}
