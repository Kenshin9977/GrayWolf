#include "stdafx.h"

#include "TreyarchWeaponStats.h"

#include "ExportNaming.h"
#include "ExportWriters.h"
#include "Gdt.h"
#include "Games/Common/WeaponStatsTable.h"
#include "Games/Treyarch/TreyarchLevelNames.h"
#include "ZonekitZone.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace
{
    using Json = nlohmann::ordered_json;
    using ExportNaming::Lower;
    using ExportNaming::Num;

    constexpr double MetresPerInch = 0.0254;

    // the number columns, in table order: (column, layout field, element, scale to the column's unit)
    struct Column
    {
        const char* Name;
        const char* Field;
        int Element;
        double Scale;
    };
    const Column Numbers[] = {
        { "fire_time_ms", "firetime", 0, 1 },
        { "burst_count", "burstcount", 0, 1 },
        { "clip_size", "clipsize", 0, 1 },
        { "start_ammo", "startammo", 0, 1 },
        { "max_ammo", "maxammo", 0, 1 },
        { "ammo_count_clip_relative", "ammocountcliprelative", 0, 1 },
        { "reload_ms", "reloadtime", 0, 1 },
        { "first_raise_ms", "firstraisetime", 0, 1 },
        { "raise_ms", "raisetime", 0, 1 },
        { "raise_quick_ms", "quickraisetime", 0, 1 },
        { "damage_1", "damage", 0, 1 },
        { "range_1_m", "damagerange", 0, MetresPerInch },
        { "damage_2", "damage", 1, 1 },
        { "range_2_m", "damagerange", 1, MetresPerInch },
        { "damage_3", "damage", 2, 1 },
        { "range_3_m", "damagerange", 2, MetresPerInch },
        { "damage_4", "damage", 3, 1 },
        { "range_4_m", "damagerange", 3, MetresPerInch },
        { "damage_5", "damage", 4, 1 },
        { "range_5_m", "damagerange", 4, MetresPerInch },
        { "damage_6", "damage", 5, 1 },
        { "range_6_m", "damagerange", 5, MetresPerInch },
        { "shot_count", "shotcount", 0, 1 },
        { "multishot_damage_1", "multishotbasedamage", 0, 1 },
        { "multishot_range_1_m", "multishotbasedamagerange", 0, MetresPerInch },
        { "multishot_damage_2", "multishotbasedamage", 1, 1 },
        { "multishot_range_2_m", "multishotbasedamagerange", 1, MetresPerInch },
        { "multishot_damage_3", "multishotbasedamage", 2, 1 },
        { "multishot_range_3_m", "multishotbasedamagerange", 2, MetresPerInch },
        { "multishot_damage_4", "multishotbasedamage", 3, 1 },
        { "multishot_range_4_m", "multishotbasedamagerange", 3, MetresPerInch },
        { "multishot_damage_5", "multishotbasedamage", 4, 1 },
        { "multishot_range_5_m", "multishotbasedamagerange", 4, MetresPerInch },
        { "multishot_damage_6", "multishotbasedamage", 5, 1 },
        { "multishot_range_6_m", "multishotbasedamagerange", 5, MetresPerInch },
        { "melee_damage", "meleedamage", 0, 1 },
        { "explosion_radius_m", "explosionradius", 0, MetresPerInch },
        { "explosion_inner_damage", "explosioninnerdamage", 0, 1 },
        { "explosion_outer_damage", "explosionouterdamage", 0, 1 },
    };

    // a zone's facts: its weapons' values by column (those Wants names: the run's name filter), its
    // localized strings
    std::string Facts(const ZonekitZone& Z, const std::function<bool(const std::string&)>& Wants, size_t& Weapons, std::string& Error)
    {
        Json Out;
        Out["zone"] = Z.Name();
        Json List = Json::array();
        Z.ForEachGdtAsset("weapon", [&](const Gdt::Type& Ty, const Gdt::Memory& M, int64_t Pos) {
            Gdt::Stats S;
            const std::string Name = Ty.Name(M, Pos);
            if (Name.empty() || Name[0] == ',' || !Wants(Name)) return;
            std::map<std::string, Gdt::Type::FieldValue> Values;
            for (auto& [K, V] : Ty.Values(M, Pos, S)) Values[Lower(K)] = std::move(V);
            const auto Find = [&](const char* Field) -> const Gdt::Type::FieldValue* {
                const auto It = Values.find(Field);
                return It == Values.end() ? nullptr : &It->second;
            };
            Json W;
            W["name"] = Name;
            bool Holds = false;
            Json Cells = Json::object();
            for (const auto& Col : Numbers)
                if (const auto* V = Find(Col.Field); V != nullptr && (size_t)Col.Element < V->Numbers.size() && std::isfinite(V->Numbers[(size_t)Col.Element]))
                {
                    const double X = V->Numbers[(size_t)Col.Element];
                    Holds = Holds || X != 0;
                    Cells[Col.Name] = X * Col.Scale;
                }
            if (!Holds) return;   // a name-only header: another zone defines the weapon
            const auto Text = [&](const char* Field) {
                const auto* V = Find(Field);
                if (V == nullptr) return std::string();
                if (!V->Texts.empty()) return V->Texts[0];
                return V->Numbers.empty() ? std::string() : Num(V->Numbers[0]);
            };
            W["display_key"] = Text("szdisplayname");
            W["class"] = Text("weapclass");
            W["fire_type"] = Text("firetype");
            W["values"] = std::move(Cells);
            List.push_back(std::move(W));
        }, Error);
        Weapons = List.size();
        Out["weapons"] = std::move(List);
        std::vector<std::pair<std::string, std::string>> Strings;
        Z.Strings(Strings);
        // [key, text] pairs (an ordered object's insert is linear: 80 000 strings would take minutes)
        Json Texts = Json::array();
        for (const auto& [K, V] : Strings)
            if (!K.empty() && !V.empty()) Texts.push_back(Json::array({ Lower(K), V }));
        Out["strings"] = std::move(Texts);
        if (Weapons == 0 && Out["strings"].empty()) return std::string();
        return Out.dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
    }

    // every facts file of Dir joined into the table
    bool Merge(const std::string& Dir, const char* Title, size_t& Rows, std::string& Error)
    {
        std::vector<fs::path> Files;
        std::error_code Ec;
        for (const auto& E : fs::directory_iterator(fs::path(Dir) / "facts", Ec))
            if (E.path().extension() == ".json") Files.push_back(E.path());
        std::sort(Files.begin(), Files.end());
        std::map<std::string, std::string> Strings;
        WeaponStatsTable::Weapons Weapons;   // the first facts
        std::vector<std::string> Zones;
        for (const auto& F : Files)
        {
            Json J;
            try
            {
                std::ifstream In(F, std::ios::binary);
                J = Json::parse(In);
            }
            catch (const std::exception& E)
            {
                Error = F.filename().string() + ": " + E.what();
                return false;
            }
            const std::string Zone = J.value("zone", F.stem().string());
            Zones.push_back(Zone);
            for (const auto& P : J["strings"])
                if (P.is_array() && P.size() == 2) Strings.try_emplace(P[0].get<std::string>(), P[1].get<std::string>());
            for (const auto& W : J["weapons"])
            {
                auto& Slot = Weapons[W["name"].get<std::string>()];
                if (Slot.second.empty()) Slot.first = W;
                Slot.second.push_back(Zone);
            }
        }
        const auto Display = [&](const std::string& Key) -> std::string {
            if (Key.empty()) return std::string();
            // the display name field holds the localized string's key hash: a key no index names is
            // hash_<hex> here and localize_<hex> in the strings
            if (Key.rfind("hash_", 0) == 0)
            {
                const auto It = Strings.find("localize_" + Key.substr(5));
                return It != Strings.end() ? It->second : std::string();
            }
            if (const auto It = Strings.find(Lower(Key)); It != Strings.end()) return It->second;
            if (const auto It = Strings.find(Treyarch::HashedLocalizeKey(Key)); It != Strings.end()) return It->second;
            return std::string();
        };

        std::vector<std::string> Head = { "weapon", "display_name", "mode", "upgrade_of", "class", "fire_type", "rpm" };
        for (const auto& Col : Numbers) Head.push_back(Col.Name);
        Head.push_back("zones");
        std::string Csv = WeaponStatsTable::Line(Head);
        Json Out = Json::array();
        for (const auto& [Name, Entry] : Weapons)
        {
            const Json& W = Entry.first;
            const Json& V = W["values"];
            std::map<std::string, std::string> Cell;
            Cell["weapon"] = Name;
            Cell["display_name"] = Display(W.value("display_key", std::string()));
            Cell["mode"] = WeaponStatsTable::Mode(Name, W, { "mp", "zm", "wz", "cp" });
            Cell["upgrade_of"] = WeaponStatsTable::UpgradeOf(Name, Weapons);
            Cell["class"] = W.value("class", std::string());
            Cell["fire_type"] = W.value("fire_type", std::string());
            for (const auto& Col : Numbers)
                if (V.contains(Col.Name)) Cell[Col.Name] = Num(V[Col.Name].get<double>());
            if (V.contains("fire_time_ms") && V["fire_time_ms"].get<double>() > 0)
                Cell["rpm"] = Num(std::round(60000.0 / V["fire_time_ms"].get<double>() * 100) / 100);
            Cell["zones"] = WeaponStatsTable::ZoneList(Entry.second);
            WeaponStatsTable::AddRow(Head, Cell, Csv, Out);
        }
        std::string Readme = std::string(Title) + " weapon stats (GrayWolf --type weaponstats)\n\n"
            "One row per weapon asset (a game mode's variant, ar_accurate_t8_mp, _wz, _zm, is its own weapon).\n"
            "The raw assets, every field by offset, are the weapons kind (--type weapons).\n\n"
            "Zones read so far (facts): ";
        for (size_t I = 0; I < Zones.size(); I++) Readme += (I ? ", " : "") + Zones[I];
        Readme +=
            "\nThe display names are the localized strings of another zone: export it into the same --out\n"
            "(Black Ops 4: the guns are in core_ui, their names in en_core_ui; Cold War: the guns in core_common,\n"
            "zombies' and multiplayer's own variants in zm_common and mp_common, the names in <lang>_core_common;\n"
            "a .txt list of zones works).\n\n"
            "Where the values come from\n"
            "The fields are read by name through the profile's gdt weapon layouts (zonekit/profiles, the\n"
            "WeaponDef, WeaponTunables and WeaponStyleSettings field files the gdt kind ports to Black Ops III).\n"
            "Those names were placed by matching the values of the weapons both games share with Black Ops III,\n"
            "whose units the BO3 executable proves (Rifling games/bo3.md): an equal value on the same weapon\n"
            "carries the unit over. Status 'data', not read in this game's code:\n"
            "- clip_size, start_ammo, max_ammo, burst_count, melee_damage, explosion radius and damages: Black\n"
            "  Ops 4's offsets are also those acts' own WeaponTunables struct declares (iClipSize 0xcc0,\n"
            "  iStartAmmo 0xe40, iMaxAmmo 0xd4c, burstcount 0xb90, meleedamage 0xd50, explosion 0x230-0x248).\n"
            "- Checked against published Black Ops 4 numbers: KN-57 damage 40 at about 550 rpm (108 ms here),\n"
            "  Maddox RFB damage 32, Titan magazine 75.\n\n"
            "Columns and units\n"
            "- class, fire_type: the layouts' enum names (weapclass, firetype; Black Ops III's lists).\n"
            "- fire_time_ms: firetime, milliseconds between two shots; rpm = 60000 / fire_time_ms.\n"
            "- burst_count: rounds a burst. clip_size: rounds a magazine. start_ammo, max_ammo: as stored, in\n"
            "  magazines when ammo_count_clip_relative is 1, else rounds (Black Ops III's rule, its Mod Tools' help).\n"
            "- ammo_count_clip_relative: ammoCountClipRelative, Black Ops 4 only (WeaponTunables 0x10bf, the byte\n"
            "  acts' struct declares under that name; 1 on 181 of the 185 guns whose max_ammo is a magazine count,\n"
            "  ar_accurate_t8_zm 9 x 35, and 0 on ar_standard_t8_zm's 490 rounds; the script field table's maxAmmo\n"
            "  and startAmmo entries multiply 0xd4c / 0xe40 by the clip size when it is set). Cold War: no place\n"
            "  known (empty).\n"
            "- reload_ms: reloadtime, the reload with rounds left, milliseconds.\n"
            "- first_raise_ms, raise_ms, raise_quick_ms: the weapon's first raise (taken out the first time), its\n"
            "  plain raise and its quick raise, milliseconds, before attachments and perks. See Raise times.\n"
            "- damage_1..6 at range_1..6_m: the six stops of the damage table (damage, damageRange; inches x\n"
            "  0.0254). damage_1 up to range_1_m, damage_6 beyond range_6_m, linear between two stops; a stop\n"
            "  whose range is 0 is skipped (the game's own interpolation, below). For a shotgun it is what ONE\n"
            "  pellet does, see Shotguns.\n"
            "- shot_count: shotCount, pellets a shot. multishot_damage_1..6 at multishot_range_1..6_m:\n"
            "  multishotBaseDamage and multishotBaseDamageRange, a second damage table of the same form (inches\n"
            "  x 0.0254), added once per target a shot hits, see Shotguns.\n"
            "- melee_damage. explosion_radius_m (inches x 0.0254), explosion_inner/outer_damage (centre, edge).\n"
            "- upgrade_of: a Pack-a-Punch weapon (<base>_upgraded_<mode>) names its base when the table holds it.\n\n"
            "Shotguns: what the damage columns mean (read in the game's executable, the decrypted dumps of\n"
            "Black Ops 4 and Cold War; the two games' code is the same, offsets aside)\n"
            "- shot_count is the game's own field: the script field table's entry shotCount (the one a script's\n"
            "  weapon.shotcount reads) calls a getter returning the tunables' 0xfc8 (Black Ops 4) or the style\n"
            "  settings' 0xe0 (Cold War), unless an equipped attachment sets its own count (Extra pellets).\n"
            "- The fire code traces shot_count pellets only for weapon class spread (and class 13); any other\n"
            "  class fires one bullet whatever shot_count says.\n"
            "- Each pellet that hits does damage_1..6: the bullet damage function reads the damage table\n"
            "  (tunables 0xbec / 0x680 in Black Ops 4, style settings 0x40 / 0x58 in Cold War) at the pellet's\n"
            "  distance. So damage_1..6 are per pellet, not per shot.\n"
            "- When shot_count is above 1, once the pellets are traced the game keeps one hit per target (later\n"
            "  hits on the same entity lose the bonus) and adds to it multishot_damage at that distance (a\n"
            "  function reading 0xf60 / 0x99c in Black Ops 4's tunables, 0x78 / 0x90 in Cold War's style\n"
            "  settings, through the same interpolation as the bullet damage), times the same per-shot scale the\n"
            "  pellet damage is multiplied by (1 normally).\n"
            "- So one shot on one target at distance d: pellets_hit x damage(d) + multishot_damage(d).\n"
            "  Checked against published totals: Black Ops 4 MOG 12 (shotgun_pump_t8_mp) 8 x 2 + 135 = 151,\n"
            "  the figure guides give for it (one kill at 150 health); Cold War Hauer 77 (shotgun_pump_t9)\n"
            "  8 x 4 + 127 = 159, the damage guides list.\n"
            "- Interpolation (both tables, both games): up to the first stop's range its damage, beyond the last\n"
            "  stop's range its damage, between two stops linear, stops whose range is below 0.001 skipped;\n"
            "  ranges are scaled by the attachments' range scales and damages by their damage scales.\n"
            "- AI (non-player) targets take another path in both functions, not described by these columns.\n\n"
            "Raise times (read in the same decrypted executables)\n"
            "- first_raise_ms is the game's own field: the script field table's entry firstRaiseTime (what a\n"
            "  script's weapon.firstraisetime reads) calls a getter returning the tunables' 0xd20 (Black Ops 4)\n"
            "  or 0xf54 (Cold War) unless an attachment sets its own. The same check on the table's fireTime,\n"
            "  reloadTime, meleeTime, clipSize, maxAmmo and startAmmo entries lands on the offsets these columns\n"
            "  already read (and acts' struct names), so the reading of the table is right.\n"
            "- The function that starts a raise picks one time by case: the first raise calls that getter; a\n"
            "  quick raise (switching between certain weapons) reads 0xfb0 (Cold War 0x1188); an empty magazine\n"
            "  0xd04 (0xf38); a special case 0x1004 (0x11e8); every other raise 0xda0 (0xfb8). raise_ms is that\n"
            "  last one, raise_quick_ms the quick one. Cold War's function is the same code, offsets aside.\n"
            "- Checked against Black Ops III's values for the weapons both games name alike (40 shared weapons):\n"
            "  firstRaiseTime equals 0xd20 on 16 (8 distinct values, next offset 7), quickRaiseTime 0xfb0 on 34\n"
            "  (next 20), raiseTime 0xda0 on 22 (the empty and special raise offsets 21 each). The same test\n"
            "  puts fireTime, clipSize, reloadTime and meleeTime first on their proven offsets.\n\n"
            "Not here (no proven place yet): ADS times, drop times, empty-reload times, hit location multipliers\n"
            "(in the weapons kind as locationDamageMultipliers), attachments.\n"
            "- Drop: the function that starts a drop reads 0xcf0 in the plain case (0xfac quick, 0xd00 empty,\n"
            "  0x101c in a special case), but Black Ops III's dropTime lands on 0x101c (32 weapons) - its values\n"
            "  equal Black Ops III's swimDropTime on all 37 weapons - and 0xcf0 ties with other offsets: the\n"
            "  value test does not confirm it, so no column.\n"
            "- ADS: the executable reads no tunables offset where the layouts guess adsTransInTime/Out (0xc9c,\n"
            "  0xca0), and the game names no ADS time anywhere (script field table, strings, zones).\n";
        std::error_code E2;
        fs::create_directories(Dir, E2);
        const std::string JsonText = Out.dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
        const bool Ok = ExportWriters::WriteBytes(fs::path(Dir) / "weapon_stats.csv", (const uint8_t*)Csv.data(), Csv.size()) &&
                        ExportWriters::WriteBytes(fs::path(Dir) / "weapon_stats.json", (const uint8_t*)JsonText.data(), JsonText.size()) &&
                        ExportWriters::WriteBytes(fs::path(Dir) / "README.txt", (const uint8_t*)Readme.data(), Readme.size());
        if (!Ok) Error = "write failed in " + Dir;
        Rows = Weapons.size();
        return Ok;
    }
}

void Treyarch::WeaponStats::Export(const ZonekitZone& Zone, const DiskGame::Context& C, DiskGame::Totals& T, const char* Title)
{
    using DiskGame::Kind;
    auto& Kt = T.Kinds[Kind::WeaponStats];
    size_t Weapons = 0;
    std::string Error;
    const std::string Facts = ::Facts(Zone, [&C](const std::string& Name) { return C.Wants(Kind::WeaponStats, Name); }, Weapons, Error);
    Kt.Seen += Weapons;
    if (!Error.empty()) T.Fail(Zone.Name(), Kind::WeaponStats, "", Error);
    if (Facts.empty()) return;
    WeaponStatsTable::WriteFacts(Zone.Name(), Facts, C, T, [Title](const std::string& Dir, size_t& Rows, std::string& Error) {
        return Merge(Dir, Title, Rows, Error);
    });
}
