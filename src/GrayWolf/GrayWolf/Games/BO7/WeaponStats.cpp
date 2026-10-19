#include "stdafx.h"

// Black Ops 7's weapon stats (WeaponStats.h). Every offset and formula below is proven from the
// exe (bo7_flat.exe RVAs) in graywolf-tools fields.md; what is not proven stays out of the table.

#include "Games/BO7/WeaponStats.h"

#include "ExportNaming.h"
#include "ExportWriters.h"
#include "GameBlackOps7.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{
    using Json = nlohmann::ordered_json;
    using ExportNaming::At;
    constexpr uint64_t IdBits = 0x7FFFFFFFFFFFFFFFull;

    std::string Hex(uint64_t H) { return std::format("{:016x}", H); }

    // weapon_def u8 0x1afb, the class weaponclass (0x74e41b0 -> 0x46542a0) names through the
    // table at 0xec874d0
    const char* const ClassNames[] = { "none", "rifle", "sniper", "mg", "smg", "spread", "pistol", "grenade",
                                       "rocketlauncher", "turret", "throwingknife", "non-player", "item", "beam", "ball", "fist" };
    // the hit locations in the order of the game's table at 0xb8f0620 (index 0, none, left out)
    const char* const HitLocations[] = { "none", "helmet", "head", "neck", "torso_upper", "torso_lower", "right_arm_upper",
                                         "left_arm_upper", "right_arm_lower", "left_arm_lower", "right_hand", "left_hand",
                                         "right_leg_upper", "left_leg_upper", "right_leg_lower", "left_leg_lower", "right_foot",
                                         "left_foot", "gun", "shield", "armor", "soft" };
    // gametype settings keys (scriptbundle): the tunables tag 0x4611a20 hands 0x460f300, and the suit
    // 0x4528930 maps to the player's suit
    constexpr uint64_t TagKey = 0x65811ecb02190e17ull, SuitKey = 0x9465580f7953cea8ull;
    // an attachment's info bundle (type 76db25c2b8532150): its attachment asset and display name (a localize hash)
    constexpr uint64_t AttachmentKey = 0x3be72d7e1d0b16e5ull, NameKey = 0xc4bcadba8e631b86ull;
    constexpr float InchesPerMetre = 39.37f;   // 0xb432ef0
    constexpr float BaseSpeed = 190.0f;        // 0xb3dfed8, the speed the weapon stats UI (0x4ced840) starts from

    uint64_t Fnv64(const std::string& S)
    {
        uint64_t H = 0xcbf29ce484222325ull;
        for (const char C : S) H = (H ^ (uint8_t)C) * 0x100000001b3ull;
        return H & IdBits;
    }

    // a tunables tag's name: the tags are FNV-1a 64 of a name (0x460f300 compares them to the
    // settings' value); the names seen in the zones
    std::string TagName(uint64_t Tag)
    {
        static const std::map<uint64_t, std::string> Known = [] {
            std::map<uint64_t, std::string> M;
            for (const char* N : { "default", "t10_br", "t10_zm", "t10_mp", "t10_ob", "t10_cp" }) M[Fnv64(N)] = N;
            return M;
        }();
        const auto It = Known.find(Tag & IdBits);
        return It == Known.end() ? Hex(Tag & IdBits) : It->second;
    }

    // ---- attachments: one attachment's (or a loadout's) effect on the proven stats
    using Map = nlohmann::json;

    const char* const AttachmentReadme =
        "\nweapon_attachments.csv: each weapon's default set with one attachment (every slot's list of\n"
        "weapon +0x80 + slot * 0x10, slot and index as 0x46843b0 reads them). weapon_loadouts.csv: the\n"
        "lines of loadouts.txt in this folder ('<weapon asset>: <attachment>, <attachment>...', an attachment by\n"
        "asset or display name), each loadout of the default set. How the attachments combine (the code):\n"
        "- clip_size: the i32 at 8 of an attachment's block 0x400 when >= 0 (0x4637e2c), else\n"
        "  (int)(clip x (1 + sum(attachment 0xb90 - 1))) (0x4637d88).\n"
        "- reload_ms, reload_empty_ms, the sprint-out times: the animation package of the override entry\n"
        "  (weapon_def 0x60) with the most condition groups whose groups all match (a group: any of its\n"
        "  conditions {slot, mask of slot indexes}; a group tied to a blueprint variant never matches here) and\n"
        "  whose time is not 0xffff, else the base package (0x46556b0, 0x464aab0, 0x46843b0). Reload modifiers\n"
        "  of perks and of 0x4757c60 are not applied.\n"
        "- sprint_to_fire_ms = max(1, (int)(time x 0x5a9c x (1 + sum(attachment 0x250->0x08 +0x18 - 1)) +\n"
        "  sum((int)(1000 x attachment 0x250->0x68 +4)))) (0x461f8d0); the same with the super sprint time.\n"
        "- ads_time_ms = max(1, ads_time / (1 + sum(0x250->0x18 +4 - 1)) + (int)(1000 x sum(0x250->0x90 +4)))\n"
        "  (0x462ee30, 0x46318c0); ads_time is replaced by (int)(1000 x t) of the first attachment with ADS\n"
        "  times at 0x458 (0x4652650; an optic's own at 0x550 and the 0x478 variant are not read).\n"
        "- muzzle_velocity_mps = velocity x product(attachment 0x250->0xd0 +4) (0x46362d4).\n";

    struct Weapon
    {
        const Map* W = nullptr;
        const Map* T = nullptr;   // the default set's tunables
        const Map* Base = nullptr;
    };

    // the stats of a weapon with attachments (slot, index) chosen
    std::vector<std::string> LoadoutStats(const Weapon& Wp, const Map& Pkgs, const Map& Attachments, const std::vector<std::pair<int, int>>& Picks,
                                          std::string& Unknown)
    {
        const Map& W = *Wp.W;
        const Map& T = *Wp.T;
        std::vector<const Map*> Atts;
        for (const auto& [Slot, Index] : Picks)
        {
            const auto& List = W["slots"][(size_t)Slot];
            const std::string Id = Index < (int)List.size() ? List[(size_t)Index].get<std::string>() : "";
            if (Attachments.contains(Id)) Atts.push_back(&Attachments[Id]);
            else Unknown += (Unknown.empty() ? "" : " ") + std::format("slot{}#{}", Slot, Index);
        }
        // the package time of a field: the override with the most groups whose groups all match
        const auto Time = [&](const char* Field) -> int {
            int Value = Wp.Base != nullptr ? (*Wp.Base)[Field].get<int>() : -1;
            size_t Rank = 0;
            for (const auto& O : W["overrides"])
            {
                const size_t Groups = O["groups"].size();
                if (Groups <= Rank || !O.contains("animpkg") || !Pkgs.contains(O["animpkg"].get<std::string>())) continue;
                bool All = true;
                for (const auto& G : O["groups"])
                {
                    bool Any = false;
                    if (G["variant"].get<int>() < 0)
                        for (const auto& C : G["conds"])
                            for (const auto& [Slot, Index] : Picks)
                                if (C["slot"].get<int>() == Slot && Index < 128 && (C["mask"][(size_t)(Index / 32)].get<uint32_t>() >> (Index % 32) & 1)) Any = true;
                    All = All && Any;
                }
                const int V = Pkgs[O["animpkg"].get<std::string>()][Field].get<int>();
                if (All && V != 0xffff)
                {
                    Value = V;
                    Rank = Groups;
                }
            }
            return Value;
        };
        const auto Snap = [](float V) { return std::fabs(1.0f - V) <= 1e-5f ? 1.0f : V; };
        const auto Sum = [&](const char* Field, double Neutral) {
            float S = 1.0f;
            for (const Map* A : Atts) S += (float)(*A)[Field].get<double>() - (float)Neutral;
            return S;
        };
        float StfB = Snap(Sum("sprint_out_time_scale", 1.0));
        float StfC = 0.0f;
        for (const Map* A : Atts) StfC += (float)(int)(1000.0f * (float)(*A)["sprint_out_time_add"].get<double>());
        const float StfA = Snap((float)T["sprint_out_time_scale"].get<double>());
        const auto Stf = [&](const char* Field) -> std::string {
            const int Tm = Time(Field);
            if (Tm < 0) return "";
            return std::to_string(std::max(1, (int)((float)Tm * StfA * StfB + StfC)));
        };
        int Clip = -1;
        float ClipScale = 1.0f;
        for (const Map* A : Atts)
        {
            if ((*A)["clip_size_override"].get<int>() >= 0) Clip = (*A)["clip_size_override"].get<int>();
            ClipScale += (float)(*A)["clip_size_scale"].get<double>() - 1.0f;
        }
        if (Clip < 0) Clip = (int)((float)T["clip_size"].get<int>() * ClipScale);
        float AdsAdd = 0.0f;
        for (const Map* A : Atts) AdsAdd += (float)(*A)["ads_time_add"][0].get<double>();
        const float AdsS = Snap(Sum("ads_speed_scale", 1.0));
        float AdsBase = (float)T["ads_time"].get<int>();
        for (const Map* A : Atts)
            if (A->contains("ads_time_override"))
            {
                if (const float O = (float)(*A)["ads_time_override"][0].get<double>(); O > 0.0f) AdsBase = (float)(int)(O * 1000.0f);
                break;
            }
        const float Ads = std::max(1.0f, AdsBase / AdsS + (float)(int)(1000.0f * AdsAdd));
        float Vel = (float)T["muzzle_velocity"].get<double>();
        for (const Map* A : Atts) Vel *= (float)(*A)["velocity_scale"].get<double>();
        const auto Int = [](int V) { return V < 0 ? std::string() : std::to_string(V); };
        return { std::to_string(Clip), Int(Time("reload")), Int(Time("reload_empty")), Stf("sprint_out"), Stf("super_sprint_out"),
                 std::format("{:.1f}", Ads), std::format("{:.2f}", Vel) };
    }

    size_t AttachmentTables(const Map& Weapons, const Map& Tunables, const Map& Pkgs, const Map& Attachments, const Map& Names,
                            const Map& Strings, const std::string& Dir, std::string& AttCsv, std::string& LoadCsv)
    {
        const char* const Stats = "clip_size,reload_ms,reload_empty_ms,sprint_to_fire_ms,super_sprint_to_fire_ms,ads_time_ms,muzzle_velocity_mps";
        const auto Quote = [](const std::string& S) {
            if (S.find_first_of(",\"\n") == std::string::npos) return S;
            std::string Q = "\"";
            for (const char C : S) Q += C == '"' ? std::string("\"\"") : std::string(1, C);
            return Q + "\"";
        };
        const auto Display = [&](const std::string& Asset) -> std::string {
            if (!Names.contains(Asset)) return "";
            const std::string H = Names[Asset].get<std::string>();
            return Strings.contains(H) ? Strings[H].get<std::string>() : "";
        };
        std::map<std::string, Weapon> ByName;
        std::set<std::string> Seen;
        for (const auto& W : Weapons)
        {
            if (!W.contains("slots") || !Seen.insert(W["id"].get<std::string>()).second || W["sets"].empty()) continue;
            const std::string Tid = W["sets"][0]["tunables"].get<std::string>();
            if (!Tunables.contains(Tid)) continue;
            Weapon Wp{ &W, &Tunables[Tid], W.contains("animpkg") && Pkgs.contains(W["animpkg"].get<std::string>()) ? &Pkgs[W["animpkg"].get<std::string>()] : nullptr };
            ByName[W["name"].get<std::string>()] = Wp;
        }
        AttCsv = std::string("weapon,slot,index,attachment,display_name,") + Stats + "\n";
        size_t Rows = 0;
        for (const auto& [Name, Wp] : ByName)
        {
            std::string Unknown;
            const auto Base = LoadoutStats(Wp, Pkgs, Attachments, {}, Unknown);
            AttCsv += Quote(Name) + ",,,(none),";
            for (const auto& V : Base) AttCsv += "," + V;
            AttCsv += "\n";
            const auto& Slots = (*Wp.W)["slots"];
            for (size_t S = 0; S < Slots.size(); S++)
                for (size_t K = 0; K < Slots[S].size(); K++)
                {
                    const std::string Id = Slots[S][K].get<std::string>();
                    if (!Attachments.contains(Id)) continue;
                    const std::string Asset = Attachments[Id]["name"].get<std::string>();
                    std::string U;
                    const auto St = LoadoutStats(Wp, Pkgs, Attachments, { { (int)S, (int)K } }, U);
                    AttCsv += Quote(Name) + "," + std::to_string(S) + "," + std::to_string(K) + "," + Quote(Asset) + "," + Quote(Display(Asset));
                    for (const auto& V : St) AttCsv += "," + V;
                    AttCsv += "\n";
                    Rows++;
                }
        }
        // loadouts.txt: "<weapon>: <attachment>, <attachment>"
        std::ifstream In(fs::path(Dir) / "loadouts.txt");
        if (In)
        {
            LoadCsv = std::string("weapon,attachments,not_found,") + Stats + "\n";
            for (std::string Line; std::getline(In, Line);)
            {
                while (!Line.empty() && (Line.back() == '\r' || Line.back() == ' ')) Line.pop_back();
                const size_t Colon = Line.find(':');
                if (Line.empty() || Line[0] == '#' || Colon == std::string::npos) continue;
                const auto Trim = [](std::string X) {
                    while (!X.empty() && X.front() == ' ') X.erase(X.begin());
                    while (!X.empty() && X.back() == ' ') X.pop_back();
                    return X;
                };
                const std::string Name = Trim(Line.substr(0, Colon));
                const auto It = ByName.find(Name);
                if (It == ByName.end()) continue;
                std::vector<std::pair<int, int>> Picks;
                std::string Missing, Rest = Line.substr(Colon + 1);
                const auto& Slots = (*It->second.W)["slots"];
                for (size_t At0 = 0; At0 <= Rest.size();)
                {
                    size_t End = Rest.find(',', At0);
                    if (End == std::string::npos) End = Rest.size();
                    const std::string Want = Trim(Rest.substr(At0, End - At0));
                    At0 = End + 1;
                    if (Want.empty()) continue;
                    bool Found = false;
                    for (size_t S = 0; S < Slots.size() && !Found; S++)
                        for (size_t K = 0; K < Slots[S].size() && !Found; K++)
                        {
                            const std::string Id = Slots[S][K].get<std::string>();
                            if (!Attachments.contains(Id)) continue;
                            const std::string Asset = Attachments[Id]["name"].get<std::string>();
                            if (_stricmp(Asset.c_str(), Want.c_str()) == 0 || _stricmp(Display(Asset).c_str(), Want.c_str()) == 0)
                            {
                                Picks.push_back({ (int)S, (int)K });
                                Found = true;
                            }
                        }
                    if (!Found) Missing += (Missing.empty() ? "" : "; ") + Want;
                }
                std::string U;
                const auto St = LoadoutStats(It->second, Pkgs, Attachments, Picks, U);
                LoadCsv += Quote(Name) + "," + Quote(Trim(Rest)) + "," + Quote(Missing);
                for (const auto& V : St) LoadCsv += "," + V;
                LoadCsv += "\n";
            }
        }
        return Rows;
    }
}

namespace BO7Stats
{
std::string Facts(const ZoneView& V, const std::string& Zone, const std::function<bool(const std::string&)>& WantsWeapon, FactsCount& N)
{
    const auto& Img = *V.Image;
    const auto& R = *V.Walk;
    std::unordered_map<int64_t, uint64_t> Unlinked;
    for (const auto& [Field, Value] : R.BackRefs) Unlinked.try_emplace(Field, Value);
    const auto TypeOf = [&](const ZoneTables::ZoneResult::Instance& I) {
        return I.Owner < R.OwnerNames.size() ? R.OwnerNames[I.Owner] : std::string();
    };
    // where a pointer field leads in this zone, -1 elsewhere
    const auto Target = [&](int64_t Field) -> int64_t {
        const uint64_t P = At<uint64_t>(Img, Field);
        return P != 0 ? V.Off(P) : -1;
    };
    // the hash of the asset a pointer field names: its own (at IdAt) when it is here, else the
    // hash the unlinked reference holds
    const auto RefId = [&](int64_t Field, int64_t IdAt) -> uint64_t {
        const uint64_t P = At<uint64_t>(Img, Field);
        if (P == 0) return 0;
        if (const int64_t To = V.Off(P); To >= 0) return At<uint64_t>(Img, To + IdAt) & IdBits;
        return Unlinked.count(Field) ? P & IdBits : 0;
    };
    Json Out;
    Out["zone"] = Zone;
    Json Weapons = Json::array(), Tunables = Json::object(), Pkgs = Json::object(), Suits = Json::object(), Settings = Json::array(),
         Strings = Json::object(), Attachments = Json::object(), AttachmentNames = Json::object();
    std::set<std::pair<std::string, uint64_t>> Done;
    for (const auto& I : R.Instances)
    {
        const std::string Type = TypeOf(I);
        const int64_t P = I.Pos;
        if (Type == "weapon")
        {
            const uint64_t Id = At<uint64_t>(Img, P) & IdBits;
            const int64_t Def = Target(P + 0x20);
            if (Def < 0 || !Done.insert({ Type, Id }).second) continue;
            const std::string Name = V.Text(P + 8);
            if (Name.empty() || !WantsWeapon(Name)) continue;
            Json W;
            W["id"] = Hex(Id);
            W["name"] = Name;
            W["display"] = Hex(At<uint64_t>(Img, P + 0x40) & IdBits);
            const uint8_t Class = At<uint8_t>(Img, Def + 0x1afb);
            W["class"] = Class < std::size(ClassNames) ? ClassNames[Class] : std::to_string(Class);
            if (const uint64_t Pkg = RefId(Def + 0x350, 0)) W["animpkg"] = Hex(Pkg);
            // the tunables sets (0x1b18, count 0x1b20) and each one's tags (0x1b28: {u32 count, u64 *tags})
            Json Sets = Json::array();
            const int64_t Arr = Target(Def + 0x1b18), Tags = Target(Def + 0x1b28);
            const uint32_t Count = std::min<uint32_t>(At<uint32_t>(Img, Def + 0x1b20), 16);
            for (uint32_t K = 0; Arr >= 0 && K < Count; K++)
            {
                Json S;
                S["tunables"] = Hex(RefId(Arr + 8 * (int64_t)K, 0));
                Json TagList = Json::array();
                if (Tags >= 0)
                {
                    const int64_t Rec = Tags + 0x10 * (int64_t)K;
                    const int64_t List = Target(Rec + 8);
                    for (uint32_t J = 0; List >= 0 && J < std::min<uint32_t>(At<uint32_t>(Img, Rec), 16); J++)
                        TagList.push_back(Hex(At<uint64_t>(Img, List + 8 * (int64_t)J) & IdBits));
                }
                S["tags"] = std::move(TagList);
                Sets.push_back(std::move(S));
            }
            if (Sets.empty())
                if (const uint64_t T = RefId(Def + 0x1b30, 0)) Sets.push_back(Json{ { "tunables", Hex(T) }, { "tags", Json::array() } });
            W["sets"] = std::move(Sets);
            // the attachments by slot: weapon +0x80 + slot * 0x10 {attachment **, u32 count}; a
            // condition's index k names the slot's k-th (0x46843b0 reads the list through 0xec8a200)
            Json Slots = Json::array();
            for (int64_t S = 0; S < 17; S++)
            {
                Json List = Json::array();
                const int64_t A = Target(P + 0x80 + S * 0x10);
                const uint32_t N2 = std::min<uint32_t>(At<uint32_t>(Img, P + 0x88 + S * 0x10), 128);
                for (uint32_t K = 0; A >= 0 && K < N2; K++) List.push_back(Hex(RefId(A + 8 * (int64_t)K, 0)));
                Slots.push_back(std::move(List));
            }
            W["slots"] = std::move(Slots);
            // the animation package overrides (weapon_def 0x60, 0x30 each, count 0x1838; 0x464aab0): u32
            // group count (the rank: more groups wins) at 0, the groups at 8 (0x18 each: u32 count,
            // conditions at 8 (0x14 each: u8 slot, u32 mask[4] of slot indexes), i32 variant at 0x10,
            // -1 any; 0x46843b0), the package at 0x10 (0x18 for the alternate view)
            Json Ovr = Json::array();
            const int64_t OArr = Target(Def + 0x60);
            const uint32_t ON = std::min<uint32_t>(At<uint32_t>(Img, Def + 0x1838), 256);
            for (uint32_t K = 0; OArr >= 0 && K < ON; K++)
            {
                const int64_t E = OArr + 0x30 * (int64_t)K;
                Json O, Groups = Json::array();
                const int64_t G = Target(E + 8);
                const uint32_t GN = std::min<uint32_t>(At<uint32_t>(Img, E), 16);
                for (uint32_t J = 0; G >= 0 && J < GN; J++)
                {
                    const int64_t Gr = G + 0x18 * (int64_t)J;
                    Json Conds = Json::array();
                    const int64_t C = Target(Gr + 8);
                    for (uint32_t L = 0; C >= 0 && L < std::min<uint32_t>(At<uint32_t>(Img, Gr), 16); L++)
                    {
                        const int64_t Cd = C + 0x14 * (int64_t)L;
                        Conds.push_back(Json{ { "slot", At<uint8_t>(Img, Cd) },
                                              { "mask", Json::array({ At<uint32_t>(Img, Cd + 4), At<uint32_t>(Img, Cd + 8), At<uint32_t>(Img, Cd + 0xc),
                                                                      At<uint32_t>(Img, Cd + 0x10) }) } });
                    }
                    Groups.push_back(Json{ { "variant", At<int32_t>(Img, Gr + 0x10) }, { "conds", std::move(Conds) } });
                }
                O["groups"] = std::move(Groups);
                if (const uint64_t Pk = RefId(E + 0x10, 0)) O["animpkg"] = Hex(Pk);
                Ovr.push_back(std::move(O));
            }
            W["overrides"] = std::move(Ovr);
            Weapons.push_back(std::move(W));
            N.Weapons++;
        }
        else if (Type == "weapontunables")
        {
            const uint64_t Id = At<uint64_t>(Img, P) & IdBits;
            if (!Done.insert({ Type, Id }).second) continue;
            Json T;
            const auto I32 = [&](int64_t O) { return At<int32_t>(Img, P + O); };
            const auto F32 = [&](int64_t O) { return (double)At<float>(Img, P + O); };
            T["fire_time"] = I32(0x5cd4);
            T["burst_count"] = I32(0x5d1c);
            T["start_ammo"] = I32(0x5d34);
            T["start_ammo_override"] = I32(0x5d38);
            T["clip_size"] = I32(0x5d48);
            T["max_ammo"] = I32(0x5d44);
            T["max_ammo_by_clips"] = At<uint8_t>(Img, P + 0x5e13);
            T["fire_type"] = At<uint8_t>(Img, P + 0x5dfc);
            T["ads_time"] = I32(0x5dac);
            T["muzzle_velocity"] = F32(0x30);
            T["damage"] = Json::array({ I32(0x70), I32(0x74), I32(0x78), I32(0x7c), I32(0x80) });
            T["ranges"] = Json::array({ F32(0x84), F32(0x88), F32(0x8c), F32(0x90) });
            Json Hit = Json::array();
            for (int K = 0; K < 22; K++) Hit.push_back(F32(0x530 + 4 * K));
            T["hit"] = std::move(Hit);
            T["sprint_out_time_scale"] = F32(0x5a9c);
            T["sprint_speed_scale"] = F32(0x5c14);
            T["super_sprint_speed_scale"] = F32(0x5c28);
            T["prone_speed_scale"] = F32(0x5c08);
            Tunables[Hex(Id)] = std::move(T);
            N.Tunables++;
        }
        else if (Type == "attachment")
        {
            const uint64_t Id = At<uint64_t>(Img, P) & IdBits;
            if (!Done.insert({ Type, Id }).second) continue;
            const std::string Name = V.Text(P + 0x248);
            if (Name.empty()) continue;
            Json A;
            A["name"] = Name;
            // clip: 0x4637d88 adds (0xb90 - 1); 0x4637e2c takes the i32 at 8 of the block at 0x400 when >= 0
            A["clip_size_scale"] = (double)At<float>(Img, P + 0xb90);
            const int64_t Ov = Target(P + 0x400);
            A["clip_size_override"] = Ov >= 0 ? At<int32_t>(Img, Ov + 8) : -1;
            // the modifier table at 0x250 (pointers to small blocks)
            const int64_t T = Target(P + 0x250);
            const auto Sub = [&](int64_t O) { return T >= 0 ? Target(T + O) : -1; };
            const int64_t B08 = Sub(0x08), B18 = Sub(0x18), B68 = Sub(0x68), B90 = Sub(0x90), BD0 = Sub(0xd0);
            // 0x461f8d0: sprint-out time scale B += (0x08 block + 0x18) - 1; C += (int)(1000 x (0x68 block + 4))
            A["sprint_out_time_scale"] = B08 >= 0 ? (double)At<float>(Img, B08 + 0x18) : 1.0;
            A["sprint_out_time_add"] = B68 >= 0 ? (double)At<float>(Img, B68 + 4) : 0.0;
            // 0x46318c0: the 4 aim-down-sight times' additions (0x90 block + 4..0x10, seconds; 0x462ee30
            // adds (int)(1000 x sum)) and the divisor term (0x18 block + 4)
            Json Ads = Json::array();
            for (int K = 0; K < 4; K++) Ads.push_back(B90 >= 0 ? (double)At<float>(Img, B90 + 4 + 4 * K) : 0.0);
            A["ads_time_add"] = std::move(Ads);
            A["ads_speed_scale"] = B18 >= 0 ? (double)At<float>(Img, B18 + 4) : 1.0;
            // 0x4652650: the first attachment with a block at 0x458 replaces the 4 ADS times (seconds, 0x462ee30
            // takes (int)(1000 x t) when t > 0)
            if (const int64_t B458 = Target(P + 0x458); B458 >= 0)
                A["ads_time_override"] = Json::array({ (double)At<float>(Img, B458), (double)At<float>(Img, B458 + 4),
                                                       (double)At<float>(Img, B458 + 8), (double)At<float>(Img, B458 + 0xc) });
            // 0x46362d4: the muzzle velocity times each attachment's 0xd0 block + 4
            A["velocity_scale"] = BD0 >= 0 ? (double)At<float>(Img, BD0 + 4) : 1.0;
            Attachments[Hex(Id)] = std::move(A);
            N.Attachments++;
        }
        else if (Type == "animpkg")
        {
            const uint64_t Id = At<uint64_t>(Img, P) & IdBits;
            const int64_t B = Target(P + 0x18);
            if (B < 0 || B + 0x526 > (int64_t)Img.size() || !Done.insert({ Type, Id }).second) continue;
            const auto U16 = [&](int64_t O) { return At<uint16_t>(Img, B + O); };
            Pkgs[Hex(Id)] = Json{ { "first_raise", U16(0x2) }, { "reload_empty", U16(0xac) }, { "reload", U16(0xae) },
                                  { "sprint_out", U16(0x522) }, { "super_sprint_out", U16(0x524) } };
            N.AnimPkgs++;
        }
        else if (Type == "suit")
        {
            const uint64_t Id = At<uint64_t>(Img, P + 0x20) & IdBits;
            if (!Done.insert({ Type, Id }).second) continue;
            Suits[Hex(Id)] = Json{ { "sprint_speed_multiplier", (double)At<float>(Img, P + 0xe18) },
                                   { "super_sprint_speed_multiplier", (double)At<float>(Img, P + 0xe78) } };
            N.Suits++;
        }
        else if (Type == "scriptbundle")
        {
            // its key table at 0x10: keys 0, entries {u8 type, value at 8} 0x10, u32 count 0x18
            const int64_t Tab = P + 0x10, Keys = Target(Tab), Entries = Target(Tab + 0x10);
            const uint32_t Count = At<uint32_t>(Img, Tab + 0x18);
            if (Keys < 0 || Entries < 0) continue;
            Json S;
            for (uint32_t K = 0; K < Count && K < 4096; K++)
            {
                const uint64_t Key = At<uint64_t>(Img, Keys + 8 * (int64_t)K);
                const int64_t E = Entries + 0x10 * (int64_t)K;
                if (Key == TagKey) S["tag"] = Hex(At<uint64_t>(Img, E + 8) & IdBits);
                else if (Key == AttachmentKey)
                {
                    if (const int64_t Rec = V.Off(At<uint64_t>(Img, E + 8)); Rec >= 0) S["attachment_name"] = V.Text(Rec);
                }
                else if (Key == NameKey && At<uint8_t>(Img, E) == 4) S["name"] = Hex(At<uint64_t>(Img, E + 8) & IdBits);
                else if (Key == SuitKey)
                    if (const int64_t Rec = V.Off(At<uint64_t>(Img, E + 8)); Rec >= 0) S["suit"] = V.Text(Rec);
            }
            if (S.contains("attachment_name") && S.contains("name"))
            {
                AttachmentNames[S["attachment_name"].get<std::string>()] = S["name"];
                N.Settings++;
            }
            if (!S.contains("suit")) continue;
            S["bundle"] = Hex(At<uint64_t>(Img, P) & IdBits);
            Settings.push_back(std::move(S));
            N.Settings++;
        }
        else if (Type == "localize")
        {
            const uint64_t Id = At<uint64_t>(Img, P) & IdBits;
            if (!Done.insert({ Type, Id }).second) continue;
            const std::string S = V.LocalizeText(P);
            if (S.empty()) continue;
            Strings[Hex(Id)] = S;
            N.Strings++;
        }
    }
    Out["weapons"] = std::move(Weapons);
    Out["tunables"] = std::move(Tunables);
    Out["animpkgs"] = std::move(Pkgs);
    Out["suits"] = std::move(Suits);
    Out["settings"] = std::move(Settings);
    Out["strings"] = std::move(Strings);
    Out["attachments"] = std::move(Attachments);
    Out["attachment_names"] = std::move(AttachmentNames);
    return Out.dump(1, '\t', false, nlohmann::json::error_handler_t::replace);
}

bool Merge(const std::string& Dir, size_t& Rows, std::string& Error)
{
    Rows = 0;
    // (std::map-backed: the facts hold tens of thousands of strings, the ordered map's search is linear)
    using Map = nlohmann::json;
    Map Weapons = Map::array(), Tunables = Map::object(), Pkgs = Map::object(), Suits = Map::object(), Strings = Map::object(),
        Attachments = Map::object(), AttachmentNames = Map::object();
    std::vector<Map> Settings;
    std::vector<std::string> Zones;
    std::error_code Ec;
    std::vector<fs::path> Files;
    for (const auto& E : fs::directory_iterator(fs::path(Dir) / "facts", Ec))
        if (E.path().extension() == ".json") Files.push_back(E.path());
    std::sort(Files.begin(), Files.end());
    // the run queues one merge per zone: the table is rebuilt only when the facts changed
    static std::string LastFacts;
    static size_t Merged = 0;
    std::string Stamp = Dir;
    for (const auto& F : Files) Stamp += "|" + F.filename().string() + ":" + std::to_string(fs::last_write_time(F, Ec).time_since_epoch().count());
    if (Stamp == LastFacts && fs::exists(fs::path(Dir) / "weapon_stats.csv", Ec))
    {
        Rows = Merged;
        return true;
    }
    for (const auto& F : Files)
    {
        std::ifstream In(F, std::ios::binary);
        Map J = Map::parse(In, nullptr, false);
        if (J.is_discarded()) continue;
        Zones.push_back(J.value("zone", F.stem().string()));
        for (auto& W : J["weapons"]) Weapons.push_back(W);
        for (auto& [K, X] : J["tunables"].items()) Tunables[K] = X;
        for (auto& [K, X] : J["animpkgs"].items()) Pkgs[K] = X;
        for (auto& [K, X] : J["suits"].items()) Suits[K] = X;
        for (auto& [K, X] : J["strings"].items()) Strings[K] = X;
        for (auto& S : J["settings"]) Settings.push_back(S);
        for (auto& [K, X] : J["attachments"].items()) Attachments[K] = X;
        for (auto& [K, X] : J["attachment_names"].items()) AttachmentNames[K] = X;
    }
    if (Files.empty())
    {
        Error = "no facts in " + (fs::path(Dir) / "facts").string();
        return false;
    }
    // each tag's suits: the gametype settings asking for it; the suits by name
    std::map<std::string, std::set<std::string>> SuitsOfTag;
    for (const auto& S : Settings)
        if (S.contains("tag")) SuitsOfTag[S["tag"].get<std::string>()].insert(S["suit"].get<std::string>());
    const auto Suit = [&](const std::string& Name) -> const Map* {
        const std::string Id = Hex(GameBlackOps7::HashIWAsset(Name));
        return Suits.contains(Id) ? &Suits[Id] : nullptr;
    };
    const std::string MpSuit = "sat_defaultsuit_mp";

    std::vector<std::string> Head = { "weapon", "display_name", "class", "set", "tags", "fire_time_ms", "rpm", "burst_count",
                                      "fire_type", "clip_size", "start_ammo", "max_ammo", "reload_ms", "reload_empty_ms",
                                      "first_raise_ms", "sprint_to_fire_ms", "super_sprint_to_fire_ms", "ads_time_ms",
                                      "muzzle_velocity_mps", "damage_max", "range_max_m", "damage_mid1", "range_mid1_m",
                                      "damage_mid2", "range_mid2_m", "damage_mid3", "range_mid3_m", "damage_min" };
    for (size_t K = 1; K < std::size(HitLocations); K++) Head.push_back(std::string("hit_") + HitLocations[K]);
    for (const char* H : { "sprint_speed_scale", "super_sprint_speed_scale", "prone_speed_scale", "suit", "sprint_mps",
                           "super_sprint_mps", "sprint_mps_mp_suit", "super_sprint_mps_mp_suit" })
        Head.push_back(H);

    const auto Num = [](double V, int Digits) {
        std::string S = std::format("{:.{}f}", V, Digits);
        if (S.find('.') != std::string::npos)
        {
            while (S.back() == '0') S.pop_back();
            if (S.back() == '.') S.pop_back();
        }
        return S;
    };
    // the UI's speed (0x4ced840): base x suit multiplier(s), through the weapon's scale entry
    // (max(add, 0) x mul x speed + offset, 0x4689760; mul = the tunables scale, add 1, offset 0)
    const auto Speed = [&](const Map* S, double Scale, bool Super) -> std::string {
        if (S == nullptr) return "";
        float V = BaseSpeed * (float)(*S)["sprint_speed_multiplier"].get<double>();
        if (Super) V *= (float)(*S)["super_sprint_speed_multiplier"].get<double>();
        return Num((double)((float)Scale * V / InchesPerMetre), 2);
    };

    struct Row
    {
        std::string Weapon;
        size_t Set = 0;
        std::vector<std::string> Cells;
    };
    std::vector<Row> Table;
    std::set<std::string> Seen;
    for (const auto& W : Weapons)
    {
        const std::string Name = W["name"].get<std::string>();
        if (!Seen.insert(W["id"].get<std::string>()).second) continue;
        const std::string Display = Strings.contains(W["display"].get<std::string>()) ? Strings[W["display"].get<std::string>()].get<std::string>() : "";
        const Map* Pkg = W.contains("animpkg") && Pkgs.contains(W["animpkg"].get<std::string>()) ? &Pkgs[W["animpkg"].get<std::string>()] : nullptr;
        size_t K = 0;
        for (const auto& S : W["sets"])
        {
            K++;
            const std::string Tid = S["tunables"].get<std::string>();
            if (!Tunables.contains(Tid)) continue;
            const Map& T = Tunables[Tid];
            std::vector<std::string> Tags;
            std::set<std::string> RowSuits;
            for (const auto& Tag : S["tags"])
            {
                Tags.push_back(TagName(std::stoull(Tag.get<std::string>(), nullptr, 16)));
                if (const auto It = SuitsOfTag.find(Tag.get<std::string>()); It != SuitsOfTag.end()) RowSuits.insert(It->second.begin(), It->second.end());
            }
            std::string TagText, SuitName;
            for (const auto& X : Tags) TagText += (TagText.empty() ? "" : " ") + X;
            if (RowSuits.size() == 1) SuitName = *RowSuits.begin();
            const int Fire = T["fire_time"].get<int>();
            const int FireType = T["fire_type"].get<int>();
            const std::string FireText = FireType == 0 ? "auto (0)" : (FireType == 1 || FireType == 3 || FireType == 4) ? std::format("semi-auto ({})", FireType) : std::to_string(FireType);
            const int StartOverride = T["start_ammo_override"].get<int>();
            const auto Stf = [&](const char* Field) -> std::string {
                if (Pkg == nullptr) return "";
                const float Scaled = (float)(*Pkg)[Field].get<int>() * (float)T["sprint_out_time_scale"].get<double>();
                return std::to_string(std::max(1, (int)Scaled));
            };
            const auto PkgTime = [&](const char* Field) { return Pkg == nullptr ? std::string() : std::to_string((*Pkg)[Field].get<int>()); };
            const auto& Dmg = T["damage"];
            const auto& Rng = T["ranges"];
            const auto Metres = [&](size_t I) { return Num((double)((float)Rng[I].get<double>() / InchesPerMetre), 2); };
            std::vector<std::string> C = { Name, Display, W["class"].get<std::string>(), std::to_string(K), TagText, std::to_string(Fire),
                                           Fire > 0 ? Num(60000.0 / Fire, 2) : "", std::to_string(T["burst_count"].get<int>()), FireText,
                                           std::to_string(T["clip_size"].get<int>()),
                                           std::to_string(StartOverride != 0 ? StartOverride : T["start_ammo"].get<int>()),
                                           T["max_ammo_by_clips"].get<int>() == 0 ? std::to_string(T["max_ammo"].get<int>()) : "",
                                           PkgTime("reload"), PkgTime("reload_empty"), PkgTime("first_raise"), Stf("sprint_out"),
                                           Stf("super_sprint_out"), std::to_string(T["ads_time"].get<int>()),
                                           Num(T["muzzle_velocity"].get<double>(), 2), std::to_string(Dmg[4].get<int>()), Metres(0),
                                           std::to_string(Dmg[1].get<int>()), Metres(1), std::to_string(Dmg[2].get<int>()), Metres(2),
                                           std::to_string(Dmg[3].get<int>()), Metres(3), std::to_string(Dmg[0].get<int>()) };
            for (size_t H = 1; H < std::size(HitLocations); H++) C.push_back(Num(T["hit"][H].get<double>(), 4));
            const double Sprint = T["sprint_speed_scale"].get<double>(), Super = T["super_sprint_speed_scale"].get<double>();
            C.push_back(Num(Sprint, 4));
            C.push_back(Num(Super, 4));
            C.push_back(Num(T["prone_speed_scale"].get<double>(), 4));
            const Map* RowSuit = SuitName.empty() ? nullptr : Suit(SuitName);
            C.push_back(RowSuit != nullptr ? SuitName : "");
            C.push_back(Speed(RowSuit, Sprint, false));
            C.push_back(Speed(RowSuit, Super, true));
            C.push_back(Speed(Suit(MpSuit), Sprint, false));
            C.push_back(Speed(Suit(MpSuit), Super, true));
            Table.push_back({ Name, K, std::move(C) });
        }
    }
    std::sort(Table.begin(), Table.end(), [](const Row& A, const Row& B) { return A.Weapon != B.Weapon ? A.Weapon < B.Weapon : A.Set < B.Set; });

    std::string Csv;
    const auto Quote = [](const std::string& S) {
        if (S.find_first_of(",\"\n") == std::string::npos) return S;
        std::string Q = "\"";
        for (const char C : S) Q += C == '"' ? std::string("\"\"") : std::string(1, C);
        return Q + "\"";
    };
    for (size_t I = 0; I < Head.size(); I++) Csv += (I ? "," : "") + Head[I];
    Csv += "\n";
    Json Out = Json::array();
    for (const auto& R : Table)
    {
        Json O;
        for (size_t I = 0; I < Head.size(); I++)
        {
            Csv += (I ? "," : "") + Quote(R.Cells[I]);
            O[Head[I]] = R.Cells[I];
        }
        Csv += "\n";
        Out.push_back(std::move(O));
    }
    std::string Readme =
        "Black Ops 7 weapon stats (GrayWolf --type weaponstats)\n"
        "=====================================================\n\n"
        "One row per weapon and tunables set. Only fields whose meaning the game's code proves are here;\n"
        "the raw struct dumps (--type weapons) keep everything else, by offset.\n\n"
        "Zones read so far (facts/): ";
    for (size_t I = 0; I < Zones.size(); I++) Readme += (I ? ", " : "") + Zones[I];
    Readme +=
        "\nA value its zone was not read for stays empty: the guns are in global_stream_mp and\n"
        "global_stream_cfwd_mp, their animation packages (reload, raise, sprint-to-fire) in ingame_shared_mp, the\n"
        "suits and game-mode settings in global_shared_mp, the display names in ens_global. Export those zones\n"
        "into the same --out (a .txt list of zone paths works) and the table is rebuilt from every facts file.\n\n"
        "Columns and units\n"
        "- set: the tunables set (weapon_def 0x1b18[set-1]). tags: the set's mode tags (FNV-1a 64 names). The\n"
        "  game picks the set whose tags hold the gametype settings' tag (0x460f300); the set tagged 'default'\n"
        "  is the fallback. t10_br is the tag of the Warzone settings (suit sat_newsuit_wz), t10_zm of Zombies.\n"
        "- fire_time_ms: tunables 0x5cd4 (weaponfiretime). rpm = 60000 / fire_time_ms.\n"
        "- burst_count: 0x5d1c (weaponburstcount). fire_type: u8 0x5dfc; weaponisauto is 0, weaponissemiauto\n"
        "  1, 3 or 4 (each builtin also checks the weapon's ammo kind, not reproduced). An attachment can override it.\n"
        "- clip_size: 0x5d48. start_ammo: 0x5d38 when not 0, else 0x5d34. max_ammo: 0x5d44 (weaponmaxammo with\n"
        "  no attachment, no perk); empty when u8 0x5e13 is set (then it counts magazines, not reproduced).\n"
        "- reload_ms, reload_empty_ms, first_raise_ms: the base animation package (weapon_def 0x350), u16 0xae,\n"
        "  0xac and 0x2 of its time block (getweaponreloadtime, weaponfirstraisetime), before attachments and perks.\n"
        "- sprint_to_fire_ms = max(1, (int)(u16 0x522 x tunables 0x5a9c)), super_sprint_to_fire_ms the same\n"
        "  with u16 0x524 (0x45891e0, 0x461f8d0: a single-precision product truncated like the game: 250 x 0.76f is 190.0f).\n"
        "- ads_time_ms: 0x5dac. muzzle_velocity_mps: 0x30 (metres per second).\n"
        "- damage_*: the first record of the table at 0x70 (0x46386d0): damage_max up to range_max_m, then the\n"
        "  first mid whose range is set and beyond the distance, else damage_min (0x464c9e0). Ranges are\n"
        "  inches / 39.37. Other tables (modes, profiles) are in the raw dump only.\n"
        "- hit_*: the multipliers of the table at 0x530 (0x4642180), by the game's hit location order.\n"
        "- sprint_speed_scale (0x5c14), super_sprint_speed_scale (0x5c28), prone_speed_scale (0x5c08): scales.\n"
        "- sprint_mps = 190 x suit 0xe18 x sprint_speed_scale / 39.37, super_sprint_mps also x suit 0xe78 and\n"
        "  with super_sprint_speed_scale: the weapon stats UI's formula (0x4ced840). suit: the suit of the\n"
        "  gametype settings whose tag the set holds (empty when no settings name the set's tags, as for\n"
        "  'default'). *_mp_suit: the same with sat_defaultsuit_mp, the suit of the multiplayer settings (they\n"
        "  hold no tag, so the 'default' set is used unless a fallback settings bundle gives one).\n";
    std::string AttCsv, LoadCsv;
    const size_t AttRows = AttachmentTables(Weapons, Tunables, Pkgs, Attachments, AttachmentNames, Strings, Dir, AttCsv, LoadCsv);
    Readme += AttachmentReadme;
    std::error_code E2;
    fs::create_directories(Dir, E2);
    if (AttRows != 0)
    {
        ExportWriters::WriteBytes(fs::path(Dir) / "weapon_attachments.csv", (const uint8_t*)AttCsv.data(), AttCsv.size());
        if (!LoadCsv.empty()) ExportWriters::WriteBytes(fs::path(Dir) / "weapon_loadouts.csv", (const uint8_t*)LoadCsv.data(), LoadCsv.size());
    }
    const std::string JsonText = Out.dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
    const bool Ok = ExportWriters::WriteBytes(fs::path(Dir) / "weapon_stats.csv", (const uint8_t*)Csv.data(), Csv.size()) &&
                    ExportWriters::WriteBytes(fs::path(Dir) / "weapon_stats.json", (const uint8_t*)JsonText.data(), JsonText.size()) &&
                    ExportWriters::WriteBytes(fs::path(Dir) / "README.txt", (const uint8_t*)Readme.data(), Readme.size());
    if (!Ok) Error = "write failed in " + Dir;
    Rows = Table.size();
    if (Ok) LastFacts = Stamp, Merged = Rows;
    return Ok;
}
}
