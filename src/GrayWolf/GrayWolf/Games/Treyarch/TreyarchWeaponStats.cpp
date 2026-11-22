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

    // the number columns, in table order: (column, layout field, element, scale to the column's unit,
    // the field's name in the other game's layouts when it differs)
    struct Column
    {
        const char* Name;
        const char* Field;
        int Element;
        double Scale;
        const char* Alt = nullptr;
    };
    const Column Numbers[] = {
        { "fire_time_ms", "firetime", 0, 1 },
        { "burst_count", "burstcount", 0, 1 },
        { "clip_size", "clipsize", 0, 1 },
        { "start_ammo", "startammo", 0, 1 },
        { "max_ammo", "maxammo", 0, 1 },
        { "ammo_count_clip_relative", "ammocountcliprelative", 0, 1, "ammocountedinclips" },
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
        // the hit location multipliers: locationDamageMultipliers by the exe's hit location index
        // (0, none, left out)
        { "hit_helmet", "locationdamagemultipliers", 1, 1 },
        { "hit_head", "locationdamagemultipliers", 2, 1 },
        { "hit_neck", "locationdamagemultipliers", 3, 1 },
        { "hit_torso_upper", "locationdamagemultipliers", 4, 1 },
        { "hit_torso_mid", "locationdamagemultipliers", 5, 1 },
        { "hit_torso_lower", "locationdamagemultipliers", 6, 1 },
        { "hit_right_arm_upper", "locationdamagemultipliers", 7, 1 },
        { "hit_left_arm_upper", "locationdamagemultipliers", 8, 1 },
        { "hit_right_arm_lower", "locationdamagemultipliers", 9, 1 },
        { "hit_left_arm_lower", "locationdamagemultipliers", 10, 1 },
        { "hit_right_hand", "locationdamagemultipliers", 11, 1 },
        { "hit_left_hand", "locationdamagemultipliers", 12, 1 },
        { "hit_right_leg_upper", "locationdamagemultipliers", 13, 1 },
        { "hit_left_leg_upper", "locationdamagemultipliers", 14, 1 },
        { "hit_right_leg_lower", "locationdamagemultipliers", 15, 1 },
        { "hit_left_leg_lower", "locationdamagemultipliers", 16, 1 },
        { "hit_right_foot", "locationdamagemultipliers", 17, 1 },
        { "hit_left_foot", "locationdamagemultipliers", 18, 1 },
        { "hit_gun", "locationdamagemultipliers", 19, 1 },
        { "hit_riotshield", "locationdamagemultipliers", 20, 1 },
        // Cold War's zombies multipliers (its tunables 0xd2c-0xd40)
        { "zm_rarity_green", "zmraritygreendamagescale", 0, 1 },
        { "zm_rarity_blue", "zmraritybluedamagescale", 0, 1 },
        { "zm_rarity_purple", "zmraritypurpledamagescale", 0, 1 },
        { "zm_rarity_orange", "zmrarityorangedamagescale", 0, 1 },
        { "zm_pap2", "zmpap2damagescale", 0, 1 },
        { "zm_pap3", "zmpap3damagescale", 0, 1 },
    };

    using AttachmentLayout = Treyarch::WeaponStats::AttachmentLayout;

    // the hit location names, by the exe's index (entry 0, none, first): the hit_ columns' suffixes
    const char* const Locations[] = { "none", "helmet", "head", "neck", "torso_upper", "torso_mid", "torso_lower",
                                      "right_arm_upper", "left_arm_upper", "right_arm_lower", "left_arm_lower", "right_hand",
                                      "left_hand", "right_leg_upper", "left_leg_upper", "right_leg_lower", "left_leg_lower",
                                      "right_foot", "left_foot", "gun", "riotshield" };

    // an asset's name: its hash at 0 (the index's name, else <kind>_<hex>)
    std::string HashedName(const Gdt::Memory& M, int64_t Pos, const char* Kind)
    {
        const uint64_t H = M.At<uint64_t>(Pos) & M.HashMask;
        if (H == 0) return std::string();
        if (std::string N = M.HashName(H); !N.empty()) return N;
        return std::format("{}_{:x}", Kind, H);
    }

    using CycleLayout = Treyarch::WeaponStats::CycleLayout;

    // a fire type's name by the exe's table, else its number
    std::string FireTypeName(const CycleLayout& Cy, int Value)
    {
        if (Value >= 0 && Value < Cy.FireTypeCount && Cy.FireTypes != nullptr) return Cy.FireTypes[Value];
        return std::to_string(Value);
    }

    // a weapon's shot cycle fields, from its tunables by the layout
    Json CycleValues(const Gdt::Memory& M, int64_t Pos, const CycleLayout& Cy)
    {
        Json V = Json::object();
        const int64_t T = Cy.Tunables >= 0 ? M.Follow(Pos + Cy.Tunables) : -1;
        if (T < 0) return V;
        const auto I = [&](const char* Col, int64_t Off) {
            if (Off >= 0) V[Col] = M.At<int32_t>(T + Off);
        };
        const auto B = [&](const char* Col, int64_t Off) {
            if (Off >= 0) V[Col] = (int)M.At<uint8_t>(T + Off);
        };
        I("weapon_type", Cy.WeaponType);
        I("fire_delay_ms", Cy.FireDelay);
        B("fire_delay_in_cycle", Cy.DelayInCycle);
        B("bolt_action", Cy.BoltAction);
        I("shots_before_rechamber", Cy.ShotsBeforeRechamber);
        B("no_rechamber_each_shot", Cy.NoRechamberEachShot);
        I("rechamber_ms", Cy.RechamberTime);
        I("rechamber_bolt_ms", Cy.RechamberBoltTime);
        I("burst_delay_ms", Cy.BurstDelay);
        I("burst_delay_step_ms", Cy.BurstDelayStep);
        I("burst_delay_min_ms", Cy.BurstDelayMin);
        I("intro_shots", Cy.IntroLength);
        I("intro_fire_time_ms", Cy.IntroFireTime);
        I("charge_hold_ms", Cy.ChargeHold);
        I("charge_levels", Cy.ChargeLevels);
        I("charge_time_ms", Cy.ChargeTime);
        return V;
    }

    // an attachment's values by the layout (empty for a name-only header: its fire time scale is 0, a
    // defined attachment's is never)
    Json AttachmentValues(const Gdt::Memory& M, int64_t A, const AttachmentLayout& L, const CycleLayout* Cy)
    {
        Json V = Json::object();
        if (L.FireTimeScale < 0 || M.At<float>(A + L.FireTimeScale) == 0) return V;
        const auto F = [&](const char* Col, int64_t Off) {
            if (Off >= 0) V[Col] = M.At<float>(A + Off);
        };
        if (Cy != nullptr)
        {
            const auto I = [&](const char* Col, int64_t Off) {
                if (Off >= 0) V[Col] = M.At<int32_t>(A + Off);
            };
            F("rechamber_scale", Cy->AttRechamberScale);
            F("rechamber_bolt_scale", Cy->AttRechamberBoltScale);
            F("burst_delay_scale", Cy->AttBurstDelayScale);
            I("burst_delay_step_set", Cy->AttBurstDelayStep);
            I("burst_delay_min_set", Cy->AttBurstDelayMin);
            I("shots_before_rechamber_set", Cy->AttShotsBeforeRechamber);
            F("intro_shots_scale", Cy->AttIntroLengthScale);
            F("intro_fire_time_scale", Cy->AttIntroFireTimeScale);
            I("burst_count_set", Cy->AttBurstCount);
            // a fire type outside the exe's table is not one (the records some dual-wield weapons point at)
            if (Cy->AttFireTypeSet >= 0 && Cy->AttFireType >= 0 && M.At<uint8_t>(A + Cy->AttFireTypeSet) != 0)
                if (const int32_t Ft = M.At<int32_t>(A + Cy->AttFireType); Ft >= 0 && Ft < Cy->FireTypeCount) V["fire_type_set"] = FireTypeName(*Cy, Ft);
        }
        F("fire_time_scale", L.FireTimeScale);
        F("clip_size_scale", L.ClipScale);
        F("range_scale", L.RangeScale);
        for (int I = 0; I < 6; I++)
        {
            if (L.DamageScales >= 0) V[std::format("damage_scale_{}", I + 1)] = M.At<float>(A + L.DamageScales + 4 * I);
            if (L.RangeScales >= 0) V[std::format("range_scale_{}", I + 1)] = M.At<float>(A + L.RangeScales + 4 * I);
        }
        if (L.ShotCount >= 0) V["shot_count"] = M.At<int32_t>(A + L.ShotCount);
        if (L.Locations >= 0)
            for (int I = 1; I < 21; I++) V[std::string("hit_") + Locations[I]] = M.At<float>(A + L.Locations + 4 * I);
        return V;
    }

    // an attachment unique's overrides by the layout: its hit location table, damage table, clip
    Json UniqueValues(const Gdt::Memory& M, int64_t U, const AttachmentLayout& L)
    {
        Json V = Json::object();
        if (L.UniqueLocations >= 0)
            if (const int64_t T = M.Follow(U + L.UniqueLocations); T >= 0)
                for (int I = 1; I < 21; I++) V[std::string("hit_") + Locations[I]] = M.At<float>(T + 4 * I);
        if (L.UniqueDamage >= 0 && M.At<int32_t>(U + L.UniqueDamage) != 0)
            for (int I = 0; I < 6; I++)
            {
                V[std::format("damage_{}", I + 1)] = M.At<int32_t>(U + L.UniqueDamage + 4 * I);
                if (L.UniqueRanges >= 0) V[std::format("range_{}_m", I + 1)] = M.At<float>(U + L.UniqueRanges + 4 * I) * MetresPerInch;
            }
        if (L.UniqueClip >= 0 && M.At<int32_t>(U + L.UniqueClip) > 0) V["clip_size"] = M.At<int32_t>(U + L.UniqueClip);
        return V;
    }

    // a zone's facts: its weapons' values by column (those Wants names: the run's name filter), its
    // localized strings; with a layout each weapon's attachment slots and the attachments' values
    std::string Facts(const ZonekitZone& Z, const std::function<bool(const std::string&)>& Wants, const AttachmentLayout* L,
                      const CycleLayout* Cy, size_t& Weapons, std::string& Error)
    {
        Json Out;
        Out["zone"] = Z.Name();
        Json List = Json::array();
        Json Atts = Json::object();   // by name, those this zone defines
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
                if (const auto* V = Find(Col.Field) != nullptr || Col.Alt == nullptr ? Find(Col.Field) : Find(Col.Alt); V != nullptr && (size_t)Col.Element < V->Numbers.size() && std::isfinite(V->Numbers[(size_t)Col.Element]))
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
            // guidedMissileType, by the layout's enum names (Cold War's scripts test it, var_5a70fda5)
            if (const auto* G = Find("guidedmissiletype"); G != nullptr && !G->Texts.empty()) W["guided_missile_type"] = G->Texts[0];
            W["values"] = std::move(Cells);
            if (Cy != nullptr)
                if (Json V = CycleValues(M, Pos, *Cy); !V.empty()) W["cycle"] = std::move(V);
            // its attachment slots: slot i's attachment and attachment unique (the unique's overrides
            // with it; the attachments' own values once per zone, by name)
            if (L != nullptr && L->List >= 0)
            {
                Json Slots = Json::array();
                const int64_t Table = M.Follow(Pos + L->List), Uniques = L->Uniques >= 0 ? M.Follow(Pos + L->Uniques) : -1;
                for (int I = 0; Table >= 0 && I < 64; I++)
                {
                    const int64_t A = M.Follow(Table + 8 * I);
                    const int64_t U = Uniques >= 0 ? M.Follow(Uniques + 8 * I) : -1;
                    if (A < 0 && U < 0) continue;
                    Json Slot;
                    Slot["slot"] = I;
                    if (A >= 0)
                    {
                        const std::string N = HashedName(M, A, "attachment");
                        Slot["attachment"] = N;
                        if (!N.empty() && !Atts.contains(N))
                            if (Json V = AttachmentValues(M, A, *L, Cy); !V.empty()) Atts[N] = std::move(V);
                    }
                    if (U >= 0)
                    {
                        Slot["unique"] = HashedName(M, U, "attachmentunique");
                        if (Json V = UniqueValues(M, U, *L); !V.empty()) Slot["unique_values"] = std::move(V);
                    }
                    Slots.push_back(std::move(Slot));
                }
                if (!Slots.empty()) W["attachment_slots"] = std::move(Slots);
            }
            List.push_back(std::move(W));
        }, Error);
        Weapons = List.size();
        Out["weapons"] = std::move(List);
        if (!Atts.empty()) Out["attachments"] = std::move(Atts);
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

    // the shot cycle columns, in table order (weapon_stats.csv; weapon_attachments.csv with fire_type
    // and burst_count before them)
    const char* const CycleColumns[] = { "fire_delay_ms", "fire_delay_in_cycle", "bolt_action", "shots_before_rechamber",
                                         "no_rechamber_each_shot", "rechamber_ms", "rechamber_bolt_ms", "burst_delay_ms",
                                         "burst_delay_step_ms", "burst_delay_min_ms", "intro_shots", "intro_fire_time_ms",
                                         "charge_hold_ms", "charge_levels", "charge_time_ms", "burst_cycle_ms",
                                         "shot_interval_ms", "shot_rpm" };

    double Get(const Json& J, const std::string& K, double Default)
    {
        return J.contains(K) && J[K].is_number() ? J[K].get<double>() : Default;
    }

    // a weapon's shot cycle with an attachment's values applied (A empty: the weapon alone), by the
    // exes' getters (README, Shot cycle): fire_type, fire_time_ms, burst_count and the cycle columns.
    // shot_interval_ms only where the code's timers give it whole: Full Auto and Single Shot (the fire
    // time, the fire delay when it is part of it, the rechamber when every shot asks for it), Burst and
    // Auto Burst without bolt action (the burst's fire times and the pause after it, over its shots).
    Json ApplyCycle(const Json& V, const Json& Cy, const std::string& FireType, const Json& A)
    {
        Json O = Json::object();
        const auto Int = [](double X) { return std::trunc(X); };
        const std::string Type = A.contains("fire_type_set") ? A["fire_type_set"].get<std::string>() : FireType;
        O["fire_type"] = Type;
        const double F = Int(Get(V, "fire_time_ms", 0) * Get(A, "fire_time_scale", 1));
        O["fire_time_ms"] = F;
        double N = Get(V, "burst_count", 1);
        if ((Type == "Burst" || Type == "Auto Burst") && Get(A, "burst_count_set", 0) != 0) N = Get(A, "burst_count_set", 0);
        O["burst_count"] = N;
        for (const auto& [K, X] : Cy.items())
            if (X.is_number()) O[K] = X.get<double>();
        // scaled when the scale is above 0 (rechamber), by every attachment (burst delay, intro), or set when not 0
        const auto Scaled = [&](const char* K, const char* S, bool Positive) {
            if (!O.contains(K)) return;
            const double Sc = Get(A, S, 1);
            if (!Positive || Sc > 0) O[K] = Int(O[K].get<double>() * Sc);
        };
        Scaled("rechamber_ms", "rechamber_scale", true);
        Scaled("rechamber_bolt_ms", "rechamber_bolt_scale", true);
        Scaled("burst_delay_ms", "burst_delay_scale", false);
        Scaled("intro_shots", "intro_shots_scale", false);
        Scaled("intro_fire_time_ms", "intro_fire_time_scale", false);
        const auto Set = [&](const char* K, const char* S) {
            if (O.contains(K) && Get(A, S, 0) != 0) O[K] = Get(A, S, 0);
        };
        Set("burst_delay_step_ms", "burst_delay_step_set");
        Set("burst_delay_min_ms", "burst_delay_min_set");
        Set("shots_before_rechamber", "shots_before_rechamber_set");
        // only what applies to this weapon: the rechamber with bolt action, the pause after a burst with
        // the fire types that end one, its step when set, the intro when its fire time differs, the
        // charge with the Charge Shot fire types (the raw fields stay in the facts)
        const bool Bolt = Get(O, "bolt_action", 0) != 0;
        const bool Charge = Type.rfind("Charge Shot", 0) == 0;
        const bool Bursts = Charge || Type == "Burst" || Type == "Auto Burst" || Type == "Stacked Fire" || Type == "Auto Stacked Fire" || Type == "All Locked";
        const bool Intro = Get(O, "intro_shots", 0) > 0 && Get(O, "intro_fire_time_ms", F) != F;
        std::vector<const char*> Drop;
        if (!Bolt) Drop.insert(Drop.end(), { "shots_before_rechamber", "no_rechamber_each_shot", "rechamber_ms", "rechamber_bolt_ms" });
        if (!Bursts) Drop.push_back("burst_delay_ms");
        if (!Bursts || Get(O, "burst_delay_step_ms", 0) == 0) Drop.insert(Drop.end(), { "burst_delay_step_ms", "burst_delay_min_ms" });
        if (!Intro) Drop.insert(Drop.end(), { "intro_shots", "intro_fire_time_ms" });
        if (!Charge) Drop.insert(Drop.end(), { "charge_hold_ms", "charge_levels", "charge_time_ms" });
        if (Get(O, "fire_delay_in_cycle", 0) == 0) Drop.push_back("fire_delay_in_cycle");
        for (const char* K : Drop) O.erase(K);
        if (!O.contains("fire_delay_ms") || F <= 0) return O;
        const double Kind = Get(O, "weapon_type", 0);
        const double D = Get(O, "fire_delay_ms", 0), Rech = Get(O, "rechamber_ms", 0), Burst = Get(O, "burst_delay_ms", 0);
        const bool InCycle = Get(O, "fire_delay_in_cycle", 0) != 0;
        const double Shots = Get(O, "shots_before_rechamber", 0);
        const bool EachShot = Shots == 1 || (Shots == 0 && Get(O, "no_rechamber_each_shot", 0) == 0);
        // the fire delay runs beside the fire time (both timers count down together) unless it is added
        // to it; a delay longer than the fire time is left out (its path not traced)
        const bool DelayOk = InCycle || D <= F;
        const double Shot = F + (InCycle ? D : 0);
        double Interval = -1;
        if (Intro || !DelayOk || Kind == 1 || Kind == 6)
            ;   // the intro shots' own fire time, a long delay, the grenade path: not given
        else if (Type == "Full Auto" || Type == "Single Shot")
        {
            if (!Bolt) Interval = Shot;
            else if (EachShot) Interval = Shot + Rech;
        }
        else if ((Type == "Burst" || Type == "Auto Burst") && !Bolt && N >= 1)
        {
            // after the burst's last fire time the weapon time is the burst delay (1 ms when it is 0)
            const double Cycle = N * Shot + (Burst > 0 ? Burst : 1);
            O["burst_cycle_ms"] = Cycle;
            Interval = std::round(Cycle / N * 100) / 100;
        }
        if (Interval > 0)
        {
            O["shot_interval_ms"] = Interval;
            O["shot_rpm"] = std::round(60000.0 / Interval * 100) / 100;
        }
        return O;
    }

    // weapon_attachments.csv: each weapon with the attachment (and attachment unique) of each of its
    // slots, one at a time, the game's rules applied to the weapon's values (README: the rules). Rows:
    // its rows.
    std::string AttachmentTable(const WeaponStatsTable::Weapons& Weapons, const std::map<std::string, Json>& Atts, bool Cycle, size_t& Rows)
    {
        std::vector<std::string> Head = { "weapon", "slot", "attachment", "unique", "fire_time_scale", "fire_time_ms", "rpm" };
        // the shot cycle with the attachment: a cell only where the attachment changes it
        if (Cycle)
        {
            Head.push_back("fire_type");
            Head.push_back("burst_count");
            for (const char* K : CycleColumns) Head.push_back(K);
        }
        for (const char* K : { "clip_size_scale", "clip_size", "shot_count", "range_scale" }) Head.push_back(K);
        for (int I = 1; I <= 6; I++) Head.push_back(std::format("damage_scale_{}", I));
        for (int I = 1; I <= 6; I++) Head.push_back(std::format("range_scale_{}", I));
        Head.push_back("unique_damage_table");
        for (int I = 1; I <= 6; I++)
        {
            Head.push_back(std::format("damage_{}", I));
            Head.push_back(std::format("range_{}_m", I));
        }
        for (int I = 1; I < 21; I++) Head.push_back(std::string("hit_") + Locations[I]);
        std::string Csv = WeaponStatsTable::Line(Head);
        Json Unused = Json::array();
        const Json None = Json::object();
        for (const auto& [Name, Entry] : Weapons)
        {
            const Json& W = Entry.first;
            if (!W.contains("attachment_slots")) continue;
            const Json& V = W["values"];
            const Json& Slots = W["attachment_slots"];
            // the default attachment unique (slot 0): a unique's hit location replaces the weapon's where it differs
            const Json& Default = !Slots.empty() && Slots[0].value("slot", -1) == 0 && Slots[0].contains("unique_values") ? Slots[0]["unique_values"] : None;
            for (const auto& S : Slots)
            {
                const int Index = S.value("slot", -1);
                const std::string Att = S.value("attachment", std::string());
                const auto It = Atts.find(Att);
                const Json& A = It != Atts.end() ? It->second : None;
                const Json& U = S.contains("unique_values") ? S["unique_values"] : None;
                if (Index == 0 || (A.empty() && U.empty())) continue;   // slot 0: the weapon alone
                std::map<std::string, std::string> C;
                C["weapon"] = Name;
                C["slot"] = std::to_string(Index);
                C["attachment"] = Att;
                C["unique"] = S.value("unique", std::string());
                for (const auto& [K, X] : A.items())
                    if (X.is_number() && K.rfind("hit_", 0) != 0) C[K] = Num(X.get<double>());
                if (const double Fire = Get(V, "fire_time_ms", 0); Fire > 0)
                {
                    const double Ms = std::trunc(Fire * Get(A, "fire_time_scale", 1));
                    C["fire_time_ms"] = Num(Ms);
                    if (Ms > 0) C["rpm"] = Num(std::round(60000.0 / Ms * 100) / 100);
                }
                if (Cycle && W.contains("cycle"))
                {
                    const std::string Type = W.value("fire_type", std::string());
                    const Json Base = ApplyCycle(V, W["cycle"], Type, None), With = ApplyCycle(V, W["cycle"], Type, A);
                    if (With["fire_type"] != Base["fire_type"]) C["fire_type"] = With["fire_type"].get<std::string>();
                    if (With["burst_count"] != Base["burst_count"]) C["burst_count"] = Num(With["burst_count"].get<double>());
                    for (const char* K : CycleColumns)
                        if (With.contains(K) && (!Base.contains(K) || With[K] != Base[K])) C[K] = Num(With[K].get<double>());
                }
                if (U.contains("clip_size")) C["clip_size"] = Num(Get(U, "clip_size", 0));
                else if (const double Clip = Get(V, "clip_size", 0); Clip > 0)
                    C["clip_size"] = Num(Clip > 1 ? std::max(1.0, std::round(Clip * Get(A, "clip_size_scale", 1))) : Clip);
                if (Get(A, "shot_count", 0) <= 0) C.erase("shot_count");
                // the damage table: the unique's when it sets one, else the weapon's; each stop's damage
                // (int) x its scale, its range x its scale x the global range scale
                const bool Own = U.contains("damage_1");
                C["unique_damage_table"] = Own ? "1" : "0";
                for (int I = 1; I <= 6; I++)
                {
                    const std::string D = std::format("damage_{}", I), R = std::format("range_{}_m", I);
                    const Json& T = Own ? U : V;
                    if (T.contains(D)) C[D] = Num(std::trunc(Get(T, D, 0) * Get(A, std::format("damage_scale_{}", I), 1)));
                    if (T.contains(R))
                        C[R] = Num(Get(T, R, 0) * Get(A, std::format("range_scale_{}", I), 1) * Get(A, "range_scale", 1));
                }
                for (int I = 1; I < 21; I++)
                {
                    const std::string H = std::string("hit_") + Locations[I];
                    if (!V.contains(H)) continue;
                    double X = Get(V, H, 1);
                    if (U.contains(H) && Default.contains(H) && Get(U, H, 1) != Get(Default, H, 1)) X = Get(U, H, 1);
                    if (const double M = Get(A, H, 1); M != 1) X *= M;
                    C[H] = Num(X);
                }
                WeaponStatsTable::AddRow(Head, C, Csv, Unused);
                Rows++;
            }
        }
        return Csv;
    }

    // every facts file of Dir joined into the table
    bool Merge(const std::string& Dir, const char* Title, const AttachmentLayout* L, const CycleLayout* Cy, size_t& Rows, std::string& Error)
    {
        std::vector<fs::path> Files;
        std::error_code Ec;
        for (const auto& E : fs::directory_iterator(fs::path(Dir) / "facts", Ec))
            if (E.path().extension() == ".json") Files.push_back(E.path());
        std::sort(Files.begin(), Files.end());
        std::map<std::string, std::string> Strings;
        WeaponStatsTable::Weapons Weapons;   // the first facts
        std::map<std::string, Json> Attachments;   // by name, the first facts defining it
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
            if (J.contains("attachments"))
                for (const auto& [N, V] : J["attachments"].items())
                    if (!Attachments.contains(N)) Attachments[N] = V;
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
        // guidedMissileType, when the game's layout reads it (Cold War)
        const bool Guided = std::any_of(Weapons.begin(), Weapons.end(), [](const auto& E) { return E.second.first.contains("guided_missile_type"); });
        if (Guided) Head.push_back("guided_missile_type");
        // the shot cycle columns after burst_count, when the game's layout reads them
        const bool Cycle = Cy != nullptr && std::any_of(Weapons.begin(), Weapons.end(), [](const auto& E) { return E.second.first.contains("cycle"); });
        for (const auto& Col : Numbers)
        {
            Head.push_back(Col.Name);
            if (Cycle && std::string(Col.Name) == "burst_count")
                for (const char* K : CycleColumns) Head.push_back(K);
        }
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
            Cell["guided_missile_type"] = W.value("guided_missile_type", std::string());
            for (const auto& Col : Numbers)
                if (V.contains(Col.Name)) Cell[Col.Name] = Num(V[Col.Name].get<double>());
            if (V.contains("fire_time_ms") && V["fire_time_ms"].get<double>() > 0)
                Cell["rpm"] = Num(std::round(60000.0 / V["fire_time_ms"].get<double>() * 100) / 100);
            if (Cycle && W.contains("cycle"))
            {
                const Json O = ApplyCycle(V, W["cycle"], W.value("fire_type", std::string()), Json::object());
                for (const char* K : CycleColumns)
                    if (O.contains(K)) Cell[K] = Num(O[K].get<double>());
            }
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
            "- class, fire_type: the layouts' enum names (weapclass: the exe's own table of fifteen, rifle, mg,\n"
            "  smg, spread, pistol, grenade, rocketlauncher, turret, non-player, gas, item, melee, Killstreak Alt\n"
            "  Stored Weapon, pistol spread, ball; firetype: Black Ops III's list).\n"
            "- fire_time_ms: firetime, milliseconds between two shots; rpm = 60000 / fire_time_ms.\n"
            "- burst_count: rounds a burst. clip_size: rounds a magazine. start_ammo, max_ammo: as stored, in\n"
            "  magazines when ammo_count_clip_relative is 1, else rounds (Black Ops III's rule, its Mod Tools' help).\n"
            "- ammo_count_clip_relative: ammoCountClipRelative, Black Ops 4 only (WeaponTunables 0x10bf, the byte\n"
            "  acts' struct declares under that name; 1 on 181 of the 185 guns whose max_ammo is a magazine count,\n"
            "  ar_accurate_t8_zm 9 x 35, and 0 on ar_standard_t8_zm's 490 rounds; the script field table's maxAmmo\n"
            "  and startAmmo entries multiply 0xd4c / 0xe40 by the clip size when it is set). Cold War: the style\n"
            "  settings' byte 0x18c (its maxAmmo and startAmmo accessors, 0x7ff63a7b09b0 / 0x7ff63a7b0940 in the\n"
            "  decrypted exe, multiply by the clip getter when it is set; the XM4's 3 / 6 are magazines).\n"
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
            "- hit_<location>: the hit location multipliers, x damage: locationDamageMultipliers, a table of 21\n"
            "  floats the weapon points at (Black Ops 4: the tunables' 0x0; Cold War: the style settings' 0xb0),\n"
            "  indexed by hit location. Read in the decrypted exes: the hit location function (Black Ops 4\n"
            "  0x7ff7916e16c0, Cold War 0x7ff638fe75c0) returns table[location], the damage code multiplies the\n"
            "  damage by it and truncates (int); the location names are the exe's own table of 21 (none,\n"
            "  helmet, head ... left_foot, gun, riotshield; entry 0, none, is not a column). An attachment\n"
            "  unique's table (Black Ops 4) or each attachment's multipliers (Cold War, multiplied in) are not\n"
            "  applied. Without a weapon the game uses the global lochit_dmgtable.\n"
            "- zm_rarity_green, zm_rarity_blue, zm_rarity_purple, zm_rarity_orange, zm_pap2, zm_pap3 (Cold War):\n"
            "  the zombies damage multipliers of the weapon's tunables 0xd2c, 0xd30, 0xd34, 0xd38, 0xd3c, 0xd40.\n"
            "  The exe's script field table (rdata 0xd6e6ce0) gives these offsets to the fields zombies' scripts\n"
            "  read: zm_weapons picks one of the four by the loot item's rarity (white or none: x1) and the\n"
            "  Pack-a-Punch level 2 or 3 one (level 1 is the upgraded weapon itself), and zm.gsc multiplies a\n"
            "  hit's damage by both (Rifling games/coldwar.md, Zombies). XM4 1.45 / 2.05 / 2.95 / 4.\n"
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
            "  puts fireTime, clipSize, reloadTime and meleeTime first on their proven offsets.\n\n";
        size_t AttRows = 0;
        const std::string AttCsv = L != nullptr && !Attachments.empty() ? AttachmentTable(Weapons, Attachments, Cycle, AttRows) : std::string();
        if (Cycle)
            Readme += "Shot cycle (" + std::string(Title) + ", read in the decrypted executable; Rifling games/<game>.md, Shot cycle)\n"
                      "The weapon's state machine runs two timers side by side, the weapon time (the state's length) and\n"
                      "the weapon delay (the time before the shot), both counted down by the frame's milliseconds.\n"
                      "A cell is written only where its field applies: the rechamber with bolt_action, burst_delay_ms\n"
                      "with the fire types that end a burst, its step and minimum when the step is set, the intro when\n"
                      "its fire time differs from fire_time_ms, the charge with the Charge Shot fire types,\n"
                      "fire_delay_in_cycle when 1 (each weapon's raw fields: facts/<zone>.json, \"cycle\").\n"
                      "- fire_delay_ms: the weapon delay a shot starts with (fireDelay), at each shot fired (the shot\n"
                      "  start sets it, the weapon time getting the fire time at the same moment): the shot leaves that\n"
                      "  long after the start, the next one can start fire_time_ms after it. fire_delay_in_cycle 1: the\n"
                      "  game adds the delay to that shot's weapon time instead. No attachment changes it (the shot start\n"
                      "  reads the tunables' field itself; perks and an ADS case can lengthen it at run time).\n"
                      "- bolt_action, rechamber_ms: a bolt-action weapon's shot asks for a rechamber (every shot when\n"
                      "  shots_before_rechamber is 0 and no_rechamber_each_shot is 0, else every shots_before_rechamber\n"
                      "  shots; charged shots count their level), whatever the fire type. Once the fire time is over the\n"
                      "  weapon goes to the rechamber state for rechamber_ms (attachments: x their scale when above 0);\n"
                      "  rechamber_bolt_ms is a second timer inside it, not added. Without bolt_action no rechamber runs,\n"
                      "  whatever rechamber_ms holds. A player state the code tests can hold the rechamber back (not\n"
                      "  named, not counted).\n"
                      "- burst_delay_ms: after the last shot of a burst (fire types Burst, Auto Burst, Stacked Fire, All\n"
                      "  Locked and the three Charge Shot ones) and its fire time, the weapon time is set to it (1 ms when\n"
                      "  it is 0): the pause between two bursts, Auto Burst holding the trigger as Burst pressing it.\n"
                      "  burst_delay_step_ms (when not 0) takes that much off per burst fired in a row, down to\n"
                      "  burst_delay_min_ms; the base value is the first pause. Attachments: x their scale, step and\n"
                      "  minimum replaced when they set one.\n"
                      "- intro_shots, intro_fire_time_ms: the first intro_shots shots of a trigger pull fire at\n"
                      "  intro_fire_time_ms instead of the fire time (the shot count restarts when the weapon was not\n"
                      "  firing). Attachments: x their scales.\n"
                      "- charge_hold_ms, charge_levels, charge_time_ms (Charge Shot fire types): holding the trigger adds\n"
                      "  the frame's milliseconds; past charge_hold_ms the charge begins, its level is held time /\n"
                      "  (charge_time_ms / charge_levels) + 1, at most charge_levels (and the rounds left). The shot fires\n"
                      "  that many shots, then burst_delay_ms. The hold before the release is the player's: no interval.\n"
                      "- shot_interval_ms, shot_rpm: the time between two shots within a magazine, only where those\n"
                      "  timers give it whole. Full Auto and Single Shot: fire_time_ms (+ fire_delay_ms when\n"
                      "  fire_delay_in_cycle), + rechamber_ms when the weapon is bolt action and every shot rechambers.\n"
                      "  Burst and Auto Burst without bolt action: burst_cycle_ms = burst_count x fire time + burst delay,\n"
                      "  shot_interval_ms = burst_cycle_ms / burst_count. Left empty: intro shots, a fire delay longer\n"
                      "  than the fire time (its path not traced), rechambers every n > 1 shots, bolt-action bursts,\n"
                      "  grenades (the shot start's other path, weapon type 1 or 6), Charge Shot, Minigun (spin-up), the\n"
                      "  other fire types. Single Shot: the fastest the trigger can be\n"
                      "  pulled again. Not counted: the last round's own fire time and the magazine's reload.\n"
                      "- weapon_attachments.csv: fire_type, burst_count and these columns with the attachment applied,\n"
                      "  a cell only where the attachment changes the weapon's value (fire_delay_ms then never).\n" +
                      std::string(Cy->Notes) + "\n";
        if (Guided)
            Readme += "guided_missile_type (Cold War): guidedMissileType, the tunables' int at 0x410 named by the exe's table of\n"
                      "12 (None, Sidewinder, Hellfire, Javelin, Ballistic, WireGuided, TVGuided, Drone, HeatSeeking, Robotech,\n"
                      "DynamicImpactPoint, Drunken). The scripts' var_5a70fda5: the exe's script field table row of that hash\n"
                      "(rdata 0xd6e66a0: tunables, offset 0x410, 4 bytes) reads it through 0x7ff63a7b0a20 -> 0x7ff63a7b2610,\n"
                      "which returns the name from that table (0x7ff63df88830, BlackOpsColdWar_dump.exe).\n\n";
        if (!AttCsv.empty())
            Readme += "weapon_attachments.csv: each weapon with the attachment and attachment unique of each of its slots,\n"
                      "one at a time (" + std::to_string(AttRows) + " rows; slot 0, the default attachment, is the weapon alone). Slot i\n"
                      "is the i-th of the weapon's 64 attachment pointers and of its attachment unique pointers (an id packs\n"
                      "the slots of its attachments, 6 bits each). An attachment defined in another zone (a name-only\n"
                      "header here) has its values when that zone is in the same --out. Columns: the attachment's own\n"
                      "scales (fire_time_scale, clip_size_scale, shot_count when it sets the pellets, range_scale the\n"
                      "global one, damage_scale_<n> / range_scale_<n> per stop), then the weapon's values with them:\n"
                      "fire_time_ms (int) of fireTime x scale; clip_size the unique's when it sets one, else\n"
                      "max(1, round(clip x scale)) (the clip dvar taken as 1); damage_<n> (int) of the stop's damage x\n"
                      "its scale, range_<n>_m the stop's range x its scale x the global scale, from the unique's damage\n"
                      "table when it sets one (unique_damage_table 1); hit_<location>: the weapon's multiplier, the\n"
                      "unique's where its table differs from the default unique's (slot 0's), x the attachment's own.\n" +
                      std::string(L->Notes) + "\n";
        Readme += "Not here (no proven place yet): ADS times, drop times, empty-reload times" +
                  std::string(AttCsv.empty() ? ", attachments" : "") + ".\n"
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
                        (AttCsv.empty() || ExportWriters::WriteBytes(fs::path(Dir) / "weapon_attachments.csv", (const uint8_t*)AttCsv.data(), AttCsv.size())) &&
                        ExportWriters::WriteBytes(fs::path(Dir) / "weapon_stats.json", (const uint8_t*)JsonText.data(), JsonText.size()) &&
                        ExportWriters::WriteBytes(fs::path(Dir) / "README.txt", (const uint8_t*)Readme.data(), Readme.size());
        if (!Ok) Error = "write failed in " + Dir;
        Rows = Weapons.size();
        return Ok;
    }
}

void Treyarch::WeaponStats::Export(const ZonekitZone& Zone, const DiskGame::Context& C, DiskGame::Totals& T, const char* Title,
                                   const AttachmentLayout* Attachments, const CycleLayout* Cycle)
{
    using DiskGame::Kind;
    auto& Kt = T.Kinds[Kind::WeaponStats];
    size_t Weapons = 0;
    std::string Error;
    const std::string Facts = ::Facts(Zone, [&C](const std::string& Name) { return C.Wants(Kind::WeaponStats, Name); }, Attachments, Cycle, Weapons, Error);
    Kt.Seen += Weapons;
    if (!Error.empty()) T.Fail(Zone.Name(), Kind::WeaponStats, "", Error);
    if (Facts.empty()) return;
    WeaponStatsTable::WriteFacts(Zone.Name(), Facts, C, T, [Title, Attachments, Cycle](const std::string& Dir, size_t& Rows, std::string& Error) {
        return Merge(Dir, Title, Attachments, Cycle, Rows, Error);
    });
}
