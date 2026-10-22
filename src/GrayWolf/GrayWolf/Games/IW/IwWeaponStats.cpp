#include "stdafx.h"

// The iwctx family's weapon stats (IwWeaponStats.h). Every offset comes from the game's Layout,
// proven from its exe; what is not proven (-1) stays out of the facts and its column stays empty.

#include "Games/IW/IwWeaponStats.h"
#include "Games/Common/WeaponStatsTable.h"

#include "ExportNaming.h"
#include "ExportWriters.h"

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
    using ExportNaming::Num;
    using ExportWriters::CsvField;
    constexpr uint64_t IdBits = 0x7FFFFFFFFFFFFFFFull;

    std::string Hex(uint64_t H) { return std::format("{:016x}", H); }

    // the weapon class u8, named by the weaponclass builtin's table (the same 16 names in each exe
    // of the family)
    const char* const ClassNames[] = { "none", "rifle", "sniper", "mg", "smg", "spread", "pistol", "grenade",
                                       "rocketlauncher", "turret", "throwingknife", "non-player", "item", "beam", "ball", "fist" };
    // the hit locations in the order of the game's hit location name table (the same 22 in each exe;
    // index 0, none, left out of the columns)
    const char* const HitLocations[] = { "none", "helmet", "head", "neck", "torso_upper", "torso_lower", "right_arm_upper",
                                         "left_arm_upper", "right_arm_lower", "left_arm_lower", "right_hand", "left_hand",
                                         "right_leg_upper", "left_leg_upper", "right_leg_lower", "left_leg_lower", "right_foot",
                                         "left_foot", "gun", "shield", "armor", "soft" };
    constexpr float InchesPerMetre = 39.37f;   // the engine's distance unit

    uint64_t Fnv64(const std::string& S)
    {
        uint64_t H = 0xcbf29ce484222325ull;
        for (const char C : S) H = (H ^ (uint8_t)C) * 0x100000001b3ull;
        return H & IdBits;
    }

    // a tunables tag's name: the tags are FNV-1a 64 of a name (the set picker compares them to the
    // settings' value); the names the game gives
    std::string TagName(const IwWeaponStats::Layout& L, uint64_t Tag)
    {
        for (const auto& N : L.TagNames)
            if (Fnv64(N) == (Tag & IdBits)) return N;
        return Hex(Tag & IdBits);
    }

    using Map = nlohmann::json;

    // a value of the facts as text, "" when the game's facts leave it out
    int IntOr(const Map& M, const char* Key, int Default) { return M.contains(Key) ? M[Key].get<int>() : Default; }
    double FloatOr(const Map& M, const char* Key, double Default) { return M.contains(Key) ? M[Key].get<double>() : Default; }

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
            int Value = Wp.Base != nullptr ? Wp.Base->value(Field, -1) : -1;
            size_t Rank = 0;
            if (!W.contains("overrides")) return Value;
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
                const int V = Pkgs[O["animpkg"].get<std::string>()].value(Field, 0xffff);
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
            for (const Map* A : Atts) S += (float)FloatOr(*A, Field, Neutral) - (float)Neutral;
            return S;
        };
        float StfB = Snap(Sum("sprint_out_time_scale", 1.0));
        float StfC = 0.0f;
        for (const Map* A : Atts) StfC += (float)(int)(1000.0f * (float)FloatOr(*A, "sprint_out_time_add", 0.0));
        const auto Stf = [&](const char* Field) -> std::string {
            const int Tm = Time(Field);
            if (Tm < 0 || !T.contains("sprint_out_time_scale")) return "";
            const float StfA = Snap((float)T["sprint_out_time_scale"].get<double>());
            return std::to_string(std::max(1, (int)((float)Tm * StfA * StfB + StfC)));
        };
        int Clip = -1;
        float ClipScale = 1.0f;
        for (const Map* A : Atts)
        {
            if (IntOr(*A, "clip_size_override", -1) >= 0) Clip = (*A)["clip_size_override"].get<int>();
            ClipScale += (float)FloatOr(*A, "clip_size_scale", 1.0) - 1.0f;
        }
        if (Clip < 0 && T.contains("clip_size")) Clip = (int)((float)T["clip_size"].get<int>() * ClipScale);
        // the ADS times K (0 in, 1 out): the same divisor, addition K and override K
        const float AdsS = Snap(Sum("ads_speed_scale", 1.0));
        const auto AdsTime = [&](size_t K, const char* Field) -> std::string {
            if (!T.contains(Field)) return "";
            float AdsAdd = 0.0f;
            for (const Map* A : Atts)
                if (A->contains("ads_time_add")) AdsAdd += (float)(*A)["ads_time_add"][K].get<double>();
            float AdsBase = (float)T[Field].get<int>();
            for (const Map* A : Atts)
                if (A->contains("ads_time_override"))
                {
                    if (const float O = (float)(*A)["ads_time_override"][K].get<double>(); O > 0.0f) AdsBase = (float)(int)(O * 1000.0f);
                    break;
                }
            return std::format("{:.1f}", std::max(1.0f, AdsBase / AdsS + (float)(int)(1000.0f * AdsAdd)));
        };
        std::string Vel;
        if (T.contains("muzzle_velocity"))
        {
            float V = (float)T["muzzle_velocity"].get<double>();
            for (const Map* A : Atts) V *= (float)FloatOr(*A, "velocity_scale", 1.0);
            Vel = std::format("{:.2f}", V);
        }
        const auto Int = [](int V) { return V < 0 ? std::string() : std::to_string(V); };
        return { Int(Clip), Int(Time("reload")), Int(Time("reload_empty")), Int(Time("reload_add")), Int(Time("reload_empty_add")),
                 Stf("sprint_out"), Stf("super_sprint_out"), AdsTime(0, "ads_time"), AdsTime(1, "ads_out_time"), Vel };
    }

    size_t AttachmentTables(const Map& Weapons, const Map& Tunables, const Map& Pkgs, const Map& Attachments, const Map& Names,
                            const Map& Strings, const std::string& Dir, std::string& AttCsv, std::string& LoadCsv)
    {
        const char* const Stats = "clip_size,reload_ms,reload_empty_ms,reload_add_ms,reload_empty_add_ms,sprint_to_fire_ms,"
                                  "super_sprint_to_fire_ms,ads_time_ms,ads_out_time_ms,muzzle_velocity_mps";
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
            AttCsv += CsvField(Name) + ",,,(none),";
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
                    AttCsv += CsvField(Name) + "," + std::to_string(S) + "," + std::to_string(K) + "," + CsvField(Asset) + "," + CsvField(Display(Asset));
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
                LoadCsv += CsvField(Name) + "," + CsvField(Trim(Rest)) + "," + CsvField(Missing);
                for (const auto& V : St) LoadCsv += "," + V;
                LoadCsv += "\n";
            }
        }
        return Rows;
    }

    // weapon_default_parts.csv: each weapon's default attachment per slot and that attachment's own
    // models (an attachment no facts file holds: its id only, its other cells empty)
    size_t DefaultPartsTable(const IwWeaponStats::Layout& L, const Map& Weapons, const Map& Attachments, const Map& Names, const Map& Strings, std::string& Csv)
    {
        std::map<std::string, const Map*> ByName;
        std::set<std::string> Seen;
        for (const auto& W : Weapons)
            if (W.contains("defaults") && Seen.insert(W["id"].get<std::string>()).second) ByName.emplace(W["name"].get<std::string>(), &W);
        Csv = "weapon,slot,attachment_id,attachment,display_name,world_model,view_model\n";
        size_t Rows = 0;
        for (const auto& [Name, W] : ByName)
            for (const auto& D : (*W)["defaults"])
            {
                const std::string Id = D["attachment"].get<std::string>();
                std::string Asset, Display, World, View;
                if (Attachments.contains(Id))
                {
                    const Map& A = Attachments[Id];
                    Asset = A["name"].get<std::string>();
                    World = A.value("world_model", std::string());
                    View = A.value("view_model", std::string());
                    // a model the hash packages do not name: the other model's name with _vm_ and _wm_
                    // swapped, kept only when its hash is the model's (the models of a part pair up so)
                    const auto Pair = [&](std::string& Model, const char* IdKey, const std::string& Other, const char* From, const char* To) {
                        if (!L.NameHash || !A.contains(IdKey)) return;
                        const uint64_t ModelId = std::stoull(A[IdKey].get<std::string>(), nullptr, 16);
                        if (Model != std::format("xmodel_{:x}", ModelId)) return;
                        const size_t At0 = Other.find(From);
                        if (At0 == std::string::npos) return;
                        const std::string Candidate = Other.substr(0, At0) + To + Other.substr(At0 + std::strlen(From));
                        if ((L.NameHash(Candidate) & IdBits) == ModelId) Model = Candidate;
                    };
                    Pair(World, "world_model_id", View, "_vm_", "_wm_");
                    Pair(View, "view_model_id", World, "_wm_", "_vm_");
                    if (Names.contains(Asset) && Strings.contains(Names[Asset].get<std::string>()))
                        Display = Strings[Names[Asset].get<std::string>()].get<std::string>();
                }
                Csv += CsvField(Name) + "," + std::to_string(D["slot"].get<int>()) + "," + Id + "," + CsvField(Asset) + "," + CsvField(Display) + "," +
                       CsvField(World) + "," + CsvField(View) + "\n";
                Rows++;
            }
        return Rows;
    }
}

namespace IwWeaponStats
{
std::string Facts(const Layout& L, const ZoneView& V, const std::string& Zone, const std::function<bool(const std::string&)>& WantsWeapon,
                  FactsCount& N)
{
    const auto& Img = *V.Image;
    const auto& R = *V.Walk;
    std::unordered_map<int64_t, uint64_t> Unlinked;
    for (const auto& [Field, Value] : R.BackRefs) Unlinked.try_emplace(Field, Value);
    const auto TypeOf = [&](const ZoneTables::ZoneResult::Instance& I) {
        return I.Owner < R.OwnerNames.size() ? R.OwnerNames[I.Owner] : std::string();
    };
    // where a pointer field leads in this zone, -1 elsewhere (or when the layout has no such field)
    const auto Target = [&](int64_t Field) -> int64_t {
        if (Field < 0) return -1;
        const uint64_t P = At<uint64_t>(Img, Field);
        return P != 0 ? V.Off(P) : -1;
    };
    // an asset's id: the hash at its 0, or the FNV-1a 64 of its name string (a game naming its
    // assets by string; a name-only header ",name" has the id of the asset it names)
    const auto AssetId = [&](int64_t Pos) -> uint64_t {
        if (L.AssetNameAt < 0) return At<uint64_t>(Img, Pos) & IdBits;
        std::string Name = V.Text(Pos + L.AssetNameAt);
        if (!Name.empty() && Name[0] == ',') Name.erase(0, 1);
        return Name.empty() ? 0 : Fnv64(Name);
    };
    // the hash of the asset a pointer field names: its own (at IdAt) when it is here, else the
    // hash the unlinked reference holds
    const auto RefId = [&](int64_t Field, int64_t IdAt) -> uint64_t {
        const uint64_t P = At<uint64_t>(Img, Field);
        if (P == 0) return 0;
        if (const int64_t To = V.Off(P); To >= 0) return L.AssetNameAt >= 0 ? AssetId(To) : At<uint64_t>(Img, To + IdAt) & IdBits;
        return Unlinked.count(Field) ? P & IdBits : 0;
    };
    Json Out;
    Out["zone"] = Zone;
    Json Weapons = Json::array(), Tunables = Json::object(), Pkgs = Json::object(), Suits = Json::object(), Settings = Json::array(),
         Strings = Json::object(), Attachments = Json::object(), AttachmentNames = Json::object();
    std::set<std::pair<std::string, uint64_t>> Done;
    // the tunables from their asset (Tun), or from the weapon's definition and the weapon (a game
    // without tunables assets); a field whose base is not given is left out
    const auto ReadTunables = [&](int64_t Tun, int64_t Def, int64_t Wpn) {
        Json T;
        const auto Base = [&](const Field& F) -> int64_t {
            if (F.At < 0) return -1;
            const int64_t B = F.Base == Field::In::Tunables ? Tun : F.Base == Field::In::Def ? Def : Wpn;
            return B < 0 ? -1 : B + F.At;
        };
        const auto I32 = [&](const char* Key, const Field& F) {
            if (const int64_t P = Base(F); P >= 0) T[Key] = At<int32_t>(Img, P);
        };
        const auto U8 = [&](const char* Key, const Field& F) {
            if (const int64_t P = Base(F); P >= 0) T[Key] = At<uint8_t>(Img, P);
        };
        const auto F32 = [&](const char* Key, const Field& F) {
            if (const int64_t P = Base(F); P >= 0) T[Key] = (double)At<float>(Img, P);
        };
        I32("fire_time", L.FireTime);
        I32("burst_count", L.BurstCount);
        I32("start_ammo", L.StartAmmo);
        I32("start_ammo_override", L.StartAmmoOverride);
        I32("clip_size", L.ClipSize);
        I32("max_ammo", L.MaxAmmo);
        U8("max_ammo_by_clips", L.MaxAmmoByClips);
        U8("fire_type", L.FireType);
        I32("ads_time", L.AdsTime);
        I32("ads_out_time", L.AdsOutTime);
        U8("reload_type", L.ReloadType);
        I32("explosion_radius", L.ExplosionRadius);
        I32("explosion_radius_alt", L.ExplosionRadiusAlt);
        U8("explosion_alt", L.ExplosionAlt);
        F32("muzzle_velocity", L.MuzzleVelocity);
        if (const int64_t P = Base(L.Damage); P >= 0)
        {
            const auto D = [&](int64_t O) { return At<int32_t>(Img, P + O); };
            const auto F = [&](int64_t O) { return (double)At<float>(Img, P + O); };
            T["damage"] = Json::array({ D(0), D(4), D(8), D(0xc), D(0x10) });
            T["ranges"] = Json::array({ F(0x14), F(0x18), F(0x1c), F(0x20) });
        }
        if (const int64_t P = L.HitIsPointer ? Target(Base(L.Hit)) : Base(L.Hit); P >= 0)
        {
            Json Hit = Json::array();
            for (int K = 0; K < 22; K++) Hit.push_back((double)At<float>(Img, P + 4 * K));
            T["hit"] = std::move(Hit);
        }
        F32("sprint_out_time_scale", L.SprintOutScale);
        F32("sprint_speed_scale", L.SprintSpeedScale);
        F32("super_sprint_speed_scale", L.SuperSprintSpeedScale);
        F32("prone_speed_scale", L.ProneSpeedScale);
        return T;
    };
    const bool TunablesAssets = L.DefSets >= 0 || L.DefTunables >= 0;
    // the end of the package time block the layout reads
    int64_t PkgEnd = 0;
    for (const int64_t O : { L.PkgFirstRaise, L.PkgReloadEmpty, L.PkgReload, L.PkgSprintOut, L.PkgSuperSprintOut, L.PkgRaise, L.PkgDrop,
                             L.PkgRaiseQuick, L.PkgReloadAdd, L.PkgReloadEmptyAdd, L.PkgReloadStart, L.PkgReloadEnd })
        PkgEnd = std::max(PkgEnd, O + 2);
    for (const auto& I : R.Instances)
    {
        const std::string Type = TypeOf(I);
        const int64_t P = I.Pos;
        if (Type == "weapon" && L.WeaponDef >= 0)
        {
            const uint64_t Id = AssetId(P);
            const int64_t Def = Target(P + L.WeaponDef);
            if (Def < 0 || !Done.insert({ Type, Id }).second) continue;
            const std::string Name = V.Text(P + L.WeaponName);
            if (Name.empty() || !WantsWeapon(Name)) continue;
            Json W;
            W["id"] = Hex(Id);
            W["name"] = Name;
            if (L.WeaponDisplay >= 0 && !L.DisplayIsKey) W["display"] = Hex(At<uint64_t>(Img, P + L.WeaponDisplay) & IdBits);
            if (L.WeaponDisplay >= 0 && L.DisplayIsKey && L.LocalizeHash)
                if (const std::string Key = V.Text(P + L.WeaponDisplay); !Key.empty()) W["display"] = Hex(L.LocalizeHash(Key));
            if (L.DefClass >= 0)
            {
                const uint8_t Class = At<uint8_t>(Img, Def + L.DefClass);
                if (!L.ClassNames.empty()) W["class"] = Class < L.ClassNames.size() ? L.ClassNames[Class] : std::to_string(Class);
                else W["class"] = Class < std::size(ClassNames) ? ClassNames[Class] : std::to_string(Class);
            }
            if (L.DefAnimPkg >= 0)
                if (const uint64_t Pkg = RefId(Def + L.DefAnimPkg, 0)) W["animpkg"] = Hex(Pkg);
            // the tunables sets and each one's tags ({u32 count, u64 *tags})
            Json Sets = Json::array();
            if (L.DefSets >= 0)
            {
                const int64_t Arr = Target(Def + L.DefSets), Tags = Target(Def + L.DefSetTags);
                const uint32_t Count = std::min<uint32_t>(At<uint32_t>(Img, Def + L.DefSetCount), 16);
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
            }
            if (Sets.empty() && L.DefTunables >= 0)
                if (const uint64_t T = RefId(Def + L.DefTunables, 0)) Sets.push_back(Json{ { "tunables", Hex(T) }, { "tags", Json::array() } });
            // no tunables assets: the weapon's own, under its id
            if (!TunablesAssets)
            {
                Tunables[Hex(Id)] = ReadTunables(-1, Def, P);
                N.Tunables++;
                Sets.push_back(Json{ { "tunables", Hex(Id) }, { "tags", Json::array() } });
            }
            W["sets"] = std::move(Sets);
            // the attachments by slot: {attachment **, u32 count}; a condition's index k names the slot's k-th
            if (L.Slots >= 0)
            {
                Json Slots = Json::array();
                for (int64_t S = 0; S < L.SlotCount; S++)
                {
                    Json List = Json::array();
                    const int64_t A = Target(P + L.Slots + L.SlotList + S * 0x10);
                    const uint32_t N2 = std::min<uint32_t>(At<uint32_t>(Img, P + L.Slots + L.SlotCountAt + S * 0x10), 128);
                    for (uint32_t K = 0; A >= 0 && K < N2; K++) List.push_back(Hex(RefId(A + 8 * (int64_t)K, 0)));
                    Slots.push_back(std::move(List));
                }
                W["slots"] = std::move(Slots);
            }
            // the default attachment of each slot from 1 ({slot, attachment id}; none: left out)
            if (L.DefaultAttachments >= 0)
            {
                Json Defaults = Json::array();
                for (int64_t S = 1; S < L.SlotCount; S++)
                    if (const uint64_t A = RefId(P + L.DefaultAttachments + 8 * S, 0)) Defaults.push_back(Json{ { "slot", S }, { "attachment", Hex(A) } });
                W["defaults"] = std::move(Defaults);
            }
            // the animation package overrides (0x30 each): u32 group count (the rank: more groups wins)
            // at 0, the groups at 8 (0x18 each: u32 count, conditions at 8 (0x14 each: u8 slot, u32
            // mask[4] of slot indexes), i32 variant at 0x10, -1 any), the package at 0x10
            if (L.DefOverrides >= 0)
            {
                Json Ovr = Json::array();
                const int64_t OArr = Target(Def + L.DefOverrides);
                const uint32_t ON = std::min<uint32_t>(At<uint32_t>(Img, Def + L.DefOverrideCount), 256);
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
                        for (uint32_t K2 = 0; C >= 0 && K2 < std::min<uint32_t>(At<uint32_t>(Img, Gr), 16); K2++)
                        {
                            const int64_t Cd = C + 0x14 * (int64_t)K2;
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
            }
            Weapons.push_back(std::move(W));
            N.Weapons++;
        }
        else if (Type == "weapontunables" && TunablesAssets)
        {
            const uint64_t Id = At<uint64_t>(Img, P) & IdBits;
            if (!Done.insert({ Type, Id }).second) continue;
            Tunables[Hex(Id)] = ReadTunables(P, -1, -1);
            N.Tunables++;
        }
        else if (Type == "attachment" && L.AttachmentName >= 0)
        {
            const uint64_t Id = AssetId(P);
            if (!Done.insert({ Type, Id }).second) continue;
            const std::string Name = V.Text(P + L.AttachmentName);
            if (Name.empty()) continue;
            Json A;
            A["name"] = Name;
            // its own models (the part with no blueprint): xmodel name hashes, 0 none
            const auto Model = [&](const char* Key, int64_t Off) {
                if (Off < 0) return;
                if (const uint64_t H = At<uint64_t>(Img, P + Off) & IdBits; H != 0)
                {
                    A[Key] = L.ModelName ? L.ModelName(H) : std::format("xmodel_{:x}", H);
                    A[std::string(Key) + "_id"] = Hex(H);
                }
            };
            Model("world_model", L.AttWorldModel);
            Model("view_model", L.AttViewModel);
            // clip: the attachments' (scale - 1) added; the i32 at 8 of the override block when >= 0
            if (L.AttClipScale >= 0)
            {
                const float Scale = At<float>(Img, P + L.AttClipScale);
                A["clip_size_scale"] = L.AttZeroScaleIsNone && Scale == 0.0f ? 1.0 : (double)Scale;
            }
            if (L.AttClipOverride >= 0)
            {
                const int64_t Ov = Target(P + L.AttClipOverride);
                A["clip_size_override"] = Ov >= 0 ? At<int32_t>(Img, Ov + 8) : -1;
            }
            // the modifier table (pointers to small blocks)
            if (L.AttModifiers >= 0)
            {
                const int64_t T = Target(P + L.AttModifiers);
                const auto Sub = [&](int64_t O) { return T >= 0 ? Target(T + O) : -1; };
                const int64_t B08 = Sub(0x08), B18 = Sub(0x18), B68 = Sub(0x68), B90 = Sub(0x90), BD0 = Sub(0xd0);
                // sprint-out time scale B += (0x08 block + 0x18) - 1; C += (int)(1000 x (0x68 block + 4))
                A["sprint_out_time_scale"] = B08 >= 0 ? (double)At<float>(Img, B08 + 0x18) : 1.0;
                A["sprint_out_time_add"] = B68 >= 0 ? (double)At<float>(Img, B68 + 4) : 0.0;
                // the 4 aim-down-sight times' additions (0x90 block + 4..0x10, seconds) and the divisor
                // term (0x18 block + 4)
                Json Ads = Json::array();
                for (int K = 0; K < 4; K++) Ads.push_back(B90 >= 0 ? (double)At<float>(Img, B90 + 4 + 4 * K) : 0.0);
                A["ads_time_add"] = std::move(Ads);
                A["ads_speed_scale"] = B18 >= 0 ? (double)At<float>(Img, B18 + 4) : 1.0;
                // the first attachment with an ADS override block replaces the 4 ADS times (seconds)
                if (L.AttAdsOverride >= 0)
                    if (const int64_t B = Target(P + L.AttAdsOverride); B >= 0)
                        A["ads_time_override"] = Json::array({ (double)At<float>(Img, B), (double)At<float>(Img, B + 4),
                                                               (double)At<float>(Img, B + 8), (double)At<float>(Img, B + 0xc) });
                // the muzzle velocity times each attachment's 0xd0 block + 4
                A["velocity_scale"] = BD0 >= 0 ? (double)At<float>(Img, BD0 + 4) : 1.0;
            }
            // the first attachment with an explosion block gives the explosion radius (i32 +0, the alternate)
            if (L.AttExplosion >= 0)
                if (const int64_t B = Target(P + L.AttExplosion); B >= 0)
                {
                    Json R = Json::array({ At<int32_t>(Img, B) });
                    if (L.AttExplosionAlt >= 0) R.push_back(At<int32_t>(Img, B + L.AttExplosionAlt));
                    A["explosion_radius_override"] = std::move(R);
                }
            Attachments[Hex(Id)] = std::move(A);
            N.Attachments++;
        }
        else if (Type == "animpkg" && L.PkgBlock >= 0)
        {
            const uint64_t Id = AssetId(P);
            const int64_t B = Target(P + L.PkgBlock);
            if (B < 0 || B + PkgEnd > (int64_t)Img.size() || !Done.insert({ Type, Id }).second) continue;
            Json K = Json::object();   // (an object even with every time none)
            const auto U16 = [&](const char* Key, int64_t O) {
                if (O >= 0 && !(L.PkgFfffIsNone && At<uint16_t>(Img, B + O) == 0xffff)) K[Key] = At<uint16_t>(Img, B + O);
            };
            U16("first_raise", L.PkgFirstRaise);
            U16("reload_empty", L.PkgReloadEmpty);
            U16("reload", L.PkgReload);
            U16("sprint_out", L.PkgSprintOut);
            U16("super_sprint_out", L.PkgSuperSprintOut);
            U16("raise", L.PkgRaise);
            U16("drop", L.PkgDrop);
            U16("raise_quick", L.PkgRaiseQuick);
            U16("reload_add", L.PkgReloadAdd);
            U16("reload_empty_add", L.PkgReloadEmptyAdd);
            U16("reload_start", L.PkgReloadStart);
            U16("reload_end", L.PkgReloadEnd);
            Pkgs[Hex(Id)] = std::move(K);
            N.AnimPkgs++;
        }
        else if (Type == "suit" && L.SuitId >= 0)
        {
            const uint64_t Id = At<uint64_t>(Img, P + L.SuitId) & IdBits;
            if (!Done.insert({ Type, Id }).second) continue;
            Json S;
            if (L.SuitSprint >= 0) S["sprint_speed_multiplier"] = (double)At<float>(Img, P + L.SuitSprint);
            if (L.SuitSuperSprint >= 0) S["super_sprint_speed_multiplier"] = (double)At<float>(Img, P + L.SuitSuperSprint);
            Suits[Hex(Id)] = std::move(S);
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
                if (L.TagKey != 0 && Key == L.TagKey) S["tag"] = Hex(At<uint64_t>(Img, E + 8) & IdBits);
                else if (L.AttachmentKey != 0 && Key == L.AttachmentKey)
                {
                    if (const int64_t Rec = V.Off(At<uint64_t>(Img, E + 8)); Rec >= 0) S["attachment_name"] = V.Text(Rec);
                }
                else if (L.NameKey != 0 && Key == L.NameKey && At<uint8_t>(Img, E) == 4) S["name"] = Hex(At<uint64_t>(Img, E + 8) & IdBits);
                else if (L.SuitKey != 0 && Key == L.SuitKey)
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
            // (a game naming its assets by string: the key string's hash, as the weapon's display key)
            const uint64_t Id = L.AssetNameAt >= 0 && L.LocalizeHash ? L.LocalizeHash(V.Text(P)) & L.StringIdMask
                                                                     : At<uint64_t>(Img, P) & L.StringIdMask;
            if (!Done.insert({ Type, Id }).second) continue;
            const std::string S = V.LocalizeText(P);
            if (S.empty()) continue;
            // (appended: the ids are unique (Done), and the ordered object's search per insert is
            // linear, minutes for a language zone's 45 000 strings)
            Strings.get_ref<Json::object_t&>().emplace_back(Hex(Id), S);
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

bool Merge(const Layout& L, const std::string& Dir, size_t& Rows, std::string& Error)
{
    Rows = 0;
    // (std::map-backed: the facts hold tens of thousands of strings, the ordered map's search is linear)
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
        if (Name.empty() || !L.NameHash) return nullptr;
        const std::string Id = Hex(L.NameHash(Name));
        return Suits.contains(Id) ? &Suits[Id] : nullptr;
    };

    std::vector<std::string> Head = { "weapon", "display_name", "class", "set", "tags", "fire_time_ms", "rpm", "burst_count",
                                      "fire_type", "clip_size", "start_ammo", "max_ammo", "reload_ms", "reload_empty_ms",
                                      "reload_add_ms", "reload_empty_add_ms", "reload_type", "reload_start_ms", "reload_end_ms",
                                      "first_raise_ms", "raise_ms", "raise_quick_ms", "drop_ms", "sprint_to_fire_ms",
                                      "super_sprint_to_fire_ms", "ads_time_ms", "ads_out_time_ms", "muzzle_velocity_mps",
                                      "explosion_radius_m", "damage_max", "range_max_m", "damage_mid1", "range_mid1_m",
                                      "damage_mid2", "range_mid2_m", "damage_mid3", "range_mid3_m", "damage_min" };
    for (size_t K = 1; K < std::size(HitLocations); K++) Head.push_back(std::string("hit_") + HitLocations[K]);
    for (const char* H : { "sprint_speed_scale", "super_sprint_speed_scale", "prone_speed_scale", "suit", "sprint_mps",
                           "super_sprint_mps", "sprint_mps_mp_suit", "super_sprint_mps_mp_suit" })
        Head.push_back(H);

    // the UI's speed: base x suit multiplier(s), through the weapon's scale entry (max(add, 0) x mul
    // x speed + offset; mul = the tunables scale, add 1, offset 0)
    const auto Speed = [&](const Map* S, const Map& T, const char* ScaleKey, bool Super) -> std::string {
        if (S == nullptr || L.BaseSpeed <= 0.0f || !T.contains(ScaleKey) || !S->contains("sprint_speed_multiplier") ||
            (Super && !S->contains("super_sprint_speed_multiplier")))
            return "";
        float V = L.BaseSpeed * (float)(*S)["sprint_speed_multiplier"].get<double>();
        if (Super) V *= (float)(*S)["super_sprint_speed_multiplier"].get<double>();
        return Num((double)((float)T[ScaleKey].get<double>() * V / InchesPerMetre), 2);
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
        const std::string Display = W.contains("display") && Strings.contains(W["display"].get<std::string>())
                                        ? Strings[W["display"].get<std::string>()].get<std::string>()
                                        : "";
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
                Tags.push_back(TagName(L, std::stoull(Tag.get<std::string>(), nullptr, 16)));
                if (const auto It = SuitsOfTag.find(Tag.get<std::string>()); It != SuitsOfTag.end()) RowSuits.insert(It->second.begin(), It->second.end());
            }
            std::string TagText, SuitName;
            for (const auto& X : Tags) TagText += (TagText.empty() ? "" : " ") + X;
            if (RowSuits.size() == 1) SuitName = *RowSuits.begin();
            const auto IntCell = [&](const char* Key) { return T.contains(Key) ? std::to_string(T[Key].get<int>()) : std::string(); };
            const auto FloatCell = [&](const char* Key, int Digits) { return T.contains(Key) ? Num(T[Key].get<double>(), Digits) : std::string(); };
            const int Fire = IntOr(T, "fire_time", 0);
            // the fire type: 0 auto (weaponisauto), 2 burst (weaponburstcount returns the count only for
            // 2), 5 beam (weaponisbeam), 1, 3, 4 semi-auto (weaponissemiauto); 3 fires two rounds a pull
            std::string FireText;
            if (T.contains("fire_type"))
            {
                const int FireType = T["fire_type"].get<int>();
                const char* const FireNames[] = { "auto", "semi-auto", "burst", "semi-auto double", "semi-auto", "beam" };
                FireText = FireType >= 0 && FireType < (int)std::size(FireNames) ? std::format("{} ({})", FireNames[FireType], FireType)
                                                                                : std::to_string(FireType);
            }
            std::string Start;
            if (T.contains("start_ammo"))
            {
                const int StartOverride = IntOr(T, "start_ammo_override", 0);
                Start = std::to_string(StartOverride != 0 ? StartOverride : T["start_ammo"].get<int>());
            }
            const auto Stf = [&](const char* Field) -> std::string {
                if (Pkg == nullptr || !Pkg->contains(Field) || !T.contains("sprint_out_time_scale")) return "";
                const float Scaled = (float)(*Pkg)[Field].get<int>() * (float)T["sprint_out_time_scale"].get<double>();
                return std::to_string(std::max(1, (int)Scaled));
            };
            const auto PkgTime = [&](const char* Field) {
                return Pkg == nullptr || !Pkg->contains(Field) ? std::string() : std::to_string((*Pkg)[Field].get<int>());
            };
            const int ReloadType = T.value("reload_type", -1);
            // the per-round reload's start and end exist only for reload type 1
            const auto RoundTime = [&](const char* Field) { return ReloadType == 1 ? PkgTime(Field) : std::string(); };
            // explosion radius in inches, 0 when the weapon has none
            const int Radius = T.value("explosion_alt", 0) != 0 ? T.value("explosion_radius_alt", 0) : T.value("explosion_radius", 0);
            std::vector<std::string> Dmg(9);
            if (T.contains("damage"))
            {
                const auto& D = T["damage"];
                const auto& Rg = T["ranges"];
                const auto Metres = [&](size_t I) {
                    return Rg[I].is_number() ? Num((double)((float)Rg[I].get<double>() / InchesPerMetre), 2) : std::string();
                };
                Dmg = { std::to_string(D[4].get<int>()), Metres(0), std::to_string(D[1].get<int>()), Metres(1), std::to_string(D[2].get<int>()),
                        Metres(2), std::to_string(D[3].get<int>()), Metres(3), std::to_string(D[0].get<int>()) };
            }
            std::string MaxAmmo;
            if (T.contains("max_ammo") && IntOr(T, "max_ammo_by_clips", 0) == 0) MaxAmmo = std::to_string(T["max_ammo"].get<int>());
            std::vector<std::string> C = { Name, Display, W.value("class", std::string()), std::to_string(K), TagText, IntCell("fire_time"),
                                           Fire > 0 ? Num(60000.0 / Fire, 2) : "", IntCell("burst_count"), FireText, IntCell("clip_size"),
                                           Start, MaxAmmo, PkgTime("reload"), PkgTime("reload_empty"), PkgTime("reload_add"),
                                           PkgTime("reload_empty_add"), ReloadType < 0 ? "" : std::to_string(ReloadType), RoundTime("reload_start"),
                                           RoundTime("reload_end"), PkgTime("first_raise"), PkgTime("raise"), PkgTime("raise_quick"),
                                           PkgTime("drop"), Stf("sprint_out"), Stf("super_sprint_out"), IntCell("ads_time"),
                                           IntCell("ads_out_time"), FloatCell("muzzle_velocity", 2),
                                           Radius > 0 ? Num((double)((float)Radius / InchesPerMetre), 2) : "" };
            C.insert(C.end(), Dmg.begin(), Dmg.end());
            // (a NaN in the facts is null: its cell empty)
            for (size_t H = 1; H < std::size(HitLocations); H++)
                C.push_back(T.contains("hit") && T["hit"][H].is_number() ? Num(T["hit"][H].get<double>(), 4) : "");
            C.push_back(FloatCell("sprint_speed_scale", 4));
            C.push_back(FloatCell("super_sprint_speed_scale", 4));
            C.push_back(FloatCell("prone_speed_scale", 4));
            const Map* RowSuit = SuitName.empty() ? nullptr : Suit(SuitName);
            C.push_back(RowSuit != nullptr ? SuitName : "");
            C.push_back(Speed(RowSuit, T, "sprint_speed_scale", false));
            C.push_back(Speed(RowSuit, T, "super_sprint_speed_scale", true));
            C.push_back(Speed(Suit(L.MpSuit), T, "sprint_speed_scale", false));
            C.push_back(Speed(Suit(L.MpSuit), T, "super_sprint_speed_scale", true));
            Table.push_back({ Name, K, std::move(C) });
        }
    }
    std::sort(Table.begin(), Table.end(), [](const Row& A, const Row& B) { return A.Weapon != B.Weapon ? A.Weapon < B.Weapon : A.Set < B.Set; });

    std::string Csv = WeaponStatsTable::Line(Head);
    Json Out = Json::array();
    for (const auto& R : Table) WeaponStatsTable::AddRow(Head, R.Cells, Csv, Out);
    const std::string Title = L.Title + " (GrayWolf --type weaponstats)";
    std::string Readme = Title + "\n" + std::string(Title.size(), '=') + "\n\n" +
                         "One row per weapon and tunables set. Only fields whose meaning the game's code proves are here;\n"
                         "the raw struct dumps (--type weapons) keep everything else, by offset.\n\n"
                         "Zones read so far (facts/): ";
    for (size_t I = 0; I < Zones.size(); I++) Readme += (I ? ", " : "") + Zones[I];
    Readme += L.Columns;
    std::string AttCsv, LoadCsv;
    const size_t AttRows = AttachmentTables(Weapons, Tunables, Pkgs, Attachments, AttachmentNames, Strings, Dir, AttCsv, LoadCsv);
    Readme += L.AttachmentNotes;
    std::string PartsCsv;
    const size_t PartRows = L.DefaultAttachments >= 0 ? DefaultPartsTable(L, Weapons, Attachments, AttachmentNames, Strings, PartsCsv) : 0;
    if (PartRows != 0) Readme += L.DefaultPartsNotes;
    std::error_code E2;
    fs::create_directories(Dir, E2);
    if (AttRows != 0)
    {
        ExportWriters::WriteBytes(fs::path(Dir) / "weapon_attachments.csv", (const uint8_t*)AttCsv.data(), AttCsv.size());
        if (!LoadCsv.empty()) ExportWriters::WriteBytes(fs::path(Dir) / "weapon_loadouts.csv", (const uint8_t*)LoadCsv.data(), LoadCsv.size());
    }
    if (PartRows != 0) ExportWriters::WriteBytes(fs::path(Dir) / "weapon_default_parts.csv", (const uint8_t*)PartsCsv.data(), PartsCsv.size());
    const std::string JsonText = Out.dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
    const bool Ok = ExportWriters::WriteBytes(fs::path(Dir) / "weapon_stats.csv", (const uint8_t*)Csv.data(), Csv.size()) &&
                    ExportWriters::WriteBytes(fs::path(Dir) / "weapon_stats.json", (const uint8_t*)JsonText.data(), JsonText.size()) &&
                    ExportWriters::WriteBytes(fs::path(Dir) / "README.txt", (const uint8_t*)Readme.data(), Readme.size());
    if (!Ok) Error = "write failed in " + Dir;
    Rows = Table.size();
    if (Ok) LastFacts = Stamp, Merged = Rows;
    return Ok;
}

std::map<std::string, ZonekitZone::DumpField> DumpFields(const Layout& L, const std::string& Type)
{
    std::map<std::string, ZonekitZone::DumpField> F;
    const auto Key = [](int64_t At) { return std::format("0x{:04x}", At); };
    const auto Put = [&](const std::string& Prefix, int64_t At, const std::string& Name, const char* Kind) {
        if (At >= 0) F[Prefix + Key(At)] = { Name, Kind };
    };
    // a u8 field: named on the 4-byte word holding it (the dump's keys), several joined by '.'
    std::map<std::string, std::string> Bytes;
    const auto Byte = [&](const std::string& Prefix, int64_t At, const std::string& Name) {
        if (At < 0) return;
        auto& N = Bytes[Prefix + Key(At & ~3)];
        N += (N.empty() ? "" : ".") + std::format("{}_u8@{}", Name, At & 3);
    };
    // the tunables fields of this asset type: a weapontunables asset's, or a weapon's (its own and
    // its definition's, under the definition's key)
    const auto Tunables = [&](bool Weapon) {
        const auto Prefix = [&](const Field& F) -> std::string {
            if (F.At < 0) return "-";
            if (!Weapon) return F.Base == Field::In::Tunables ? "" : "-";
            if (F.Base == Field::In::Weapon) return "";
            return F.Base == Field::In::Def && L.WeaponDef >= 0 ? Key(L.WeaponDef) + "/" : "-";
        };
        const auto Name = [&](const Field& F, int64_t Add, const std::string& N, const char* Kind) {
            if (const std::string P = Prefix(F); P != "-") Put(P, F.At + Add, N, Kind);
        };
        for (const auto& [F, N] : { std::pair<const Field*, const char*>{ &L.FireTime, "fire_time" }, { &L.BurstCount, "burst_count" },
                                    { &L.StartAmmo, "start_ammo" }, { &L.StartAmmoOverride, "start_ammo_override" }, { &L.ClipSize, "clip_size" },
                                    { &L.MaxAmmo, "max_ammo" }, { &L.AdsTime, "ads_time" }, { &L.AdsOutTime, "ads_out_time" },
                                    { &L.ExplosionRadius, "explosion_radius" }, { &L.ExplosionRadiusAlt, "explosion_radius_alt" } })
            Name(*F, 0, N, "i32");
        for (const auto& [F, N] : { std::pair<const Field*, const char*>{ &L.MuzzleVelocity, "muzzle_velocity" },
                                    { &L.SprintOutScale, "sprint_out_time_scale" }, { &L.SprintSpeedScale, "sprint_speed_scale" },
                                    { &L.SuperSprintSpeedScale, "super_sprint_speed_scale" }, { &L.ProneSpeedScale, "prone_speed_scale" } })
            Name(*F, 0, N, "f32");
        for (const auto& [F, N] : { std::pair<const Field*, const char*>{ &L.FireType, "fire_type" }, { &L.MaxAmmoByClips, "max_ammo_by_clips" },
                                    { &L.ReloadType, "reload_type" }, { &L.ExplosionAlt, "explosion_alt" } })
            if (const std::string P = Prefix(*F); P != "-") Byte(P, F->At, N);
        static const char* Ints[] = { "damage_min", "damage_mid1", "damage_mid2", "damage_mid3", "damage_max" };
        static const char* Floats[] = { "range_max_damage", "range_mid1", "range_mid2", "range_mid3" };
        for (int K = 0; K < 5; K++) Name(L.Damage, K * 4, Ints[K], "i32");
        for (int K = 0; K < 4; K++) Name(L.Damage, 0x14 + K * 4, Floats[K], "f32");
        if (!L.HitIsPointer)
            for (int K = 0; K < 22; K++) Name(L.Hit, K * 4, std::string("hit_multiplier_") + HitLocations[K], "f32");
        else if (const std::string P = Prefix(L.Hit); P != "-")
        {
            // the pointer, and the block it leads to (its fields under the pointer's path)
            Put(P, L.Hit.At, "hit_multipliers", "");
            for (int K = 0; K < 22; K++) Put(P + Key(L.Hit.At) + "/", K * 4, std::string("hit_multiplier_") + HitLocations[K], "f32");
        }
    };
    if (Type == "weapon")
    {
        Put("", L.WeaponName, "name", "");
        Put("", L.WeaponDef, "weapon_def", "");
        Put("", L.WeaponDisplay, "display_name", "");
        Put("", L.Slots, "attachment_slots", "");
        if (L.DefaultAttachments >= 0)
            for (int64_t S = 1; S < L.SlotCount; S++) Put("", L.DefaultAttachments + 8 * S, std::format("default_attachment_slot{}", S), "");
        if (L.WeaponDef >= 0)
        {
            const std::string D = Key(L.WeaponDef) + "/";
            Put(D, L.DefTunables, "tunables", "");
            Put(D, L.DefSets, "tunables_sets", "");
            Put(D, L.DefSetCount, "tunables_set_count", "i32");
            Put(D, L.DefSetTags, "tunables_set_tags", "");
            Put(D, L.DefAnimPkg, "animpkg", "");
            Put(D, L.DefOverrides, "animpkg_overrides", "");
            Put(D, L.DefOverrideCount, "animpkg_override_count", "i32");
            Byte(D, L.DefClass, "class");
        }
        Tunables(true);
    }
    else if (Type == "weapontunables")
        Tunables(false);
    else if (Type == "attachment")
    {
        Put("", L.AttachmentName, "name", "");
        Put("", L.AttClipScale, "clip_size_scale", "f32");
        Put("", L.AttClipOverride, "clip_size_override", "");
        Put("", L.AttModifiers, "modifiers", "");
        Put("", L.AttAdsOverride, "ads_time_override", "");
        Put("", L.AttExplosion, "explosion_radius_override", "");
        Put("", L.AttWorldModel, "world_model", "hash");
        Put("", L.AttViewModel, "view_model", "hash");
    }
    else if (Type == "animpkg" && L.PkgBlock >= 0)
    {
        // u16 times, two to a word: "low|high" ("_" a time not named)
        std::map<int64_t, std::pair<std::string, std::string>> Words;
        for (const auto& [At, Name] : { std::pair<int64_t, const char*>{ L.PkgFirstRaise, "first_raise_time" }, { L.PkgReloadEmpty, "reload_empty_time" },
                                        { L.PkgReload, "reload_time" }, { L.PkgSprintOut, "sprint_out_time" },
                                        { L.PkgSuperSprintOut, "super_sprint_out_time" }, { L.PkgRaise, "raise_time" }, { L.PkgDrop, "drop_time" },
                                        { L.PkgRaiseQuick, "raise_quick_time" }, { L.PkgReloadAdd, "reload_add_time" },
                                        { L.PkgReloadEmptyAdd, "reload_empty_add_time" }, { L.PkgReloadStart, "reload_start_time" },
                                        { L.PkgReloadEnd, "reload_end_time" } })
            if (At >= 0)
            {
                auto& W = Words[At & ~3];
                (At & 2 ? W.second : W.first) = Name;
            }
        for (const auto& [At, W] : Words)
            F[Key(L.PkgBlock) + "/" + Key(At)] = { (W.first.empty() ? "_" : W.first) + "|" + (W.second.empty() ? "_" : W.second), "u16x2" };
    }
    else if (Type == "suit")
    {
        Put("", L.SuitSprint, "sprint_speed_multiplier", "f32");
        Put("", L.SuitSuperSprint, "super_sprint_speed_multiplier", "f32");
    }
    for (const auto& [K, N] : Bytes)
        if (!F.count(K)) F[K] = { N, "" };
    return F;
}

void Export(const Layout& L, const ZoneView& V, const std::string& Zone, const DiskGame::Context& C, DiskGame::Totals& T)
{
    using DiskGame::Kind;
    auto& Kt = T.Kinds[Kind::WeaponStats];
    FactsCount N;
    const std::string Text = Facts(L, V, Zone, [&C](const std::string& Name) { return C.Wants(Kind::WeaponStats, Name); }, N);
    Kt.Seen += N.Weapons;
    if (N.Weapons + N.Tunables + N.AnimPkgs + N.Suits + N.Settings + N.Strings + N.Attachments == 0) return;
    WeaponStatsTable::WriteFacts(Zone, Text, C, T, [L](const std::string& Dir, size_t& Rows, std::string& Error) {
        return Merge(L, Dir, Rows, Error);
    });
}
}
