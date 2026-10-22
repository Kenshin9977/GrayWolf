#include "stdafx.h"

#include "Games/Common/WeaponStatsTable.h"

#include "ExportNaming.h"
#include "ExportWriters.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace WeaponStatsTable
{
std::string Mode(const std::string& Name, const Json& Facts, std::initializer_list<const char*> Modes)
{
    const size_t Us = Name.rfind('_');
    const std::string Mode = Facts.contains("mode")    ? Facts["mode"].get<std::string>()
                             : Us == std::string::npos ? std::string()
                                                       : Name.substr(Us + 1);
    for (const char* M : Modes)
        if (Mode == M) return Mode;
    return std::string();
}

std::string UpgradeOf(const std::string& Name, const Weapons& All)
{
    const size_t Up = Name.find("_upgraded");
    if (Up == std::string::npos) return std::string();
    const std::string Base = Name.substr(0, Up) + Name.substr(Up + 9);
    return All.count(Base) ? Base : std::string();
}

std::string ZoneList(const std::vector<std::string>& Zones)
{
    std::string List;
    for (const auto& Z : Zones) List += (List.empty() ? "" : " ") + Z;
    return List;
}

std::string Line(const std::vector<std::string>& Cells)
{
    std::string Out;
    for (size_t I = 0; I < Cells.size(); I++) Out += (I ? "," : "") + ExportWriters::CsvField(Cells[I]);
    return Out + "\n";
}

void AddRow(const std::vector<std::string>& Head, const std::vector<std::string>& Cells, std::string& Csv, Json& Rows)
{
    Json O;
    for (size_t I = 0; I < Head.size(); I++)
    {
        Csv += (I ? "," : "") + ExportWriters::CsvField(Cells[I]);
        O[Head[I]] = Cells[I];
    }
    Csv += "\n";
    Rows.push_back(std::move(O));
}

void AddRow(const std::vector<std::string>& Head, const std::map<std::string, std::string>& Cells, std::string& Csv, Json& Rows)
{
    std::vector<std::string> Row;
    for (const auto& H : Head)
    {
        const auto It = Cells.find(H);
        Row.push_back(It == Cells.end() ? std::string() : It->second);
    }
    AddRow(Head, Row, Csv, Rows);
}

void WriteFacts(const std::string& Zone, const std::string& Text, const DiskGame::Context& C, DiskGame::Totals& T,
                std::function<bool(const std::string& Dir, size_t& Rows, std::string& Error)> Merge)
{
    using DiskGame::Kind;
    const fs::path Dir = C.KindDir(Kind::WeaponStats);
    if (!ExportWriters::WriteBytes(Dir / "facts" / (ExportNaming::SafeFileName(Zone) + ".json"), (const uint8_t*)Text.data(), Text.size()))
        T.Fail(Zone, Kind::WeaponStats, Zone, "write failed");
    C.After([Dir, &T, Merge = std::move(Merge)] {
        size_t Rows = 0;
        std::string Error;
        bool Ok = false;
        try
        {
            Ok = Merge(Dir.string(), Rows, Error);
        }
        catch (const std::exception& E)
        {
            Error = std::string("merge: ") + E.what();
        }
        if (!Ok) T.Fail("", Kind::WeaponStats, "weapon_stats", Error);
        T.Kinds[Kind::WeaponStats].Written = Rows;
    });
}
}
