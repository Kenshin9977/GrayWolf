#include "stdafx.h"

#include "Games/Common/WeaponFiles.h"
#include "Games/Common/WeaponStatsTable.h"

#include "ExportNaming.h"
#include "ExportWriters.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>

namespace fs = std::filesystem;

namespace
{
    using Json = nlohmann::ordered_json;
    using ExportNaming::Lower;
    using ExportNaming::Num;
    using ExportWriters::CsvField;

    constexpr double MetresPerInch = 0.0254;

    using WeaponFiles::Column;
    using WeaponFiles::Unit;

    // the number columns, in table order: (column, the weapon file's field, its unit). The damage
    // stops are apart (Stops below).
    const Column Before[] = {
        { "fire_time_ms", "fireTime", Unit::Ms },
        { "shot_count", "shotCount", Unit::Count },
        { "clip_size", "clipSize", Unit::Count },
        { "start_ammo", "startAmmo", Unit::Count },
        { "max_ammo", "maxAmmo", Unit::Count },
        { "ammo_count_clip_relative", "ammoCountClipRelative", Unit::Count },
        { "reload_ms", "reloadTime", Unit::Ms },
        { "reload_empty_ms", "reloadEmptyTime", Unit::Ms },
        { "reload_add_ms", "reloadAddTime", Unit::Ms },
        { "reload_empty_add_ms", "reloadEmptyAddTime", Unit::Ms },
        { "reload_start_ms", "reloadStartTime", Unit::Ms },
        { "reload_end_ms", "reloadEndTime", Unit::Ms },
        { "first_raise_ms", "firstRaiseTime", Unit::Ms },
        { "raise_ms", "raiseTime", Unit::Ms },
        { "raise_quick_ms", "quickRaiseTime", Unit::Ms },
        { "drop_ms", "dropTime", Unit::Ms },
        { "sprint_out_ms", "sprintOutTime", Unit::Ms },
        { "ads_time_ms", "adsTransInTime", Unit::Ms },
        { "ads_out_time_ms", "adsTransOutTime", Unit::Ms },
    };
    // the damage table's stops, nearest first: Black Ops II's six (damage[0..5], damageRange[0..5]),
    // the others' two (damage up to maxDamageRange, minDamage from minDamageRange), CoD1's one
    // (damage); the columns damage_<n>, range_<n>_m numbered by the stops the weapon file has
    const std::pair<const char*, const char*> Stops[] = { { "damage", "maxDamageRange" }, { "damage2", "damageRange2" },
                                                         { "damage3", "damageRange3" },  { "damage4", "damageRange4" },
                                                         { "damage5", "damageRange5" },  { "minDamage", "minDamageRange" } };
    const Column After[] = {
        { "melee_damage", "meleeDamage", Unit::Count },
        { "explosion_radius_m", "explosionRadius", Unit::Inches },
        { "explosion_radius_min_m", "explosionRadiusMin", Unit::Inches },
        { "explosion_inner_damage", "explosionInnerDamage", Unit::Count },
        { "explosion_outer_damage", "explosionOuterDamage", Unit::Count },
        { "projectile_speed_mps", "projectileSpeed", Unit::InchesPerSecond },
        { "move_speed_scale", "moveSpeedScale", Unit::Scale },
        { "ads_move_speed_scale", "adsMoveSpeedScale", Unit::Scale },
    };
    // the hit location multipliers (locationDamageMultipliers): loc<Name> in the file, hit_<name> here
    const std::pair<const char*, const char*> HitLocations[] = {
        { "locHelmet", "hit_helmet" },
        { "locHead", "hit_head" },
        { "locNeck", "hit_neck" },
        { "locTorsoUpper", "hit_torso_upper" },
        { "locTorsoMid", "hit_torso_mid" },
        { "locTorsoLower", "hit_torso_lower" },
        { "locRightArmUpper", "hit_right_arm_upper" },
        { "locLeftArmUpper", "hit_left_arm_upper" },
        { "locRightArmLower", "hit_right_arm_lower" },
        { "locLeftArmLower", "hit_left_arm_lower" },
        { "locRightHand", "hit_right_hand" },
        { "locLeftHand", "hit_left_hand" },
        { "locRightLegUpper", "hit_right_leg_upper" },
        { "locLeftLegUpper", "hit_left_leg_upper" },
        { "locRightLegLower", "hit_right_leg_lower" },
        { "locLeftLegLower", "hit_left_leg_lower" },
        { "locRightFoot", "hit_right_foot" },
        { "locLeftFoot", "hit_left_foot" },
        { "locGun", "hit_gun" },
    };
    // the text fields kept
    const char* const Texts[] = { "displayName", "weaponType", "weaponClass", "fireType" };

    bool Number(const std::string& Text, double& V)
    {
        if (Text.empty()) return false;
        char* End = nullptr;
        V = std::strtod(Text.c_str(), &End);
        return End != Text.c_str() && *End == '\0' && std::isfinite(V);
    }

    std::string Cell(const Json& F, const char* Field, Unit How, bool TruncateMs = false)
    {
        if (!F.contains(Field)) return std::string();
        if (How == Unit::Text) return F[Field].get<std::string>();
        double V = 0;
        if (!Number(F[Field].get<std::string>(), V)) return std::string();
        switch (How)
        {
        case Unit::Text: break;
        case Unit::ChargeShots:
        {
            const double Cost = std::min(std::trunc((double)((float)V * 4095.0f)), 4095.0);
            return Cost > 0 ? Num(std::floor(4095 / Cost), 0) : std::string();
        }
        case Unit::Count: return Num(V, 0);
        // (Infinite Warfare's ammo per shot, 0x737530: max(1, the i32))
        case Unit::PerShot: return Num(V > 1 ? std::trunc(V) : 1, 0);
        // (Infinite Warfare's burst pause, 0x730169: (int)floor(ms + 0.5), a Burst weapon's only)
        case Unit::BurstMs:
            return F.contains("fireType") && F["fireType"].get<std::string>() == "Burst" ? Num(WeaponStatsTable::BurstDelayMs(V), 0) : std::string();
        case Unit::BoltMs:
            if (!F.contains("boltAction") || F["boltAction"].get<std::string>() != "1") return std::string();
            return Num(std::round(V * 1000), 0);
        // the weapon file writes the ms field in seconds (ms / 1000); a game parsing it as (int)(s x 1000)
        // truncates
        case Unit::Ms: return Num(TruncateMs ? std::trunc(V * 1000) : std::round(V * 1000), 0);
        case Unit::Inches: return Num(V * MetresPerInch, 2);
        case Unit::InchesPerSecond: return Num(V * MetresPerInch, 2);
        case Unit::Scale: return Num(V, 4);
        }
        return std::string();
    }

    bool Patch(const std::string& Zone) { return Lower(Zone).find("patch") != std::string::npos; }

    // Black Ops II's attachment files: the fields the attachment table reads (the scales a weapon's
    // value is multiplied by, as their names say), and the weapon file's list of its attachments
    const char* const WeaponAttachmentList = "attachments";
    const char* const AttachmentFields[] = { "displayName",         "attachmentType",       "clipSizeScale",      "fireTimeScale",
                                             "reloadTimeScale",     "reloadEmptyTimeScale", "reloadAddTimeScale", "adsTransInTimeScale",
                                             "adsTransOutTimeScale", "damageRangeScale",    "adsMoveSpeedScale",
                                             // Black Ops III's (its attachment.gdf): the six stops' own damage and
                                             // range scales, the burst delay's
                                             "damageScale1", "damageScale2", "damageScale3", "damageScale4", "damageScale5",
                                             "damageScale6", "damageRangeScale1", "damageRangeScale2", "damageRangeScale3",
                                             "damageRangeScale4", "damageRangeScale5", "damageRangeScale6", "burstDelayTimeScale" };

    // A game's attachment rules, applied to a weapon with a set of attachments in its primary mode:
    // the values the game's own code reads (the RVAs in the README). The weapon file's fields (F), the
    // attachment files by name (Atts: their facts' fields), the chosen attachments; the row's columns
    // in C, the attachments in the order the rules take them in Order.
    bool Get(const Json& J, const std::string& Field, double& V)
    {
        return J.contains(Field) && J[Field].is_string() && Number(J[Field].get<std::string>(), V);
    }
    double Or(const Json& J, const std::string& Field, double Default)
    {
        double V = 0;
        return Get(J, Field, V) ? V : Default;
    }
    std::string Text(const Json& J, const std::string& Field)
    {
        return J.contains(Field) && J[Field].is_string() ? J[Field].get<std::string>() : std::string();
    }
    // a list field (the weapon's attachments, its overrides): one entry a line, or ';' separated (WWII's)
    std::vector<std::string> Lines(std::string S)
    {
        std::replace(S.begin(), S.end(), ';', '\n');
        std::vector<std::string> Out;
        std::istringstream In(S);
        for (std::string L; std::getline(In, L);)
        {
            while (!L.empty() && (L.back() == '\r' || L.back() == ' ' || L.back() == '\t')) L.pop_back();
            if (!L.empty()) Out.push_back(L);
        }
        return Out;
    }
    std::vector<std::string> Words(const std::string& S)
    {
        std::vector<std::string> Out;
        std::istringstream In(S);
        for (std::string W; In >> W;) Out.push_back(W);
        return Out;
    }
    // a weapon file time (seconds) as the asset's milliseconds
    bool Ms(const Json& F, const std::string& Field, double& V)
    {
        if (!Get(F, Field, V)) return false;
        V = std::round(V * 1000);
        return true;
    }
    void Fire(std::map<std::string, std::string>& C, double Fire)
    {
        C["fire_time_ms"] = Num(Fire, 0);
        if (Fire > 0) C["rpm"] = Num(std::round(60000.0 / Fire * 100) / 100, 2);
    }

    // MW3's attachments (the iw5. fields OatWeapons writes from OAT's attachment JSON) and the weapon
    // file's animOverrides, applied as iw5mp.exe does (the README says the rules and their RVAs)
    const char* const Iw5Format = "iw5.format";
    // the weapon's times the akimbo state takes (iw5mp 0x4cb20, 0x517b0, 0x51460, 0x51250)
    const char* const AkimboFields[] = { "fireTimeAkimbo", "reloadTimeAkimbo", "reloadEmptyTimeAkimbo", "reloadAddTimeAkimbo" };
    bool Iw5Counts(const Json& A)
    {
        // (0x4e6e0) an underbarrel counts in the primary mode only when its weapClass is rifle (grip), it
        // has adsSettingsMain, or adsSettingsScaleMain is neither 0 nor 1; else it is the alternate mode's
        const bool Underbarrel = Text(A, "attachmentType") == "underbarrel";
        const double ScaleMain = Or(A, "iw5.adsSettingsScaleMain", 0);
        return !Underbarrel || Text(A, "iw5.weapClass") == "rifle" || A.contains("iw5.adsSettingsMain") || (ScaleMain != 0 && ScaleMain != 1);
    }
    void Iw5Values(const Json& F, const Json& Atts, const std::vector<std::string>& Chosen, std::map<std::string, std::string>& C,
                   std::vector<std::string>& Order)
    {
        const std::vector<std::string> List = Lines(Text(F, WeaponAttachmentList));
        const auto Of = [&](const std::string& Name) -> const Json& { return Atts[Name]; };
        const auto Type = [&](const std::string& Name) { return Text(Of(Name), "attachmentType"); };
        std::string Scope, Under;
        std::vector<std::string> Others;
        // the Weapon's bits: one scope, one underbarrel, the others a mask in the weapon's others[] order
        for (const auto& L : List)
            if (std::find(Chosen.begin(), Chosen.end(), L) != Chosen.end())
            {
                const std::string T = Type(L);
                if (T == "scope") Scope = L;
                else if (T == "underbarrel") Under = L;
                else Others.push_back(L);
            }
        // (0x4e6e0) the three slots: the scope, the others, the underbarrel when it counts; a fourth
        // takes the third slot
        std::vector<std::string> Slots;
        const auto Slot = [&](const std::string& N) {
            if (Slots.size() < 3) Slots.push_back(N);
            else Slots[2] = N;
        };
        if (!Scope.empty()) Slot(Scope);
        for (const auto& O : Others) Slot(O);
        if (!Under.empty() && Iw5Counts(Of(Under))) Slot(Under);
        Order = Slots;
        if (!Under.empty() && !Iw5Counts(Of(Under))) Order.push_back(Under);
        // (0x53d90) the animation overrides' set: scope, underbarrel (counting or not), the others; a
        // fourth takes the third place
        std::vector<std::string> Set;
        for (const auto* N : { &Scope, &Under })
            if (!N->empty()) Set.push_back(*N);
        for (const auto& O : Others)
            if (Set.size() < 3) Set.push_back(O);
            else Set[2] = O;
        const auto InSet = [&](const std::string& N) { return N != "none" && std::find(Set.begin(), Set.end(), N) != Set.end(); };
        // the entry of a kind matching the most of the set (att1, att2 each counting one; the first of the
        // best; none matching none); its time
        const auto Override = [&](const std::string& Kind, double& Time) {
            int Best = 0;
            for (const auto& Line : Lines(Text(F, "animOverrides")))
            {
                const auto W = Words(Line);
                if (W.size() < 7 || W[2] != Kind) continue;
                const int Score = (InSet(W[0]) ? 1 : 0) + (InSet(W[1]) ? 1 : 0);
                if (Score > Best && Number(W[5], Time)) Best = Score;
            }
            return Best > 0;
        };
        // (0x538f0) the akimbo state: an attachment named akimbo
        const bool Akimbo = std::find(Chosen.begin(), Chosen.end(), "akimbo") != Chosen.end();
        double V = 0;
        if (Get(F, "clipSize", V))
        {
            // (0x4c470) the first slot's ammunition clipSize, else the weapon's; x every nonzero ammunitionScale
            float Clip = (float)V, Scale = 1;
            bool Block = false;
            for (const auto& S : Slots)
            {
                if (!Block && Of(S).contains("iw5.ammunition")) Clip = (float)Or(Of(S), "iw5.ammunition.clipSize", 0), Block = true;
                if (const float X = (float)Or(Of(S), "iw5.ammunitionScale", 0); X != 0) Scale *= X;
            }
            C["clip_size"] = Num(std::trunc(Clip * Scale), 0);
        }
        // (0x4cb20) fire: the akimbo time, else the fire override (0: none), else fireTime; x every slot's
        // fireTimersScale
        {
            double T = 0;
            const bool Has = Akimbo ? Ms(F, "fireTimeAkimbo", T) : ((Override("fire", T) && T != 0) || Ms(F, "fireTime", T));
            if (Has && T != 0)
            {
                float Scale = 1;
                for (const auto& S : Slots) Scale *= (float)Or(Of(S), "iw5.fireTimersScale", 1);
                Fire(C, std::trunc((float)T * Scale));
            }
        }
        // (0x517b0, 0x51460, 0x51250) reloads: the akimbo times, else the override (reload: 0 is none;
        // reload_empty: any entry matching), else the weapon's; reload add has no attachment override in
        // the primary mode (reloadOverrides: the alternate mode's, 0x53c60)
        {
            double T = 0;
            if (Akimbo ? Ms(F, "reloadTimeAkimbo", T) : ((Override("reload", T) && T != 0) || Ms(F, "reloadTime", T))) C["reload_ms"] = Num(T, 0);
            if (Akimbo ? Ms(F, "reloadEmptyTimeAkimbo", T) : (Override("reload_empty", T) || Ms(F, "reloadEmptyTime", T)))
                C["reload_empty_ms"] = Num(T, 0);
            if (Ms(F, Akimbo ? "reloadAddTimeAkimbo" : "reloadAddTime", T)) C["reload_add_ms"] = Num(T, 0);
        }
        // (0x4b990) ADS: the first slot's block (adsSettingsMain for one sharing its ammo with the
        // alternate mode, else adsSettings; 0 gives 300/500 ms), else the weapon's; divided by every
        // slot's scale
        {
            double Scale = 1;
            std::string Block, BlockOf;
            for (const auto& S : Slots)
            {
                const Json& A = Of(S);
                const bool Shares = Or(A, "iw5.shareAmmoWithAlt", 0) != 0;
                const std::string B = Shares ? "iw5.adsSettingsMain" : "iw5.adsSettings";
                if (Block.empty() && A.contains(B)) Block = B, BlockOf = S;
                if (!Shares) Scale *= Or(A, "iw5.adsSettingsScale", 0) > 0 ? Or(A, "iw5.adsSettingsScale", 0) : 1;
                else if (!A.contains("iw5.adsSettingsMain") && Or(A, "iw5.adsSettingsScaleMain", 0) != 0) Scale *= Or(A, "iw5.adsSettingsScaleMain", 0);
            }
            for (const auto& [Col, Field, Default] : { std::tuple<const char*, const char*, double>{ "ads_time_ms", "adsTransInTime", 300 },
                                                       { "ads_out_time_ms", "adsTransOutTime", 500 } })
            {
                double T = 0;
                if (!Block.empty())
                    T = Or(Of(BlockOf), Block + "." + Field, 0) > 0 ? Or(Of(BlockOf), Block + "." + Field, 0) * 1000 : Default;
                else if (Get(F, Field, V))
                    T = V * 1000;
                else
                    continue;
                C[Col] = Num(std::round(T / Scale), 0);
            }
        }
        // (0x4c7a0) ranges: the first slot's damage block, else the weapon's; x every nonzero
        // damageScale / damageScaleMin
        size_t Stop = 0;
        for (const auto& [Damage, Range] : Stops)
        {
            if (!F.contains(Damage)) continue;
            Stop++;
            const bool Min = std::string(Range) == "minDamageRange";
            if ((!Min && std::string(Range) != "maxDamageRange") || !Get(F, Range, V)) continue;
            double R = V, Scale = 1;
            bool Block = false;
            for (const auto& S : Slots)
            {
                if (!Block && Of(S).contains("iw5.damage")) R = Or(Of(S), std::string("iw5.damage.") + Range, 0), Block = true;
                if (const double X = Or(Of(S), Min ? "iw5.damageScaleMin" : "iw5.damageScale", 0); X != 0) Scale *= X;
            }
            C[std::format("range_{}_m", Stop)] = Num(R * Scale * MetresPerInch, 2);
        }
    }

    // WWII's and Advanced Warfare's shot cycle (Notes::ShotCycle; the games' ExtraNotes say the code): the
    // fields a shot's timing reads, the columns they make
    const char* const CycleColumns[] = { "fire_delay_ms", "bolt_action",   "rechamber_ms",     "rechamber_bolt_ms",
                                         "burst_delay_ms", "ammo_per_shot", "shot_interval_ms", "shot_rpm" };
    // the fire types by value (both exes' name tables): an attachment sets one by its value
    const char* const IwFireTypes[] = { "Full Auto", "Single Shot", "2-Round Burst", "3-Round Burst", "4-Round Burst", "5-Round Burst", "Double Barrel" };
    struct CycleIn
    {
        std::string WeaponType, FireType;
        double Fire = -1, Delay = -1, Rechamber = -1, RechamberBolt = -1, Burst = -1, Ammo = -1, Clip = -1;   // -1: not read
        int Bolt = -1;
        bool Akimbo = false;   // the second timer set: its delay and rechamber are not read
    };
    void ShotCycle(const CycleIn& In, std::map<std::string, std::string>& C)
    {
        int Rounds = 0;   // a 2- to 5-Round Burst's
        for (int K = 2; K <= 5; K++)
            if (In.FireType == IwFireTypes[K]) Rounds = K;
        if (In.Akimbo) return;
        if (In.Delay >= 0) C["fire_delay_ms"] = Num(In.Delay, 0);
        if (In.Bolt >= 0) C["bolt_action"] = In.Bolt ? "1" : "0";
        const bool Bolt = In.Bolt == 1;
        if (Bolt && In.Rechamber >= 0) C["rechamber_ms"] = Num(In.Rechamber, 0);
        if (Bolt && In.RechamberBolt >= 0) C["rechamber_bolt_ms"] = Num(In.RechamberBolt, 0);
        // (int)floor(cooldown + 0.5): the weapon time the burst's end sets
        const double Pause = Rounds && In.Burst >= 0 ? WeaponStatsTable::BurstDelayMs(In.Burst) : -1;
        if (Pause >= 0) C["burst_delay_ms"] = Num(Pause, 0);
        if (In.Ammo >= 0) C["ammo_per_shot"] = Num(In.Ammo, 0);
        // the interval within a magazine: only where the code traced gives it whole
        if (In.Fire <= 0 || In.Delay != 0 || (In.WeaponType != "bullet" && In.WeaponType != "projectile")) return;
        if (In.Clip >= 0 && In.Clip <= std::max(In.Ammo, 1.0)) return;   // one shot a magazine: a reload between shots
        double Interval = -1;
        if (In.FireType == "Full Auto" || In.FireType == "Single Shot")
        {
            if (Bolt && In.Rechamber < 0) return;
            Interval = In.Fire + (Bolt ? In.Rechamber : 0);
        }
        else if (Rounds && !Bolt && Pause >= 0)
            Interval = (Rounds * In.Fire + Pause) / Rounds;
        if (Interval <= 0) return;
        C["shot_interval_ms"] = Num(Interval, 2);
        C["shot_rpm"] = Num(std::round(60000.0 / Interval * 100) / 100, 2);
    }
    // the inputs from a weapon file's own fields (weapon_stats.csv: no attachment)
    CycleIn CycleOf(const Json& F, bool TruncateMs)
    {
        CycleIn In;
        const auto Field = [&](const char* Name, WeaponFiles::Unit How, double& Out) {
            if (double V = 0; Number(Cell(F, Name, How, TruncateMs), V)) Out = V;
        };
        In.WeaponType = Text(F, "weaponType"), In.FireType = Text(F, "fireType");
        Field("fireTime", Unit::Ms, In.Fire);
        Field("fireDelay", Unit::Ms, In.Delay);
        Field("rechamberTime", Unit::Ms, In.Rechamber);
        Field("rechamberBoltTime", Unit::Ms, In.RechamberBolt);
        Field("ammoUsedPerShot", Unit::Count, In.Ammo);
        Field("clipSize", Unit::Count, In.Clip);
        if (double V = 0; Get(F, "burstFireCooldown", V)) In.Burst = V;
        if (double V = 0; Get(F, "boltAction", V)) In.Bolt = V != 0 ? 1 : 0;
        return In;
    }

    // WWII's attachments (the s2. fields IwWeaponDefs writes from the zone's attachment assets): each a
    // list of field overrides (s2.overrides: offset, operation, value), the weapon's fields at those
    // offsets (s2.offsets), its default attachments and its reload animation overrides
    const char* const S2Format = "s2.format";
    const int S2Rank[] = { 0, 4, 1, 2, 3 };   // a type's place in the attachment list (s2_mp64 0xb6a2c8)
    void S2Values(const Json& F, const Json& Atts, const std::vector<std::string>& Chosen, std::map<std::string, std::string>& C,
                  std::vector<std::string>& Order)
    {
        const std::vector<std::string> List = Lines(Text(F, WeaponAttachmentList));
        const auto Type = [&](const std::string& N) { return Atts.contains(N) ? (int)Or(Atts[N], "attachmentType", -1) : -1; };
        const auto Index = [&](const std::string& N) {
            const auto It = std::find(List.begin(), List.end(), N);
            return It == List.end() ? 0 : (int)(It - List.begin()) + 1;
        };
        // (0x3b5960) the chosen ones, then the defaults: WeaponDef 0x50..0x53 for types 0, 2, 1, 3 when
        // none of that type is chosen, 0x54 always; in the game's order (type rank, then name)
        std::vector<std::string> All = Chosen;
        const std::vector<std::string> Defaults = Words(Text(F, "s2.defaults"));
        const int DefaultType[] = { 0, 2, 1, 3, -1 };
        for (size_t K = 0; K < Defaults.size() && K < std::size(DefaultType); K++)
        {
            const int D = std::atoi(Defaults[K].c_str());
            if (D <= 0 || D > (int)List.size()) continue;
            const std::string& N = List[(size_t)D - 1];
            if (DefaultType[K] >= 0 && std::any_of(Chosen.begin(), Chosen.end(), [&](const std::string& X) { return Type(X) == DefaultType[K]; }))
                continue;
            if (std::find(All.begin(), All.end(), N) == All.end()) All.push_back(N);
        }
        const auto Rank = [&](const std::string& N) {
            const int T = Type(N);
            return T >= 0 && T < (int)std::size(S2Rank) ? S2Rank[T] : 99;
        };
        std::sort(All.begin(), All.end(), [&](const std::string& A, const std::string& B) {
            return Rank(A) != Rank(B) ? Rank(A) < Rank(B) : A < B;
        });
        Order = All;
        // (0x3b6190) the primary mode's: all but a last underbarrel that has an alternate mode
        std::vector<std::string> Use = All;
        if (!Use.empty() && Type(Use.back()) == 1 && Or(Atts[Use.back()], "s2.alt", 0) != 0) Use.pop_back();
        std::erase_if(Use, [&](const std::string& N) { return !Atts.contains(N); });
        // (0x3b66b0, 0x3b6500) a field: the weapon's value at its offset, the first 'set' (3) of the list
        // replacing it, every other operation multiplying it
        std::map<std::string, std::string> Offsets;
        for (const auto& L : Lines(Text(F, "s2.offsets")))
            if (const auto W = Words(L); W.size() == 2) Offsets[W[0]] = W[1];
        const auto Value = [&](const std::string& Field, bool Time, bool Int, double& Out) {
            const auto It = Offsets.find(Field);
            if (It == Offsets.end() || !(Time ? Ms(F, Field, Out) : Get(F, Field, Out))) return false;
            bool Set = false;
            float Scale = 1;
            for (const auto& N : Use)
                for (const auto& L : Lines(Text(Atts[N], "s2.overrides")))
                {
                    const auto W = Words(L);
                    double X = 0;
                    if (W.size() != 3 || W[0] != It->second || !Number(W[2], X)) continue;
                    if (W[1] == "3")
                    {
                        if (!Set) Out = X, Set = true;
                    }
                    else
                        Scale *= (float)X;
                    break;
                }
            Out = Int ? std::trunc((float)Out * Scale) : (double)((float)Out * Scale);
            return true;
        };
        // (0x3c50f0) akimbo: an attachment whose name starts with akimbo; the getters then take the second
        // timer set (0x3bbf40 0x77c, 0x3c1510 0x7ac, 0x3c0ce0 0x7c8, 0x3c0ad0 0x7cc)
        const bool Akimbo = std::any_of(All.begin(), All.end(), [](const std::string& N) { return N.rfind("akimbo", 0) == 0; });
        const std::string Ak = Akimbo ? "Akimbo" : "";
        double V = 0;
        if (Value("clipSize", false, true, V)) C["clip_size"] = Num(V, 0);
        if (Value("fireTime" + Ak, true, true, V) && V != 0) Fire(C, V);
        // (0x3c5a80) reload, reload empty: the weapon's animation override for the state (0x16, 0x17)
        // matching the most of the list's attachments (by their place in the weapon's list), naming the
        // akimbo attachment when there is one (0x3c59c0); its time when not 0
        std::vector<int> Indices;
        for (const auto& N : All) Indices.push_back(Index(N));
        int AkIndex = 0;
        if (Akimbo)
            for (size_t K = 0; K < List.size() && !AkIndex; K++)
                if (List[K].rfind("akimbo", 0) == 0) AkIndex = (int)K + 1;
        const auto Anim = [&](int State, double& Time) {
            int Best = 0;
            double Found = 0;
            for (const auto& L : Lines(Text(F, "s2.animOverrides")))
            {
                const auto W = Words(L);
                if (W.size() < 5 || std::atoi(W[0].c_str()) == 1 || std::atoi(W[3].c_str()) != State) continue;
                const int A1 = std::atoi(W[1].c_str()), A2 = std::atoi(W[2].c_str());
                if (AkIndex != 0 && AkIndex != A1 && AkIndex != A2) continue;
                const auto In = [&](int A) { return A != 0 && std::find(Indices.begin(), Indices.end(), A) != Indices.end(); };
                const int Score = (In(A1) ? 1 : 0) + (In(A2) ? 1 : 0);
                if (Score > Best) Best = Score, Found = std::atof(W[4].c_str());
            }
            if (Found == 0) return false;
            Time = Found;
            return true;
        };
        if (Anim(0x16, V) || Value("reloadTime" + Ak, true, true, V)) C["reload_ms"] = Num(V, 0);
        if (Anim(0x17, V) || Value("reloadEmptyTime" + Ak, true, true, V)) C["reload_empty_ms"] = Num(V, 0);
        if (Value("reloadAddTime" + Ak, true, true, V)) C["reload_add_ms"] = Num(V, 0);
        // (0x3badf0) ADS: in, out
        if (Value("adsTransInTime", true, true, V)) C["ads_time_ms"] = Num(V, 0);
        if (Value("adsTransOutTime", true, true, V)) C["ads_out_time_ms"] = Num(V, 0);
        size_t Stop = 0;
        for (const auto& [Damage, Range] : Stops)
        {
            if (!F.contains(Damage)) continue;
            Stop++;
            if (Value(Range, false, false, V)) C[std::format("range_{}_m", Stop)] = Num(V * MetresPerInch, 2);
        }
        // the shot cycle (the weapons with its fields: the games' ExtraNotes say the code)
        if (!F.contains("boltAction")) return;
        const bool S1 = !Use.empty() && Text(Atts[Use.front()], "s2.game") == "s1";
        // the first override of a field's offset among the attachments held (Use's order): its line's words
        const auto FirstOverride = [&](const std::string& Field, bool SetOnly, bool TypeFourGoesOn) {
            std::vector<std::string> Found;
            const auto It = Offsets.find(Field);
            if (It == Offsets.end()) return Found;
            for (const auto& N : Use)
                for (const auto& L : Lines(Text(Atts[N], "s2.overrides")))
                {
                    const auto W = Words(L);
                    if (W.size() != 3 || W[0] != It->second || (SetOnly && W[1] != "3")) continue;
                    Found = W;
                    // (Advanced Warfare's flag getter 0x164590: a type 4 attachment's does not stop the search)
                    if (!TypeFourGoesOn || Or(Atts[N], "attachmentType", -1) != 4) return Found;
                    break;
                }
            return Found;
        };
        CycleIn In;
        In.WeaponType = Text(F, "weaponType"), In.Akimbo = Akimbo;
        // (0x3bdbe0 / 0x16cae0, the i32 getter) the fire type: the first attachment setting it
        In.FireType = Text(F, "fireType");
        if (const auto W = FirstOverride("fireType", true, false); !W.empty())
            if (const int T = std::atoi(W[2].c_str()); T >= 0 && T < (int)std::size(IwFireTypes)) In.FireType = IwFireTypes[T];
        if (Value("fireTime", true, true, V)) In.Fire = V;
        if (Value("fireDelay", true, true, V)) In.Delay = V;
        // (0x3be620 / 0x16d550, the flag getter: the first attachment holding an override of the offset gives
        // its i32 != 0)
        if (double B = 0; Get(F, "boltAction", B))
        {
            In.Bolt = B != 0 ? 1 : 0;
            if (const auto W = FirstOverride("boltAction", false, S1); !W.empty()) In.Bolt = std::atoi(W[2].c_str()) != 0 ? 1 : 0;
        }
        // (0x3c0a30 / 0x16f520) the rechamber: the animation override for state 6, else the field
        if (Anim(6, V) || Value("rechamberTime", true, true, V)) In.Rechamber = V;
        if (Value("rechamberBoltTime", true, true, V)) In.RechamberBolt = V;
        if (Value("burstFireCooldown", false, false, V)) In.Burst = V;
        // (0x3bb280, the i32 getter; Advanced Warfare's 0x1696b0 reads the WeaponDef's as is)
        if (S1 || Use.empty() ? Get(F, "ammoUsedPerShot", V) : Value("ammoUsedPerShot", false, true, V)) In.Ammo = V;
        if (double Clip = 0; Number(C["clip_size"], Clip)) In.Clip = Clip;
        C["fire_type"] = In.FireType;
        ShotCycle(In, C);
    }

    // Call of Duty Online's attachments (the codol. fields Games/CODOL writes from the zone's
    // weaponattach assets): each a list of field overrides (codol.overrides: value type, operation,
    // WeaponDef offset, f32 value), the weapon's fields at those offsets (codol.offsets); the weapon's
    // list of attachments from mp/coweaponattachmenttable.csv (CodolTable)
    const char* const CodolFormat = "codol.format";
    const char* const CodolTable = "mp/coweaponattachmenttable.csv";
    void CodolValues(const Json& F, const Json& Atts, const std::vector<std::string>& Chosen, std::map<std::string, std::string>& C,
                     std::vector<std::string>& Order)
    {
        Order = Chosen;
        std::map<std::string, std::string> Offsets;
        for (const auto& L : Lines(Text(F, "codol.offsets")))
            if (const auto W = Words(L); W.size() == 2) Offsets[W[0]] = W[1];
        // (0x42e520) the overrides of operations 6-9 of every attachment, by offset then operation (qsort
        // 0x42e4f0), each applied in turn (0x42dd70, 0x42dc60 i32, 0x42dcf0 f32) to the copy of the weapon's
        // WeaponDef: 6 sets the value, 7 adds it, 8 adds the weapon's own value times it, 9 multiplies;
        // a time (type 9) is in seconds (x 1000) and a speed (type 8) x 17.6 unless the operation is 8 or
        // 9; an i32 (types 4, 9) is truncated (ftol). Operations 0-5 touch no number field in these zones.
        const auto Value = [&](const std::string& Field, bool Time, double& Out) {
            const auto It = Offsets.find(Field);
            if (It == Offsets.end() || !(Time ? Ms(F, Field, Out) : Get(F, Field, Out))) return false;
            struct Entry
            {
                int Type, Op;
                double V;
            };
            std::vector<Entry> Es;
            for (const auto& N : Chosen)
                if (Atts.contains(N))
                    for (const auto& L : Lines(Text(Atts[N], "codol.overrides")))
                    {
                        const auto W = Words(L);
                        double X = 0;
                        if (W.size() != 4 || W[2] != It->second || !Number(W[3], X)) continue;
                        const int Type = std::atoi(W[0].c_str()), Op = std::atoi(W[1].c_str());
                        if (Op >= 6 && Op <= 9) Es.push_back({ Type, Op, X });
                    }
            std::stable_sort(Es.begin(), Es.end(), [](const Entry& A, const Entry& B) { return A.Op < B.Op; });
            const float Orig = (float)Out;
            float Cur = Orig;
            for (const auto& E : Es)
            {
                double V = (float)E.V;
                if ((E.Type == 9 || E.Type == 8) && E.Op != 8 && E.Op != 9) V = (float)(V * (E.Type == 9 ? 1000.0 : 17.6));
                const double R = E.Op == 6 ? V : E.Op == 7 ? Cur + V : E.Op == 8 ? Cur + Orig * V : Cur * V;
                Cur = E.Type == 4 || E.Type == 9 ? std::trunc((float)R) : (float)R;
            }
            Out = Cur;
            return true;
        };
        double V = 0;
        if (Value("clipSize", false, V)) C["clip_size"] = Num(V, 0);
        if (Value("fireTime", true, V) && V != 0) Fire(C, V);
        if (Value("reloadTime", true, V)) C["reload_ms"] = Num(V, 0);
        if (Value("reloadEmptyTime", true, V)) C["reload_empty_ms"] = Num(V, 0);
        if (Value("reloadAddTime", true, V)) C["reload_add_ms"] = Num(V, 0);
        if (Value("adsTransInTime", true, V)) C["ads_time_ms"] = Num(V, 0);
        if (Value("adsTransOutTime", true, V)) C["ads_out_time_ms"] = Num(V, 0);
        size_t Stop = 0;
        for (const auto& [Damage, Range] : Stops)
        {
            if (!F.contains(Damage)) continue;
            Stop++;
            if (Value(Range, false, V)) C[std::format("range_{}_m", Stop)] = Num(V * MetresPerInch, 2);
        }
    }

    // Infinite Warfare's attachments (the iw7. fields IwWeaponDefs writes from the zone's attachment
    // assets): blocks that replace the weapon's (the first attachment holding one) and scales multiplied
    // over the attachments, as the exe's accessors read them (iw7_flat VAs, the README)
    const char* const Iw7Format = "iw7.format";
    // a hit location field's column: locTorsoUpper -> hit_torso_upper
    std::string HitColumn(const std::string& Field)
    {
        std::string C = "hit";
        for (size_t I = 3; I < Field.size(); I++)
        {
            if (std::isupper((unsigned char)Field[I])) C += '_';
            C += (char)std::tolower((unsigned char)Field[I]);
        }
        return C;
    }
    // weapon_stats.csv's explosion and projectile columns (After): those an Infinite Warfare attachment's
    // projectile block replaces
    bool ProjectileColumn(const Column& Col)
    {
        const std::string_view N = Col.Name;
        return N.starts_with("explosion_") || N.starts_with("projectile_");
    }
    // the columns the iw7 rules add to weapon_attachments.csv (the hit locations: the weapon file's)
    std::vector<std::string> Iw7Columns()
    {
        std::vector<std::string> Out;
        for (size_t I = 1; I <= std::size(Stops); I++) Out.push_back(std::format("damage_{}", I));
        Out.push_back("start_ammo");
        Out.push_back("max_ammo");
        for (const auto& [Field, Column] : HitLocations)
            if (std::string(Column) != "hit_torso_mid") Out.push_back(Column);
        for (const char* C : { "hit_shield", "hit_armor", "hit_soft" }) Out.push_back(C);
        Out.push_back("weapon_type");
        Out.push_back("fire_type");
        for (const auto& Col : After)
            if (ProjectileColumn(Col)) Out.push_back(Col.Name);
        for (const auto& Col : WeaponFiles::Iw7ChargeColumns()) Out.push_back(Col.Name);
        for (const auto& Col : WeaponFiles::Iw7BurstBoltColumns()) Out.push_back(Col.Name);
        for (const auto& Col : WeaponFiles::Iw7AmmoColumns()) Out.push_back(Col.Name);
        return Out;
    }
    void Iw7Values(const Json& F, const Json& Atts, const std::vector<std::string>& Chosen, std::map<std::string, std::string>& C,
                   std::vector<std::string>& Order)
    {
        Order = Chosen;
        // (0x743cf0) the gather: the alternate mode's attachment (the weapon asset's list 0x58: iw7.altSlot) left
        // out of the primary mode unless its class (attachment 0x20) is none (14)
        const std::vector<std::string> Alt = Lines(Text(F, "iw7.altSlot"));
        std::vector<const Json*> Use;
        for (const auto& N : Chosen)
            if (Atts.contains(N) && (std::find(Alt.begin(), Alt.end(), N) == Alt.end() || Text(Atts[N], "iw7.class") == "none"))
                Use.push_back(&Atts[N]);
        // the first attachment holding a field's block
        const auto First = [&](const char* Field) -> const Json* {
            for (const Json* A : Use)
                if (A->contains(Field)) return A;
            return nullptr;
        };
        // (0x73d0f0) fire time: the weapon's fireTime, replaced by the first attachment's i32 0x1a8 that is not
        // 0 (not from one of kind 1, the alternate mode's), x every other attachment's f32 0x1b0, truncated
        if (double Base = 0; Ms(F, "fireTime", Base))
        {
            double Override = 0;
            float Scale = 1;
            for (const Json* A : Use)
            {
                const double O = Or(*A, "iw7.fireTime", 0);
                if (Override == 0 && O != 0 && Or(*A, "iw7.kind", 0) != 1)
                {
                    Override = O;
                    continue;
                }
                Scale *= (float)Or(*A, "iw7.fireTimeScale", 1);
            }
            const double T = std::trunc((float)(Override != 0 ? Override : Base) * Scale);
            if (T > 0) Fire(C, T);
        }
        // (0x73c620 clip, 0x74efa0 start, 0x74a890 max) the first attachment's ammo block (i32 max 0, start
        // 4, clip 8) replacing the weapon's, x every attachment's f32 0x198 (0: none), truncated
        {
            float Scale = 1;
            for (const Json* A : Use)
                if (const double S = Or(*A, "iw7.ammoScale", 0); S != 0) Scale *= (float)S;
            const Json* B = First("iw7.ammo");
            const std::vector<std::string> W = B ? Words(Text(*B, "iw7.ammo")) : std::vector<std::string>();
            const auto Ammo = [&](const char* Col, const char* Field, size_t K) {
                double V = 0;
                if (W.size() == 3) V = std::atof(W[K].c_str());
                else if (!Get(F, Field, V)) return;
                C[Col] = Num(std::trunc((float)V * Scale), 0);
            };
            Ammo("max_ammo", "maxAmmo", 0);
            Ammo("start_ammo", "startAmmo", 1);
            Ammo("clip_size", "clipSize", 2);
        }
        // (0x746af6 damage, 0x742570 ranges) the first attachment's damage table (5 damages, 4 ranges)
        // replacing the weapon's, the ranges x every attachment's f32 0x19c (0: none); the stops as the
        // damage code takes them: damage_1 up to range_1, each next stop whose range is above 0, the last
        // damage beyond
        {
            float Scale = 1;
            for (const Json* A : Use)
                if (const double S = Or(*A, "iw7.rangeScale", 0); S != 0) Scale *= (float)S;
            double D[5] = {}, R[4] = {};
            const Json* B = First("iw7.damage");
            const std::vector<std::string> W = B ? Words(Text(*B, "iw7.damage")) : std::vector<std::string>();
            if (W.size() == 9)
            {
                for (int K = 0; K < 5; K++) D[K] = std::atof(W[(size_t)K].c_str());
                for (int K = 0; K < 4; K++) R[K] = std::atof(W[(size_t)K + 5].c_str());
            }
            else
            {
                const char* Damages[] = { "damage", "damage2", "damage3", "damage4", "minDamage" };
                const char* Ranges[] = { "maxDamageRange", "damageRange2", "damageRange3", "damageRange4" };
                for (int K = 0; K < 5; K++) D[K] = Or(F, Damages[K], 0);
                for (int K = 0; K < 4; K++) R[K] = Or(F, Ranges[K], 0);
            }
            size_t Stop = 0;
            for (int K = 0; K < 4; K++)
            {
                const double Range = (float)R[K] * Scale;
                if (K > 0 && !(Range > 0)) continue;
                Stop++;
                C[std::format("damage_{}", Stop)] = Num(D[K], 0);
                C[std::format("range_{}_m", Stop)] = Num(Range * MetresPerInch, 2);
            }
            C[std::format("damage_{}", Stop + 1)] = Num(D[4], 0);
        }
        // (0x744ec0) the weapon type: the first attachment's i32 0x1c that is not 0 replacing weaponType; (0x73aa30
        // radius, 0x73cba0 inner and outer damage, 0x741800 speed) the first attachment's projectile block (pointer
        // 0x160: i32 0, 8, 0xc, 0x14) replacing explosionRadius, explosionInnerDamage, explosionOuterDamage,
        // projectileSpeed (explosionRadiusMin: no attachment field). The weapon's fields with those replaced, read
        // as weapon_stats.csv reads them (Cell); an attachment row's column written only when it changes the
        // value (the weapon alone: its own)
        {
            Json G = F;
            if (const Json* B = First("iw7.weaponType")) G["weaponType"] = Text(*B, "iw7.weaponType");
            if (const Json* B = First("iw7.projectile"))
                if (const auto W = Words(Text(*B, "iw7.projectile")); W.size() == 4)
                {
                    const char* Fields[] = { "explosionRadius", "explosionInnerDamage", "explosionOuterDamage", "projectileSpeed" };
                    for (size_t K = 0; K < 4; K++) G[Fields[K]] = W[K];
                }
            // (0x751b80, the first attachment whose charge block (pointer 0x168) is not null, for each getter:
            // 0x752620 type, 0x7521c0 gain, 0x752010 cost per shot, 0x7522e0 loss when idle, 0x7520a0 empty
            // cooldown) its block replacing chargeType 0xd08, chargeGain 0xd0c, chargeCostPerShot 0xd10,
            // chargeLossWhenIdle 0xd14, chargeEmptyCooldown 0xd18
            if (const Json* B = First("iw7.charge"))
                if (const auto W = Words(Text(*B, "iw7.charge")); W.size() == 4)
                {
                    G["chargeType"] = Text(*B, "iw7.chargeType");
                    const char* Fields[] = { "chargeGain", "chargeCostPerShot", "chargeLossWhenIdle", "chargeEmptyCooldown" };
                    for (size_t K = 0; K < 4; K++) G[Fields[K]] = W[K];
                }
            // (0x744770) the fire type: the first attachment whose ammo general block (pointer 0x88) is not null,
            // its i32 0xc replacing fireType 0x244; (0x7475f0) the bolt action: the first attachment whose general
            // block (pointer 0xc8) is not null, its u8 0 replacing boltAction 0xb83
            if (const Json* B = First("iw7.fireType")) G["fireType"] = Text(*B, "iw7.fireType");
            if (const Json* B = First("iw7.boltAction")) G["boltAction"] = Text(*B, "iw7.boltAction");
            // (0x73c1e0) the burst cooldown: the weapon's f32 0x33c, replaced by the first attachment's burst
            // block (pointer 0x188: i32 0) not of kind 1 (attachment i32 0x18) unless it is the weapon's
            // alternate slot's, x every other attachment's f32 0x1b4 (a 0 not skipped)
            if (double Base = 0; Get(F, "burstFireCooldown", Base))
            {
                float Value = (float)Base, Scale = 1;
                bool Replaced = false;
                for (const Json* A : Use)
                {
                    const bool Slot = std::any_of(Alt.begin(), Alt.end(), [&](const std::string& N) { return Atts.contains(N) && &Atts[N] == A; });
                    if (!Replaced && A->contains("iw7.burst") && (Or(*A, "iw7.kind", 0) != 1 || Slot))
                    {
                        Value = (float)Or(*A, "iw7.burst", 0);
                        Replaced = true;
                        continue;
                    }
                    Scale *= (float)Or(*A, "iw7.burstScale", 1);
                }
                G["burstFireCooldown"] = std::format("{}", Value * Scale);
            }
            // (0x737530) the ammo a shot takes from the clip: the first attachment's i32 0x1f0 above 0, else the
            // weapon's ammoUsedPerShot 0x4c0 (below 1: 1)
            for (const Json* A : Use)
                if (Or(*A, "iw7.ammoPerShot", 0) > 0)
                {
                    G["ammoUsedPerShot"] = Text(*A, "iw7.ammoPerShot");
                    break;
                }
            const auto Put = [&](const char* Col, const std::string& Base, const std::string& With) {
                if (Chosen.empty() || With != Base) C[Col] = With;
            };
            Put("weapon_type", Text(F, "weaponType"), Text(G, "weaponType"));
            Put("fire_type", Text(F, "fireType"), Text(G, "fireType"));
            for (const auto& Col : After)
                if (ProjectileColumn(Col)) Put(Col.Name, Cell(F, Col.Field, Col.How), Cell(G, Col.Field, Col.How));
            for (const auto& Col : WeaponFiles::Iw7ChargeColumns()) Put(Col.Name, Cell(F, Col.Field, Col.How), Cell(G, Col.Field, Col.How));
            // (the rechamber times: the animation package's, which no attachment block changes; the package
            // overrides an attachment may select (WeaponDef 0xe20) not applied, as for the reload times)
            for (const auto& Col : WeaponFiles::Iw7BurstBoltColumns()) Put(Col.Name, Cell(F, Col.Field, Col.How), Cell(G, Col.Field, Col.How));
            for (const auto& Col : WeaponFiles::Iw7AmmoColumns()) Put(Col.Name, Cell(F, Col.Field, Col.How), Cell(G, Col.Field, Col.How));
        }
        // (0x740120) the hit location table: the first attachment's replacing the weapon's
        if (const Json* B = First("iw7.hits"))
            for (const auto& L : Lines(Text(*B, "iw7.hits")))
            {
                if (const auto W = Words(L); W.size() == 2) C[HitColumn(W[0])] = Num(std::atof(W[1].c_str()), 4);
            }
        else
            for (const auto& Col : Iw7Columns())
                if (Col.rfind("hit_", 0) == 0)
                {
                    std::string Field = "loc";
                    bool Up = true;
                    for (size_t I = 4; I < Col.size(); I++)
                    {
                        if (Col[I] == '_') { Up = true; continue; }
                        Field += Up ? (char)std::toupper((unsigned char)Col[I]) : Col[I];
                        Up = false;
                    }
                    if (double V = 0; Get(F, Field, V)) C[Col] = Num(V, 4);
                }
    }

    // The combinations a game's class builder makes, as lists of attachments (Notes say whose rules)
    struct ComboTable
    {
        std::vector<std::vector<std::string>> Rows;
        std::map<std::string, size_t> Row;   // by the first cell
        explicit ComboTable(const std::string& Text)
        {
            for (const auto& L : Lines(Text))
            {
                std::vector<std::string> Cells;
                std::string Cell;
                for (const char Ch : L)
                    if (Ch == ',') Cells.push_back(Cell), Cell.clear();
                    else Cell += Ch;
                Cells.push_back(Cell);
                Row.try_emplace(Lower(Cells[0]), Rows.size());
                Rows.push_back(std::move(Cells));
            }
        }
        // tablelookup(table, 0, B, tablelookuprownum(table, 0, A)) == "no": the pair refused (-1: a name
        // the table does not hold)
        int Refuses(const std::string& A, const std::string& B) const
        {
            const auto Ia = Row.find(A), Ib = Row.find(B);
            if (Ia == Row.end() || Ib == Row.end()) return -1;
            const auto& R = Rows[Ib->second];
            return Ia->second < R.size() && Lower(R[Ia->second]) == "no" ? 1 : 0;
        }
    };
    // MW3 (maps/mp/gametypes/_class.gsc buildWeaponName, the class builder): two attachments, the pair
    // refused by mp/attachmentCombos.csv left out; a sniper rifle's own scope (<base>scope) added when
    // neither is a scope (rail) nor zoomscope; vzscope is <base>scopevz. The table's names: the script's
    // validateAttachment (reflexsmg: reflex...)
    std::vector<std::vector<std::string>> Iw5Combos(const std::string& Weapon, const Json& F, const Json& Atts, const ComboTable& Table, size_t& Unknown)
    {
        std::string Base = Weapon;
        if (Base.rfind("iw5_", 0) == 0) Base = Base.substr(4);
        if (Base.ends_with("_mp")) Base.resize(Base.size() - 3);
        const std::vector<std::string> List = Lines(Text(F, WeaponAttachmentList));
        const bool Sniper = Text(F, "weaponClass") == "sniper";
        const std::string Own = Sniper && std::find(List.begin(), List.end(), Base + "scope") != List.end() ? Base + "scope" : std::string();
        const auto TableName = [&](const std::string& N) -> std::string {
            if (N == Base + "scopevz") return "vzscope";
            if (Table.Row.count(N)) return N;
            static const std::map<std::string, std::string> Validate = { { "silencer03", "silencer" }, { "silencer02", "silencer" },
                                                                         { "m320", "gl" },             { "gp25", "gl" },
                                                                         { "reflexlmg", "reflex" },    { "reflexsmg", "reflex" },
                                                                         { "eotechlmg", "eotech" },    { "eotechsmg", "eotech" },
                                                                         { "acogsmg", "acog" },        { "thermalsmg", "thermal" } };
            const auto It = Validate.find(N);
            return It != Validate.end() ? It->second : N;
        };
        const auto Type = [&](const std::string& N) { return Atts.contains(N) ? Text(Atts[N], "attachmentType") : std::string(); };
        const auto Rail = [&](const std::string& N) { return Type(N) == "scope" || N == "zoomscope"; };
        std::vector<std::string> Pick;
        for (const auto& N : List)
            if (N != Own && Atts.contains(N)) Pick.push_back(N);
        std::vector<std::vector<std::string>> Out;
        const auto Add = [&](std::vector<std::string> Set) {
            if (!Own.empty() && std::none_of(Set.begin(), Set.end(), Rail)) Set.push_back(Own);
            if (Set.size() < 2) return;
            std::sort(Set.begin(), Set.end());   // the weapon name's order
            Out.push_back(std::move(Set));
        };
        for (size_t I = 0; I < Pick.size(); I++)
        {
            if (!Own.empty()) Add({ Pick[I] });
            for (size_t J = I + 1; J < Pick.size(); J++)
            {
                // the Weapon holds one scope and one underbarrel
                if (Type(Pick[I]) == Type(Pick[J]) && Type(Pick[I]) != "other") continue;
                const int R = Table.Refuses(TableName(Pick[I]), TableName(Pick[J]));
                if (R < 0) Unknown++;
                if (R != 0) continue;
                Add({ Pick[I], Pick[J] });
            }
        }
        return Out;
    }
    // WWII: two attachments of the weapon's list, the pair refused by mp/attachmentCombos_mtx12.csv
    // (the class editor's, ui/lui/mp_menus/s1cacutils) left out
    std::vector<std::vector<std::string>> S2Combos(const Json& F, const ComboTable& Table, size_t& Unknown)
    {
        const std::vector<std::string> List = Lines(Text(F, WeaponAttachmentList));
        std::vector<std::vector<std::string>> Out;
        for (size_t I = 0; I < List.size(); I++)
            for (size_t J = I + 1; J < List.size(); J++)
            {
                const int R = Table.Refuses(List[I], List[J]);
                if (R < 0) Unknown++;
                if (R == 0) Out.push_back({ List[I], List[J] });
            }
        return Out;
    }

    // weapon_attachments.csv: each weapon whose file lists attachments, first alone ("(none)"), then
    // with each attachment of its list the facts hold; the attachment's scales applied to the
    // weapon's values (times and clip rounded to the unit), or MW3's and WWII's rules. Rows: the
    // attachment rows. Combos: weapon_attachment_combos.csv (MW3, WWII: the class builder's pairs, by
    // the same rules), empty without the game's combination table.
    std::string AttachmentTable(const std::map<std::string, std::pair<Json, std::vector<std::string>>>& Weapons, const Json& Attachments,
                                const std::map<std::string, std::string>& Strings, const Json& Tables, size_t& Rows, size_t& Missing,
                                std::string& Combos, size_t& ComboRows, size_t& Unknown)
    {
        std::vector<std::string> Head = { "weapon", "attachment", "display_name", "attachment_type", "clip_size", "rpm", "fire_time_ms",
                                          "reload_ms", "reload_empty_ms", "reload_add_ms", "ads_time_ms", "ads_out_time_ms" };
        for (size_t I = 1; I <= std::size(Stops); I++) Head.push_back(std::format("range_{}_m", I));
        std::vector<std::string> ComboHead = { "weapon", "attachments" };
        ComboHead.insert(ComboHead.end(), Head.begin() + 4, Head.end());
        for (const char* S : { "clip_size_scale", "fire_time_scale", "reload_time_scale", "reload_empty_time_scale", "reload_add_time_scale",
                               "ads_time_scale", "ads_out_time_scale", "damage_range_scale", "ads_move_speed_scale" })
            Head.push_back(S);
        // Infinite Warfare's attachments change the damages, the ammo and the hit locations too
        const bool Iw7 = std::any_of(Attachments.begin(), Attachments.end(), [](const Json& A) { return A.contains(Iw7Format); });
        if (Iw7)
            for (const auto& Col : Iw7Columns()) Head.push_back(Col);
        // Black Ops III's attachments scale each stop's damage and range on their own (damageScale1-6,
        // damageRangeScale1-6): the stops' damages and those scales, only when an attachment has them
        const bool PerStop = std::any_of(Attachments.begin(), Attachments.end(), [](const Json& A) { return A.contains("damageScale1"); });
        if (PerStop)
        {
            if (!Iw7)   // Infinite Warfare's columns hold the stops' damages already
                for (size_t I = 1; I <= std::size(Stops); I++) Head.push_back(std::format("damage_{}", I));
            for (size_t I = 1; I <= std::size(Stops); I++) Head.push_back(std::format("damage_scale_{}", I));
            for (size_t I = 1; I <= std::size(Stops); I++) Head.push_back(std::format("range_scale_{}", I));
            Head.push_back("burst_delay_time_scale");
        }
        // WWII's and Advanced Warfare's shot cycle, with the attachment applied: a cell only where the
        // attachment changes the weapon's value (its "(none)" row gives them all)
        const bool Cycle = std::any_of(Attachments.begin(), Attachments.end(), [](const Json& A) { return A.contains(S2Format); });
        if (Cycle)
        {
            Head.push_back("fire_type");
            Head.insert(Head.end(), std::begin(CycleColumns), std::end(CycleColumns));
        }
        std::string Csv;
        for (size_t I = 0; I < Head.size(); I++) Csv += (I ? "," : "") + Head[I];
        Csv += "\n";
        const auto Value = [](const Json& F, const char* Field, double& V) {
            return F.contains(Field) && Number(F[Field].get<std::string>(), V);
        };
        const auto Format = [&](const Json* A) {
            return A && A->contains(Iw5Format)     ? 5
                   : A && A->contains(S2Format)    ? 2
                   : A && A->contains(CodolFormat) ? 3
                   : A && A->contains(Iw7Format)   ? 7
                                                   : 0;
        };
        // the games' combination tables, when a zone read holds one
        const auto TableOf = [&](const char* Name) {
            return Tables.contains(Name) ? std::make_unique<ComboTable>(Tables[Name].get<std::string>()) : std::unique_ptr<ComboTable>();
        };
        const auto Iw5Table = TableOf("mp/attachmentcombos.csv");
        const auto S2Table = TableOf("mp/attachmentcombos_mtx12.csv");
        for (const auto& [Name, Entry] : Weapons)
        {
            const Json& F = Entry.first["fields"];
            if (!F.contains(WeaponAttachmentList)) continue;
            std::vector<std::string> List = Lines(F[WeaponAttachmentList].get<std::string>());
            {
                std::vector<std::string> Split;
                for (const auto& L : List)
                    for (const auto& W : Words(L)) Split.push_back(W);
                List = std::move(Split);
            }
            if (List.empty()) continue;
            // the weapon's rules: its attachments' format (MW3's, WWII's), the first the facts hold
            int Game = 0;
            for (const auto& Att : List)
                if (Attachments.contains(Att) && (Game = Format(&Attachments[Att])) != 0) break;
            const auto Row = [&](const std::string& Att, const Json* A) {
                std::map<std::string, std::string> C;
                C["weapon"] = Name;
                C["attachment"] = A ? Att : "(none)";
                const auto Scale = [&](const char* Field) {
                    double S = 1;
                    return A && Value(*A, Field, S) ? S : 1.0;
                };
                if (A)
                {
                    const std::string Key = A->contains("displayName") ? Lower((*A)["displayName"].get<std::string>()) : std::string();
                    if (const auto It = Strings.find(Key); !Key.empty() && It != Strings.end()) C["display_name"] = It->second;
                    if (A->contains("attachmentType")) C["attachment_type"] = (*A)["attachmentType"].get<std::string>();
                    for (const auto& [Col, Field] : std::initializer_list<std::pair<const char*, const char*>>{
                             { "clip_size_scale", "clipSizeScale" },
                             { "fire_time_scale", "fireTimeScale" },
                             { "reload_time_scale", "reloadTimeScale" },
                             { "reload_empty_time_scale", "reloadEmptyTimeScale" },
                             { "reload_add_time_scale", "reloadAddTimeScale" },
                             { "ads_time_scale", "adsTransInTimeScale" },
                             { "ads_out_time_scale", "adsTransOutTimeScale" },
                             { "damage_range_scale", "damageRangeScale" },
                             { "ads_move_speed_scale", "adsMoveSpeedScale" } })
                        if (double S = 0; Value(*A, Field, S)) C[Col] = Num(S, 4);
                }
                std::vector<std::string> Order;
                if (Format(A) == 5)
                {
                    Iw5Values(F, Attachments, { Att }, C, Order);
                    for (const auto& [Col, Field] : std::initializer_list<std::pair<const char*, const char*>>{ { "clip_size_scale", "iw5.ammunitionScale" },
                                                                                                                { "fire_time_scale", "iw5.fireTimersScale" },
                                                                                                                { "damage_range_scale", "iw5.damageScale" },
                                                                                                                { "ads_time_scale", "iw5.adsSettingsScale" } })
                        if (double S = 0; Get(*A, Field, S)) C[Col] = Num(S, 4);
                }
                else if (Game == 2 && (A == nullptr || Format(A) == 2))
                {
                    S2Values(F, Attachments, A ? std::vector<std::string>{ Att } : std::vector<std::string>{}, C, Order);
                    // the shot cycle's cells the attachment leaves as the "(none)" row has them: empty
                    if (A)
                    {
                        std::map<std::string, std::string> Base;
                        std::vector<std::string> BaseOrder;
                        S2Values(F, Attachments, {}, Base, BaseOrder);
                        for (const char* Col : CycleColumns)
                            if (C[Col] == Base[Col]) C.erase(Col);
                        if (C["fire_type"] == Base["fire_type"]) C.erase("fire_type");
                    }
                }
                else if (Game == 3 && (A == nullptr || Format(A) == 3))
                    CodolValues(F, Attachments, A ? std::vector<std::string>{ Att } : std::vector<std::string>{}, C, Order);
                else if (Game == 7 && (A == nullptr || Format(A) == 7))
                {
                    Iw7Values(F, Attachments, A ? std::vector<std::string>{ Att } : std::vector<std::string>{}, C, Order);
                    if (A)
                        for (const auto& [Col, Field] : std::initializer_list<std::pair<const char*, const char*>>{
                                 { "clip_size_scale", "iw7.ammoScale" }, { "fire_time_scale", "iw7.fireTimeScale" }, { "damage_range_scale", "iw7.rangeScale" } })
                            if (double S = 0; Get(*A, Field, S)) C[Col] = Num(S, 4);
                }
                else
                {
                    double V = 0;
                    if (Value(F, "clipSize", V)) C["clip_size"] = Num(std::round(V * Scale("clipSizeScale")), 0);
                    const auto Time = [&](const char* Col, const char* Field, const char* ScaleField) {
                        if (double S = 0; Value(F, Field, S)) C[Col] = Num(std::round(S * 1000 * Scale(ScaleField)), 0);
                    };
                    Time("fire_time_ms", "fireTime", "fireTimeScale");
                    Time("reload_ms", "reloadTime", "reloadTimeScale");
                    Time("reload_empty_ms", "reloadEmptyTime", "reloadEmptyTimeScale");
                    Time("reload_add_ms", "reloadAddTime", "reloadAddTimeScale");
                    Time("ads_time_ms", "adsTransInTime", "adsTransInTimeScale");
                    Time("ads_out_time_ms", "adsTransOutTime", "adsTransOutTimeScale");
                    if (double Ms = 0; Number(C["fire_time_ms"], Ms) && Ms > 0) C["rpm"] = Num(std::round(60000.0 / Ms * 100) / 100, 2);
                    size_t Stop = 0;
                    for (size_t I = 0; I < std::size(Stops); I++)
                    {
                        const auto& [Damage, Range] = Stops[I];
                        if (!F.contains(Damage)) continue;
                        Stop++;
                        // the stop's own scales (Black Ops III: by its place among the six; 1 elsewhere)
                        const std::string DamageScale = std::format("damageScale{}", I + 1), RangeScale = std::format("damageRangeScale{}", I + 1);
                        if (double R = 0; Value(F, Range, R))
                            C[std::format("range_{}_m", Stop)] = Num(R * Scale("damageRangeScale") * Scale(RangeScale.c_str()) * MetresPerInch, 2);
                        if (!PerStop) continue;
                        // (int) of damage x scale, as BO3's damage function truncates each stop
                        if (double D = 0; Value(F, Damage, D)) C[std::format("damage_{}", Stop)] = Num(std::trunc(D * Scale(DamageScale.c_str())), 0);
                        if (A && A->contains(DamageScale)) C[std::format("damage_scale_{}", I + 1)] = Num(Scale(DamageScale.c_str()), 4);
                        if (A && A->contains(RangeScale)) C[std::format("range_scale_{}", I + 1)] = Num(Scale(RangeScale.c_str()), 4);
                    }
                    if (PerStop && A && A->contains("burstDelayTimeScale")) C["burst_delay_time_scale"] = Num(Scale("burstDelayTimeScale"), 4);
                }
                for (size_t I = 0; I < Head.size(); I++) Csv += (I ? "," : "") + CsvField(C[Head[I]]);
                Csv += "\n";
            };
            Row("", nullptr);
            for (const auto& Att : List)
            {
                if (!Attachments.contains(Att))
                {
                    if (Att != "defaultattachment") Missing++;   // the list's first entry, no attachment file
                    continue;
                }
                Row(Att, &Attachments[Att]);
                Rows++;
            }
            // the class builder's combinations
            std::vector<std::vector<std::string>> Sets;
            if (Game == 5 && Iw5Table) Sets = Iw5Combos(Name, F, Attachments, *Iw5Table, Unknown);
            if (Game == 2 && S2Table) Sets = S2Combos(F, *S2Table, Unknown);
            for (const auto& Set : Sets)
            {
                if (std::any_of(Set.begin(), Set.end(), [&](const std::string& N) { return !Attachments.contains(N); })) continue;
                std::map<std::string, std::string> C;
                std::vector<std::string> Order;
                if (Game == 5) Iw5Values(F, Attachments, Set, C, Order);
                else S2Values(F, Attachments, Set, C, Order);
                C["weapon"] = Name;
                std::string Names;
                for (const auto& N : Set) Names += (Names.empty() ? "" : "+") + N;
                C["attachments"] = Names;
                if (Combos.empty())
                {
                    for (size_t I = 0; I < ComboHead.size(); I++) Combos += (I ? "," : "") + ComboHead[I];
                    Combos += "\n";
                }
                for (size_t I = 0; I < ComboHead.size(); I++) Combos += (I ? "," : "") + CsvField(C[ComboHead[I]]);
                Combos += "\n";
                ComboRows++;
            }
        }
        return Csv;
    }

    bool Merge(const std::string& Dir, const WeaponFiles::Notes& N, size_t& Rows, std::string& Error)
    {
        std::vector<fs::path> Files;
        std::error_code Ec;
        for (const auto& E : fs::directory_iterator(fs::path(Dir) / "facts", Ec))
            if (E.path().extension() == ".json") Files.push_back(E.path());
        std::sort(Files.begin(), Files.end());
        std::map<std::string, std::string> Strings;
        WeaponStatsTable::Weapons Weapons;   // the first facts (a patch zone's first)
        std::vector<std::string> Zones;
        Json Attachments = Json::object();   // by name: the first facts' fields
        Json Packages = Json::object();      // the animation packages by name: the first facts' fields
        Json Tables = Json::object();        // the string tables kept (the combination tables) by name: the first
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
                // a patch zone's weapon wins (the game loads it after the zone it patches: patch_mp
                // after common_mp), else the first one read
                auto& Slot = Weapons[W["name"].get<std::string>()];
                if (Slot.second.empty() || (Patch(Zone) && std::none_of(Slot.second.begin(), Slot.second.end(), Patch))) Slot.first = W;
                Slot.second.push_back(Zone);
            }
            if (J.contains("attachments"))
                for (const auto& A : J["attachments"])
                    if (!Attachments.contains(A["name"].get<std::string>())) Attachments[A["name"].get<std::string>()] = A["fields"];
            if (J.contains("animpackages"))
                for (const auto& A : J["animpackages"])
                    if (!Packages.contains(A["name"].get<std::string>())) Packages[A["name"].get<std::string>()] = A["fields"];
            if (J.contains("tables"))
                for (const auto& [K, V] : J["tables"].items())
                    if (!Tables.contains(K)) Tables[K] = V;
        }
        // a weapon's times its file has not, from the animation package it names (another zone's)
        for (auto& [Name, Entry] : Weapons)
        {
            Json& F = Entry.first["fields"];
            if (!F.contains(WeaponFiles::AnimPackageField)) continue;
            const auto It = Packages.find(F[WeaponFiles::AnimPackageField].get<std::string>());
            if (It == Packages.end()) continue;
            for (const auto& [K, V] : It->items())
                if (!F.contains(K)) F[K] = V;
        }
        // Call of Duty Online: a weapon's attachments are the weaponattach assets its items name in
        // mp/coweaponattachmenttable.csv (weapon, item, attachment; "nop" none: a camo), in the table's order
        if (Tables.contains(CodolTable))
        {
            std::map<std::string, std::vector<std::string>> Of;
            for (const auto& L : Lines(Tables[CodolTable].get<std::string>()))
            {
                std::vector<std::string> Cells(1);
                for (const char Ch : L)
                    if (Ch == ',') Cells.emplace_back();
                    else Cells.back() += Ch;
                if (Cells.size() < 3 || Cells[2].empty() || Lower(Cells[2]) == "nop") continue;
                auto& List = Of[Lower(Cells[0])];
                if (std::find(List.begin(), List.end(), Cells[2]) == List.end()) List.push_back(Cells[2]);
            }
            for (auto& [Name, Entry] : Weapons)
            {
                Json& F = Entry.first["fields"];
                const auto It = Of.find(Lower(Name));
                if (!F.contains("codol.offsets") || F.contains(WeaponAttachmentList) || It == Of.end()) continue;
                std::string List;
                for (const auto& A : It->second) List += (List.empty() ? "" : ";") + A;
                F[WeaponAttachmentList] = List;
            }
        }

        std::vector<std::string> Head = { "weapon", "display_name", "mode", "upgrade_of", "type", "class", "fire_type", "rpm" };
        for (const auto& Col : Before) Head.push_back(Col.Name);
        for (size_t I = 1; I <= std::size(Stops); I++)
        {
            Head.push_back(std::format("damage_{}", I));
            Head.push_back(std::format("range_{}_m", I));
        }
        for (const auto& Col : After) Head.push_back(Col.Name);
        for (const auto& [Field, Name] : HitLocations) Head.push_back(Name);
        for (const auto& Col : N.Extra) Head.push_back(Col.Name);
        if (N.ShotCycle) Head.insert(Head.end(), std::begin(CycleColumns), std::end(CycleColumns));
        Head.push_back("zones");

        std::string Csv = WeaponStatsTable::Line(Head);
        Json Out = Json::array();
        for (const auto& [Name, Entry] : Weapons)
        {
            const Json& F = Entry.first["fields"];
            const auto Text = [&](const char* Field) { return F.contains(Field) ? F[Field].get<std::string>() : std::string(); };
            std::map<std::string, std::string> C;
            C["weapon"] = Name;
            if (const std::string Key = Lower(Text("displayName")); !Key.empty())
                if (const auto It = Strings.find(Key); It != Strings.end()) C["display_name"] = It->second;
            C["mode"] = WeaponStatsTable::Mode(Name, Entry.first, { "mp", "sp", "zm", "cp" });
            C["upgrade_of"] = WeaponStatsTable::UpgradeOf(Name, Weapons);
            C["type"] = Text("weaponType");
            C["class"] = Text("weaponClass");
            C["fire_type"] = Text("fireType");
            for (const auto& Col : Before) C[Col.Name] = Cell(F, Col.Field, Col.How, N.TruncateMs);
            if (double Ms = 0; Number(C["fire_time_ms"], Ms) && Ms > 0) C["rpm"] = Num(std::round(60000.0 / Ms * 100) / 100, 2);
            size_t Stop = 0;
            for (const auto& [Damage, Range] : Stops)
            {
                if (!F.contains(Damage)) continue;
                Stop++;
                C[std::format("damage_{}", Stop)] = Cell(F, Damage, Unit::Count);
                C[std::format("range_{}_m", Stop)] = Cell(F, Range, Unit::Inches);
            }
            for (const auto& Col : After) C[Col.Name] = Cell(F, Col.Field, Col.How, N.TruncateMs);
            for (const auto& [Field, Column] : HitLocations) C[Column] = Cell(F, Field, Unit::Scale);
            for (const auto& Col : N.Extra) C[Col.Name] = Cell(F, Col.Field, Col.How, N.TruncateMs);
            if (N.ShotCycle) ShotCycle(CycleOf(F, N.TruncateMs), C);
            C["zones"] = WeaponStatsTable::ZoneList(Entry.second);
            WeaponStatsTable::AddRow(Head, C, Csv, Out);
        }

        std::string Readme = N.Title + " weapon stats (GrayWolf --type weaponstats)\n\n"
            "One row per weapon asset (a mode's or an attachment's variant, ak47_mp, ak47_acog_mp, is its own\n"
            "weapon). The weapon files themselves are the weapons kind (--type weapons).\n\n"
            "Zones read so far (facts): ";
        for (size_t I = 0; I < Zones.size(); I++) Readme += (I ? ", " : "") + Zones[I];
        Readme += "\n" + N.Names + "\nWhere the values come from\n" + N.Source +
            "\nColumns and units\n"
            "- type, class, fire_type: weaponType, weaponClass, fireType, the game's names (\"bullet\", \"rifle\",\n"
            "  \"Full Auto\", \"3-Round Burst\"...). A field the weapon file does not name is left empty.\n"
            "- fire_time_ms: fireTime, milliseconds between two shots; rpm = 60000 / fire_time_ms.\n"
            "- shot_count: shotCount, pellets a shot (shotguns). clip_size, start_ammo, max_ammo: clipSize, startAmmo,\n"
            "  maxAmmo, as stored. ammo_count_clip_relative: ammoCountClipRelative (Black Ops, Black Ops II), 1 when\n"
            "  the ammo counts are magazines rather than rounds, as its name says (an AN-94: max_ammo 8 with it); not\n"
            "  read in this game's code.\n"
            "- reload_ms, reload_empty_ms: reloadTime, reloadEmptyTime (rounds left, empty magazine); reload_add_ms,\n"
            "  reload_empty_add_ms: reloadAddTime, reloadEmptyAddTime, when the ammo is added within them;\n"
            "  reload_start_ms, reload_end_ms: reloadStartTime, reloadEndTime (a segmented, round by round reload).\n"
            "- first_raise_ms, raise_ms, raise_quick_ms, drop_ms: firstRaiseTime, raiseTime, quickRaiseTime, dropTime.\n"
            "- sprint_out_ms: sprintOutTime, leaving a sprint (before the weapon can fire).\n"
            "- ads_time_ms, ads_out_time_ms: adsTransInTime, adsTransOutTime, aiming down the sights and back.\n"
            "- damage_<n> at range_<n>_m: the damage table, nearest stop first (ranges: inches x 0.0254). Black Ops II\n"
            "  has six stops (damage, damage2-5, minDamage at maxDamageRange, damageRange2-5, minDamageRange); the\n"
            "  others two: damage up to maxDamageRange, minDamage from minDamageRange (damage_1/range_1_m,\n"
            "  damage_2/range_2_m); CoD1 one, damage at any range (no range). A Black Ops II stop whose damage is 0\n"
            "  is unused. The interpolation between stops is not read here.\n"
            "- melee_damage: meleeDamage. explosion_radius_m, explosion_radius_min_m: explosionRadius,\n"
            "  explosionRadiusMin (inches x 0.0254); explosion_inner/outer_damage: explosionInnerDamage,\n"
            "  explosionOuterDamage (centre, edge).\n"
            "- projectile_speed_mps: projectileSpeed (inches a second x 0.0254), a projectile weapon's; 0 for bullets.\n"
            "- move_speed_scale, ads_move_speed_scale: moveSpeedScale, adsMoveSpeedScale, the player's speed holding\n"
            "  the weapon, x 1.\n"
            "- hit_<location>: the hit location multipliers (loc<Location>, locationDamageMultipliers), x damage.\n"
            "  hit_torso_mid is Black Ops II's only. Multiplayer modes may scale damage further in script.\n"
            "- mode: the weapon file's folder (CoD1, CoD2: weapons/mp, weapons/sp), else the name's last part when it\n"
            "  is one (mp, sp, zm). upgrade_of: a Pack-a-Punch weapon (<base>_upgraded_<mode>) names its base when\n"
            "  the table holds it.\n" + N.ExtraNotes;
        size_t AttRows = 0, AttMissing = 0, ComboRows = 0, Unknown = 0;
        std::string ComboCsv;
        const std::string AttCsv =
            Attachments.empty() ? std::string() : AttachmentTable(Weapons, Attachments, Strings, Tables, AttRows, AttMissing, ComboCsv, ComboRows, Unknown);
        const auto Has = [&](const char* Format) {
            return std::any_of(Attachments.begin(), Attachments.end(), [&](const Json& A) { return A.contains(Format); });
        };
        // the combinations file, as the README says it
        const auto CombosNote = [&](const std::string& Table, const std::string& Zone, const std::string& Rule) {
            if (ComboCsv.empty())
                return "\nweapon_attachment_combos.csv: not written, " + Table + " (" + Zone +
                       ") is in no zone read: export\nit into the same --out.\n";
            return "\nweapon_attachment_combos.csv: the combinations of attachments the class builder makes (" + std::to_string(ComboRows) +
                   " rows), each by the\nrules above, the attachments column joining them with + (MW3: in the weapon name's order; WWII: in\n"
                   "the weapon's list order). " + Rule + ". Pairs naming an attachment the table does not hold: " + std::to_string(Unknown) +
                   ", left out.\nThe game applies the same rules to any set of attachments: a set this file does not list is computed\n"
                   "the same way.\n";
        };
        if (Attachments.empty())
            Readme += "Not here: attachments (Black Ops II and MW3 keep them as their own assets: --type weapons writes them),\n"
                      "fireDelay, recoil, spread, every other field (in the weapon files).\n";
        else if (Has(Iw5Format))
            Readme += "Not here: fireDelay, recoil, spread, every other field (in the weapon files).\n\n"
                      "weapon_attachments.csv: each weapon whose file lists attachments (its attachments field), first alone\n"
                      "(attachment \"(none)\"), then with each attachment of the list, one at a time (" + std::to_string(AttRows) + " rows; " +
                      std::to_string(AttMissing) + " listed attachments\n"
                      "no zone read gives). An attachment is OpenAssetTools' attachment/<name>.json (weapons kind). MW3's\n"
                      "rules, read in iw5mp.exe (x64 RVAs; offsets by OAT's IW5 structs built for x64, which these functions\n"
                      "read field for field), for the weapon in its primary mode:\n"
                      "- The attachments it holds (0x4e6e0; the Weapon's bits: one scope, one underbarrel, the others a mask in\n"
                      "  the weapon's list order): up to three slots, the scope, then the others, then the underbarrel when it\n"
                      "  counts (a fourth takes the third slot). An underbarrel counts only when its weapClass is rifle (grip),\n"
                      "  or it has adsSettingsMain, or adsSettingsScaleMain is neither 0 nor 1 (the hybrid sights); the others\n"
                      "  (gl, gp25, m320, shotgun) are the alternate mode's and change nothing here.\n"
                      "- clip_size (0x4c470): the first slot's ammunition clipSize when one has an ammunition block, else the\n"
                      "  weapon's, x every slot's ammunitionScale (0 counts as 1), truncated (Extended Mags 1.5: M4A1 45, MP7 60).\n"
                      "- fire_time_ms (0x4cb20): the weapon's animOverrides \"fire\" time (0: none), else fireTime, x every slot's\n"
                      "  fireTimersScale, truncated (Rapid Fire 0.8); rpm from it.\n"
                      "- reload_ms (0x517b0), reload_empty_ms (0x51460): the animOverrides \"reload\" time (0: none) and\n"
                      "  \"reload_empty\" time (any entry found), else the weapon's; no attachment scale. The entry (0x53d90): of\n"
                      "  that kind, the one whose two attachments match the most of the weapon's (scope, underbarrel, others;\n"
                      "  each match counts one; the first of the best; none matching is none).\n"
                      "- reload_add_ms (0x51250): the weapon's reloadAddTime: its reloadOverrides (by attachment) are read only\n"
                      "  in the alternate mode (0x53c60 returns none in the primary one): the underbarrel shotgun's 550 ms.\n"
                      "- ads_time_ms, ads_out_time_ms (0x4b990): the first slot's adsTransInTime/OutTime (seconds; 0 gives\n"
                      "  300/500 ms) when one has the ADS block (adsSettingsMain for one sharing its ammo with the alternate\n"
                      "  mode, else adsSettings), else the weapon's; divided by every slot's adsSettingsScale above 0 (the exe\n"
                      "  keeps a rate, 1/time, multiplied by them; one sharing ammo: adsSettingsScaleMain when it has no\n"
                      "  adsSettingsMain).\n"
                      "- range_1_m, range_2_m (0x4c7a0): the first slot's damage block maxDamageRange/minDamageRange, else the\n"
                      "  weapon's, x every slot's damageScale / damageScaleMin (0 counts as 1; Suppressor 0.75: the ranges, not\n"
                      "  the damage).\n"
                      "- Akimbo: the attachment named akimbo makes the weapon dual wielded when given (0x1846b0: 0x538f0 looks\n"
                      "  for it among the slots; the scripts' _giveWeapon passes the same flag for a name holding _akimbo), and\n"
                      "  the getters then read the weapon's akimbo times instead (player state 0x390): fireTimeAkimbo x the\n"
                      "  slots' fireTimersScale (Akimbo's own 1.25: FMG9 60 x 1.25 = 75 ms, 800 rpm), reloadTimeAkimbo,\n"
                      "  reloadEmptyTimeAkimbo, reloadAddTimeAkimbo (akimboStateTimers), no animation override.\n"
                      "- The scale columns: the attachment's own ammunitionScale, fireTimersScale, damageScale, adsSettingsScale.\n"
                      "- Perks (Sleight of Hand, Proficiencies) are not applied.\n" +
                      CombosNote("mp/attachmentCombos.csv", "zone/<language>/code_post_gfx_mp",
                                 "The class builder (maps/mp/gametypes/\n"
                                 "_class.gsc buildWeaponName) takes two attachments; a pair is left out when mp/attachmentCombos.csv\n"
                                 "says \"no\" for it (the names as the script's validateAttachment gives them: reflexsmg is reflex), or\n"
                                 "the Weapon cannot hold it (two scopes, two underbarrels); a sniper rifle gets its own scope\n"
                                 "(<weapon>scope) as a third attachment when neither is a scope nor zoomscope (with a single one too)");
        else if (Has(S2Format) && std::any_of(Attachments.begin(), Attachments.end(), [](const Json& A) { return Text(A, "s2.game") == "s1"; }))
            Readme += "Not here: recoil, spread, every other field (in the weapon files).\n\n"
                      "weapon_attachments.csv: each weapon whose asset lists attachments, first alone (attachment \"(none)\"),\n"
                      "then with each attachment of its list, one at a time (" + std::to_string(AttRows) + " rows; " + std::to_string(AttMissing) +
                      " listed attachments no zone read\ngives: export common_mp into the same --out, it defines the shared ones). A weapon's list:\n"
                      "WeaponDef 0x60, its u8 count 0x524 (s1_mp64_flat RVAs: 0x133e67, 0x163fa0), then the Exo Zombies level\n"
                      "attachments the scripts give by name when the list does not name them (maps/mp/zombies/_wall_buys.gsc,\n"
                      "getattachment1forweaponlevel above level 3, 2 above 6, 3 above 9: an optic or the laser sight, then\n"
                      "lasersight, stock, quickdraw, firerate or xmags, then stock, quickdraw or xmags, by weapon). An\n"
                      "attachment is the zone's attachment asset: its type (attachment_type, i32 0x10), its display name (the\n"
                      "string at 8) and its list of field overrides: WeaponDef offsets (u16 at 0x48, sorted), each with a\n"
                      "16-byte entry (0x50: byte 2 the operation, the value at 8; count i32 at 0x58), the lookup 0x133c50\n"
                      "taking an offset's first entry. Advanced Warfare's rules, for the weapon in its primary mode:\n"
                      "- The attachments it holds (0x164320): the Weapon's set (a table entry of five attachments, by the\n"
                      "  weapon's bits 23-30), type 4 ones first, then the others, a last underbarrel (type 1) with an\n"
                      "  alternate mode (0x18 or byte 0x6f set) only in that mode.\n"
                      "- A field (0x1649d0 i32, 0x164760 f32, each taking the field's WeaponDef offset): the weapon's value,\n"
                      "  replaced by the first attachment that sets it (operation 3), multiplied by every other one's value\n"
                      "  (Rapid Fire fireTime x 0.8), truncated for an i32 (a type 4 attachment's product is applied at once\n"
                      "  and its set does not stop a later one: the same for one attachment).\n"
                      "- clip_size: clipSize (0x638). fire_time_ms: fireTime (0x668); rpm from it. reload_ms, reload_empty_ms,\n"
                      "  reload_add_ms: 0x68c, 0x694, 0x698. ads_time_ms, ads_out_time_ms: 0x988, 0x990. range_<n>_m: the\n"
                      "  damage ranges (0xc90, 0xc94). Each by the same rule. A set of attachments (a weapon level's two or\n"
                      "  three) is computed the same way: this file lists them one at a time.\n"
                      "- fire_type and the shot cycle columns (weapon_stats.csv's, their code in the columns above), each a\n"
                      "  cell only where the attachment changes the weapon's \"(none)\" value: fire_type (0x16cae0, i32 0x544:\n"
                      "  the first attachment setting it), fire_delay_ms (0x658), rechamber_ms (0x66c, or the weapon's animation\n"
                      "  override for state 6 matching the attachments, 0x172a00), rechamber_bolt_ms (0x674), burst_delay_ms\n"
                      "  (f32 0x550) by the same rule; bolt_action (0x164590: the first attachment holding an override of byte\n"
                      "  0xd7b gives its i32 != 0, a type 4 one not stopping the search); ammo_per_shot read as is (0xd54);\n"
                      "  shot_interval_ms, shot_rpm from them.\n"
                      "- Not applied: akimbo, the damages (no column here), perks and the scripts' level damage.\n"
                      "weapon_attachment_combos.csv: not written (no combination table read).\n";
        else if (Has(S2Format))
            Readme += "Not here: recoil, spread, every other field (in the weapon files).\n\n"
                      "weapon_attachments.csv: each weapon whose asset lists attachments, first alone (attachment \"(none)\":\n"
                      "its default attachments only), then with each attachment of its list, one at a time (" + std::to_string(AttRows) +
                      " rows). An\nattachment is the zone's attachment asset: its type (attachment_type: 0 sights, 1 underbarrel (bayonet,\n"
                      "grenade launcher...), 2 muzzle, 3 magazine, 4 the others, by the attachments of each), its display name\n"
                      "(the string at 8) and its list of field overrides: WeaponDef offsets (u16 at 0x58, sorted), each with a\n"
                      "16-byte entry (0x60: byte 1 the field's type, byte 2 the operation, the value at 8; count at 0x68).\n"
                      "WWII's rules, read in s2_mp64_flat.exe (RVAs), for the weapon in its primary mode:\n"
                      "- The attachments it holds (0x3b5960): the chosen ones and the weapon's defaults (WeaponDef 0x50, 0x51,\n"
                      "  0x52, 0x53: list indices for types 0, 2, 1, 3, added when none of that type is chosen; 0x54 always),\n"
                      "  ordered by type (0, 2, 3, 4, 1) then name. The primary mode leaves out a last underbarrel that has an\n"
                      "  alternate mode (0x3b6190, 0x3b8620: its 0x18 or byte 0x88 set: the grenade launchers).\n"
                      "- A field (0x3b66b0 i32, 0x3b6500 f32): the weapon's value, replaced by the first attachment that sets it\n"
                      "  (operation 3), multiplied by every other one's value (Extended Mag clipSize x 1.5, Rapid Fire fireTime\n"
                      "  x 0.94, Fast ADS ADS times x 0.84, Suppressor ranges x 0.7), truncated for an i32.\n"
                      "- clip_size: clipSize (0x608). fire_time_ms: fireTime (0x63c, 0x3bbf40); rpm from it. reload_add_ms:\n"
                      "  reloadAddTime (0x68c, 0x3c0ad0; its override table, 0x78, is the alternate mode's). ads_time_ms,\n"
                      "  ads_out_time_ms: 0x9d8 and 0x9e0, the ADS function's (0x3badf0, a rate 1/time) in and out times.\n"
                      "- reload_ms, reload_empty_ms (0x3c1510, 0x3c0ce0): reloadTime (0x66c), reloadEmptyTime (0x688), unless\n"
                      "  the weapon's animation override for the state (0x3c5a80: WeaponDef 0x60, 32-byte entries, count byte\n"
                      "  0x4d1; reload 0x16, empty 0x17) matching the most of the attachments (two list indices each) gives a\n"
                      "  time other than 0.\n"
                      "- range_<n>_m: the weapon's ranges (maxDamageRange 0xe1c, midDamageRange 0xe28, minDamageRange 0xe3c) by\n"
                      "  the same rule (Extended Range x 1.3).\n"
                      "- Akimbo: an attachment whose name starts with akimbo (0x3c50f0) makes the weapon dual wielded when given\n"
                      "  (0x5c5446; player state 0x628), and the getters then read the second timer set: fire 0x77c, reload\n"
                      "  0x7ac, reload empty 0x7c8, reload add 0x7cc (fireTimeAkimbo... in the weapon facts), by the same\n"
                      "  override rule (akimbo_fullauto's overrides are on the first set's offsets, which akimbo does not read).\n"
                      "  The shot cycle columns stay empty on an akimbo row (the second set's delay and rechamber not read).\n"
                      "- fire_type and the shot cycle columns (weapon_stats.csv's, their code in the columns above), each a\n"
                      "  cell only where the attachment changes the weapon's \"(none)\" value: fire_type (0x3bdbe0, i32 0x4f4:\n"
                      "  the first attachment setting it: Dragon's Breath, m30_rifle, the grenade launchers set Single Shot),\n"
                      "  fire_delay_ms (0x628), rechamber_ms (0x644, or the weapon's animation override for state 6, as the\n"
                      "  reload's), rechamber_bolt_ms (0x64c), burst_delay_ms (f32 0x500), ammo_per_shot (0xf34: Tribolt DP28\n"
                      "  3) by the same rule; bolt_action (0x3b63b0: the first attachment holding an override of byte 0xf84\n"
                      "  gives its i32 != 0); shot_interval_ms, shot_rpm from them.\n"
                      "- Not applied: the hit location overrides (Head Damage, its multipliers), the attachments a weapon\n"
                      "  variant adds (0x3b5b30, special_grip), perks and divisions.\n" +
                      CombosNote("mp/attachmentCombos_mtx12.csv", "code_post_gfx_mp",
                                 "The pairs of the\nweapon's list that mp/attachmentCombos_mtx12.csv (the table the class editor names,\n"
                                 "ui/lui/mp_menus/s1cacutils) does not refuse (\"no\"). A class holds up to four attachments a weapon\n"
                                 "(primaryAttachmentGUID1-4; how many is the weapon level's and the division's): the sets of three\n"
                                 "and four are not listed (a weapon lists up to 14 attachments: up to 1365 such sets each)");
        else if (Has(CodolFormat))
            Readme += "Not here: fireDelay, recoil, spread, every other field (in the weapon files).\n\n"
                      "weapon_attachments.csv: each weapon first alone (attachment \"(none)\"), then with each attachment its\n"
                      "items name, one at a time (" + std::to_string(AttRows) + " rows; " + std::to_string(AttMissing) +
                      " named attachments no zone read gives). A weapon's attachments:\n"
                      "mp/coweaponattachmenttable.csv (zone code_post_gfx_mp; the exe reads it at 0x6ec350: an item's\n"
                      "attachment, column 2 by column 1), the weaponattach assets its rows name for the weapon (column 0),\n"
                      "\"nop\" (camos, defaults) left out. An attachment is the zone's weaponattach asset: its name, a count\n"
                      "(4) and 12-byte overrides (8): byte 0 the value's type, byte 1 the operation, the WeaponDef offset\n"
                      "(u16 at 4), the value (f32 at 8). No display name nor type is in the asset (the item's are the\n"
                      "marketplace's): those columns stay empty. Call of Duty Online's rules, read in the decrypted\n"
                      "codoMP_client_shipRetail exe (VAs), for a weapon built with attachments (0x431810: a copy of the\n"
                      "WeaponDef, the attachments' overrides applied by 0x42e520, the ADS rates recomputed by 0x42d8d0):\n"
                      "- The overrides of operations 6 to 9, of every attachment, sorted by offset then operation, applied in\n"
                      "  turn (0x42dd70; 0x42dc60 an i32, 0x42dcf0 an f32): 6 sets the value, 7 adds it, 8 adds the weapon's\n"
                      "  own value times it, 9 multiplies. A time (type 9) is given in seconds (x 1000) and a speed (type 8)\n"
                      "  x 17.6, unless the operation is 8 or 9 (a scale); an i32 field (types 4, 9) is truncated.\n"
                      "  Operations 0-5 (set, the first of an offset winning) touch only strings, models and animations here.\n"
                      "- clip_size: clipSize (0xe68; Extended Mags: + clip x 0.5...). reload_ms, reload_empty_ms,\n"
                      "  reload_add_ms: 0x85c, 0x868, 0x870 (Fast Mags...). ads_time_ms, ads_out_time_ms: 0xc28, 0xc2c.\n"
                      "  range_<n>_m: the damage table's ranges (0xd28-0xd3c: Suppressor...). fire_time_ms: fireTime (0xe70),\n"
                      "  which no attachment of these zones changes.\n"
                      "- Not applied: the damage overrides (FMJ, High Caliber: 0xd10-0xd24, not columns of this file),\n"
                      "  akimbo (the dual wield times reloadTimeAkimbo..., named in the weapons kind's dumps: which attachment sets\n"
                      "  the dual wield state is not read), the marketplace's buffs and perks.\n"
                      "weapon_attachment_combos.csv: not written (no combination table: an item takes a slot of the weapon).\n";
        else if (Has(Iw7Format))
            Readme += "Not here: fireDelay, recoil, spread, every other field (in the weapon files).\n\n"
                      "weapon_attachments.csv: each weapon whose asset lists attachments, first alone (attachment \"(none)\"),\n"
                      "then with each attachment of its lists, one at a time (" + std::to_string(AttRows) + " rows; " + std::to_string(AttMissing) +
                      " listed attachments no zone\nread gives). A weapon's attachments: the weapon asset's lists (a pointer to attachment pointers,\n"
                      "its u32 count: 0x48 / 0x28, 0x50 / 0x2c, 0x60 / 0x34, 0x70 / 0x3c, then 0x58 / 0x30), the ones the\n"
                      "gather (0x743cf0) takes for the Weapon's slot bytes 0xd, 3, 5, the others' bits (u16 6) and slot 4\n"
                      "(last outside its alternate mode); the cosmetics' list (0x68 / 0x38, slot 0xc: cos_*) is left out. The\n"
                      "Zombies Pack-a-Punch levels are attachments of the first list (<weapon>pap1, <weapon>pap2: the script's\n"
                      "return_pap_attachment), Bang Bangs one of the others (doubletap). Infinite Warfare's rules, read in\n"
                      "iw7_flat.exe (VAs), for the weapon in its primary mode, each accessor taking the gathered attachments:\n"
                      "- The gather leaves the alternate mode's attachment (list 0x58, slot 4: the underbarrel launchers,\n"
                      "  glsmoke...) out of the primary mode unless its class (attachment i32 0x20) is none (14): such a row\n"
                      "  is the weapon's own.\n"
                      "- fire_time_ms (0x73d0f0): fireTime (WeaponDef 0x1dc), replaced by the first attachment whose i32\n"
                      "  0x1a8 is not 0 (not one of kind 1, attachment 0x18: the alternate mode's), x every other attachment's\n"
                      "  f32 0x1b0, truncated (Bang Bangs 0.67: rpm x 1.49); rpm from it. Not applied: the spin-up (WeaponDef\n"
                      "  0xcfc, 0xd04: 0 on the guns read) and the two further scales (0x73b380: WeaponDef 0x628, 0 on every\n"
                      "  gun, or an attachment's 0x100 / 0x108 block; 0x73d580).\n"
                      "- clip_size, start_ammo, max_ammo (0x73c620, 0x74efa0, 0x74a890): the first attachment's ammo block\n"
                      "  (pointer 0xe8: i32 max 0, start 4, clip 8) replacing the weapon's (clipSize 0xbc in the weapon asset,\n"
                      "  startAmmo 0x48c, maxAmmo 0x4a4), x every attachment's f32 0x198 (0 none), truncated (NV4 Pack-a-Punch:\n"
                      "  40 then 50 rounds).\n"
                      "- damage_<n>, range_<n>_m (0x746af6, 0x742570): the first attachment's damage table (pointer 0xf0: i32\n"
                      "  damages 0-0x10, f32 ranges 0x18-0x24) replacing the weapon's five damages and four ranges, the ranges\n"
                      "  x every attachment's f32 0x19c (0 none; not applied: the further scale 0x73b1b0); the stops as the\n"
                      "  damage code takes them (a step, no fall-off): damage_1 up to range_1_m, each next stop whose range is\n"
                      "  above 0, the last damage beyond (NV4 zm 130 / 50, pap1 220 / 120, pap2 300 / 220, at 25.4 m). The\n"
                      "  second table (player_damage_<n>, the block's 0x2c-0x3c) is not a column here.\n"
                      "- hit_<location> (0x740120): the first attachment's hit location table (pointer 0x190, 22 f32)\n"
                      "  replacing the weapon's (pap1 head x 4, body x 1.25; pap2 head x 5, body x 2).\n"
                      "- weapon_type (0x744ec0): weaponType (WeaponDef 0x22c), replaced by the first attachment whose i32 0x1c\n"
                      "  is not 0 (named by the same table). explosion_radius_m (0x73aa30), explosion_inner_damage,\n"
                      "  explosion_outer_damage (0x73cba0), projectile_speed_mps (0x741800): explosionRadius 0x670,\n"
                      "  explosionInnerDamage 0x67c, explosionOuterDamage 0x680, projectileSpeed 0x690, replaced by the first\n"
                      "  attachment's projectile block (pointer 0x160: i32 radius 0, inner 8, outer 0xc, speed 0x14), in\n"
                      "  weapon_stats.csv's units. An attachment's cell is written only when it changes the value (empty: the\n"
                      "  weapon's, given in its \"(none)\" row); explosion_radius_min_m (0x674) has no attachment field: the\n"
                      "  weapon's always (the starting pistol's Pack-a-Punch levels, g18pap1 / g18pap2, make it a projectile).\n"
                      "- fire_type (0x744770): fireType (WeaponDef 0x244), replaced by the first attachment's ammo general block\n"
                      "  (pointer 0x88: i32 0xc; m8pap1 and m8pap2 make the Burst iw7_m8_zml_charge Full Auto, firetypeauto\n"
                      "  the pistol).\n"
                      "  burst_delay_ms (0x73c1e0): burstFireCooldown (WeaponDef f32 0x33c), replaced by the first\n"
                      "  attachment's burst block (pointer 0x188: i32 0; not of kind 1 unless it is the weapon's alternate\n"
                      "  slot's), x every other attachment's f32 0x1b4 (0.8 mod_burst_cooldown, 0.66 the arkpink stones),\n"
                      "  empty unless fire_type (with the attachment) is Burst. bolt_action (0x7475f0): boltAction (WeaponDef\n"
                      "  u8 0xb83), replaced by the first attachment's general block (pointer 0xc8: u8 0); the rechamber_*\n"
                      "  times are the weapon's animation package's (no attachment block changes them), empty unless\n"
                      "  bolt_action is 1. The charge_* columns: the first attachment's charge block (pointer 0x168).\n"
                      "- ammo_per_shot (0x737530): the first attachment's i32 0x1f0 above 0, else ammoUsedPerShot (WeaponDef\n"
                      "  0x4c0), 1 when below 1 (doubletap makes the VPR, iw7_crdb, 2 a shot, take 1).\n"
                      "- The scale columns: the attachment's own 0x198 (clip_size_scale), 0x1b0 (fire_time_scale), 0x19c\n"
                      "  (damage_range_scale). Not applied: the reload and other anim time scales (0x1d4-0x1dc, Bang Bangs\n"
                      "  1.25 / 1.25 / 1.2), akimbo (0x1ac, the akimbo fire time), perks.\n"
                      "weapon_attachment_combos.csv: not written (a set of attachments is computed by the same rules).\n";
        else
            Readme += "Not here: fireDelay, recoil, spread, every other field (in the weapon files).\n\n"
                      "weapon_attachments.csv: each weapon whose file lists attachments (its attachments field), first alone\n"
                      "(attachment \"(none)\"), then with each attachment of the list, one at a time (" + std::to_string(AttRows) + " rows; " +
                      std::to_string(AttMissing) + " listed attachments\n"
                      "no zone read gives). An attachment is its attachment file (attachment/<name> in the weapons kind):\n"
                      "- display_name: its displayName's localized string; attachment_type: attachmentType.\n"
                      "- clip_size = clipSize x clipSizeScale, rounded; fire_time_ms = fireTime x fireTimeScale, rpm from it;\n"
                      "  reload_ms, reload_empty_ms, reload_add_ms: x reloadTimeScale, reloadEmptyTimeScale, reloadAddTimeScale;\n"
                      "  ads_time_ms, ads_out_time_ms: x adsTransInTimeScale, adsTransOutTimeScale; range_<n>_m: the weapon's\n"
                      "  damage ranges x damageRangeScale (the damages are the weapon's). The scale columns: the attachment's\n"
                      "  own values (ads_move_speed_scale is the attachment's adsMoveSpeedScale, not applied).\n"
                      "  Applied as the fields' names say (a multiplier on the weapon's field), not read in the game's code;\n"
                      "  clip sizes checked against published numbers (README of GrayWolf, Data dumps).\n"
                      "- A combination of attachments, their attachment unique (attachmentunique/au_<weapon>_<att>: hit\n"
                      "  multipliers, models) and the perks an attachment gives (FMJ) are not applied.\n";

        std::error_code E2;
        fs::create_directories(Dir, E2);
        const std::string JsonText = Out.dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
        const bool Ok = ExportWriters::WriteBytes(fs::path(Dir) / "weapon_stats.csv", (const uint8_t*)Csv.data(), Csv.size()) &&
                        ExportWriters::WriteBytes(fs::path(Dir) / "weapon_stats.json", (const uint8_t*)JsonText.data(), JsonText.size()) &&
                        ExportWriters::WriteBytes(fs::path(Dir) / "README.txt", (const uint8_t*)Readme.data(), Readme.size()) &&
                        (AttCsv.empty() || ExportWriters::WriteBytes(fs::path(Dir) / "weapon_attachments.csv", (const uint8_t*)AttCsv.data(), AttCsv.size())) &&
                        (ComboCsv.empty() ||
                         ExportWriters::WriteBytes(fs::path(Dir) / "weapon_attachment_combos.csv", (const uint8_t*)ComboCsv.data(), ComboCsv.size()));
        if (!Ok) Error = "write failed in " + Dir;
        Rows = Weapons.size();
        return Ok;
    }
}

namespace WeaponFiles
{
    std::map<std::string, std::string> Parse(const std::string& Text)
    {
        std::map<std::string, std::string> Fields;
        std::vector<std::string> Parts;
        size_t From = 0;
        for (;;)
        {
            const size_t At = Text.find('\\', From);
            Parts.push_back(Text.substr(From, At == std::string::npos ? std::string::npos : At - From));
            if (At == std::string::npos) break;
            From = At + 1;
        }
        for (size_t I = 1; I + 1 < Parts.size(); I += 2) Fields[Parts[I]] = Parts[I + 1];
        return Fields;
    }

    std::string Clean(std::string Value)
    {
        for (char& C : Value)
            if (C == '\\' || C == '\n' || C == '\r') C = ' ';
        return Value;
    }

    bool IsWeaponFile(const std::string& Text) { return Text.rfind("WEAPONFILE", 0) == 0; }
    bool IsAttachmentFile(const std::string& Text) { return Text.rfind("ATTACHMENTFILE", 0) == 0; }
    const char* const AnimPackageField = "animPackage";
    bool IsAnimPackageFile(const std::string& Text) { return Text.rfind("ANIMPACKAGE", 0) == 0; }
    const std::vector<Column>& Iw7ChargeColumns()
    {
        static const std::vector<Column> Cols = { { "charge_type", "chargeType", Unit::Text },
                                                  { "charge_time_ms", "chargeGain", Unit::Ms },
                                                  { "charge_cost_per_shot", "chargeCostPerShot", Unit::Scale },
                                                  { "charge_shots", "chargeCostPerShot", Unit::ChargeShots },
                                                  { "charge_drain_ms", "chargeLossWhenIdle", Unit::Ms },
                                                  { "charge_empty_cooldown_ms", "chargeEmptyCooldown", Unit::Ms } };
        return Cols;
    }
    const std::vector<Column>& Iw7BurstBoltColumns()
    {
        static const std::vector<Column> Cols = { { "burst_delay_ms", "burstFireCooldown", Unit::BurstMs },
                                                  { "bolt_action", "boltAction", Unit::Count },
                                                  { "rechamber_ms", "rechamberTime", Unit::BoltMs },
                                                  { "rechamber_one_handed_ms", "rechamberTimeOneHanded", Unit::BoltMs },
                                                  { "rechamber_bolt_ms", "rechamberBoltTime", Unit::BoltMs } };
        return Cols;
    }
    const std::vector<Column>& Iw7AmmoColumns()
    {
        static const std::vector<Column> Cols = { { "ammo_per_shot", "ammoUsedPerShot", Unit::PerShot } };
        return Cols;
    }

    void ExportStats(const std::vector<Weapon>& Weapons, const std::vector<std::pair<std::string, std::string>>& Strings,
                     const std::string& Zone, const DiskGame::Context& C, DiskGame::Totals& T, const Notes& N,
                     const std::map<std::string, std::string>& Tables)
    {
        using DiskGame::Kind;
        // the fields the table reads, kept as the weapon file has them
        std::set<std::string> Kept(std::begin(Texts), std::end(Texts));
        for (const auto& Col : Before) Kept.insert(Col.Field);
        for (const auto& Col : After) Kept.insert(Col.Field);
        for (const auto& [D, R] : Stops) Kept.insert(D), Kept.insert(R);
        for (const auto& [F, Name] : HitLocations) Kept.insert(F);
        for (const auto& Col : N.Extra) Kept.insert(Col.Field);
        if (N.ShotCycle)
            for (const char* Field : { "fireDelay", "boltAction", "rechamberTime", "rechamberBoltTime", "burstFireCooldown", "ammoUsedPerShot" })
                Kept.insert(Field);
        // a weapon's attachment list only beside the attachment files it names (Black Ops II: both in
        // the zone); MW3's weapon files hold the field with no attachment file to join
        // (Advanced Warfare's, WWII's and Infinite Warfare's lists: the weapon asset's own, kept in any zone,
        // the attachments another zone may define)
        if (std::any_of(Weapons.begin(), Weapons.end(), [](const Weapon& W) {
                return IsAttachmentFile(W.Text) || W.Text.find("\\s2.offsets\\") != std::string::npos || W.Text.find("\\iw7.lists\\") != std::string::npos;
            }))
            Kept.insert(WeaponAttachmentList);
        // MW3's: the weapon file's animation overrides too (their times for an attachment)
        if (std::any_of(Weapons.begin(), Weapons.end(), [](const Weapon& W) { return IsAttachmentFile(W.Text) && W.Text.find(Iw5Format) != std::string::npos; }))
            Kept.insert("animOverrides");
        // MW3's and WWII's: the akimbo times (an akimbo attachment's), WWII's s2. fields (its weapons' rules)
        const bool Rules = std::any_of(Weapons.begin(), Weapons.end(), [](const Weapon& W) {
            return IsAttachmentFile(W.Text) && (W.Text.find(Iw5Format) != std::string::npos || W.Text.find(S2Format) != std::string::npos);
        });
        if (Rules) Kept.insert(std::begin(AkimboFields), std::end(AkimboFields));
        const std::set<std::string> AttachmentKept(std::begin(AttachmentFields), std::end(AttachmentFields));
        if (N.AnimPackages) Kept.insert(AnimPackageField);
        Json List = Json::array(), Attachments = Json::array(), Packages = Json::array();
        for (const auto& W : Weapons)
        {
            // an animation package: its times, for the weapons (of any zone) naming it
            if (N.AnimPackages && IsAnimPackageFile(W.Text))
            {
                Json Fields = Json::object();
                for (auto& [K, V] : Parse(W.Text))
                    if (Kept.count(K)) Fields[K] = V;
                Packages.push_back(Json{ { "name", W.Name }, { "fields", std::move(Fields) } });
                continue;
            }
            // an attachment: every one the zone holds (a weapon the filter keeps may list it)
            if (IsAttachmentFile(W.Text))
            {
                Json Fields = Json::object();
                for (auto& [K, V] : Parse(W.Text))
                    if (AttachmentKept.count(K) || K.rfind("iw5.", 0) == 0 || K.rfind("s2.", 0) == 0 || K.rfind("codol.", 0) == 0 ||
                        K.rfind("iw7.", 0) == 0)
                        Fields[K] = V;
                Attachments.push_back(Json{ { "name", W.Name }, { "fields", std::move(Fields) } });
                continue;
            }
            if (!IsWeaponFile(W.Text) || !C.Wants(Kind::WeaponStats, W.Name)) continue;
            Json Fields = Json::object();
            for (auto& [K, V] : Parse(W.Text))
                // (Call of Duty Online's codol. fields: the offsets its attachment rules address)
                if (Kept.count(K) || (Rules && K.rfind("s2.", 0) == 0) || K.rfind("codol.", 0) == 0 || K == "iw7.altSlot") Fields[K] = V;
            Json Entry{ { "name", W.Name } };
            if (!W.Mode.empty()) Entry["mode"] = W.Mode;
            Entry["fields"] = std::move(Fields);
            List.push_back(std::move(Entry));
        }
        T.Kinds[Kind::WeaponStats].Seen += List.size();
        Json StringRows = Json::array();
        for (const auto& [K, V] : Strings)
            if (!K.empty() && !V.empty()) StringRows.push_back(Json::array({ Lower(K), V }));
        if (List.empty() && StringRows.empty() && Packages.empty() && Tables.empty()) return;
        Json Facts;
        Facts["zone"] = Zone;
        Facts["weapons"] = std::move(List);
        Facts["strings"] = std::move(StringRows);
        if (!Attachments.empty()) Facts["attachments"] = std::move(Attachments);
        if (!Packages.empty()) Facts["animpackages"] = std::move(Packages);
        if (!Tables.empty()) Facts["tables"] = Tables;
        const std::string Text = Facts.dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
        WeaponStatsTable::WriteFacts(Zone, Text, C, T, [N](const std::string& Dir, size_t& Rows, std::string& Error) {
            return Merge(Dir, N, Rows, Error);
        });
    }
}
