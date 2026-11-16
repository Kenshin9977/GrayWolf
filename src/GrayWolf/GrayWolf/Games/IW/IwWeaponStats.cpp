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
    // an attachment of the alternate mode slot that is an alternate mode (not a normal attachment)
    bool IsAltMode(const IwWeaponStats::Layout& L, int Slot, const Map& A)
    {
        return L.AltSlot >= 0 && Slot == L.AltSlot && A.contains("alt_normal") && A["alt_normal"].get<int>() == 0;
    }

    // the stats of a weapon with attachments (slot, index) chosen; AltActive: with the alternate
    // mode (the alternate slot's attachment) active
    std::vector<std::string> LoadoutStats(const IwWeaponStats::Layout& L, const Weapon& Wp, const Map& Pkgs, const Map& Attachments, const std::vector<std::pair<int, int>>& Picks,
                                          std::string& Unknown, bool AltActive = false)
    {
        const Map& W = *Wp.W;
        const Map& T = *Wp.T;
        // the attachments in the game's gather order (its first holding a field wins); an alternate mode
        // counts only while active, then first, and alone unless it keeps the others
        std::vector<std::pair<int, const Map*>> Picked;
        for (const auto& [Slot, Index] : Picks)
        {
            const auto& List = W["slots"][(size_t)Slot];
            const std::string Id = Index < (int)List.size() ? List[(size_t)Index].get<std::string>() : "";
            if (Attachments.contains(Id)) Picked.push_back({ Slot, &Attachments[Id] });
            else Unknown += (Unknown.empty() ? "" : " ") + std::format("slot{}#{}", Slot, Index);
        }
        if (!L.SlotOrder.empty())
        {
            const auto Rank = [&](int Slot) {
                const auto It = std::find(L.SlotOrder.begin(), L.SlotOrder.end(), Slot);
                return (int)(It - L.SlotOrder.begin());
            };
            std::stable_sort(Picked.begin(), Picked.end(), [&](const auto& A, const auto& B) { return Rank(A.first) < Rank(B.first); });
        }
        std::vector<const Map*> Atts;
        const Map* Alt = nullptr;
        for (const auto& [Slot, A] : Picked)
            if (IsAltMode(L, Slot, *A)) Alt = A;
            else Atts.push_back(A);
        if (Alt != nullptr && AltActive && IntOr(*Alt, "alt_active", 0) != 0)
        {
            if (IntOr(*Alt, "alt_with_others", 0) == 0) Atts.clear();
            Atts.insert(Atts.begin(), Alt);
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
        // an override the active alternate mode alone may give (u8 AttAltMode 1)
        const auto Overrides = [&](const Map* A) { return !A->contains("alt_only_overrides") || (A == Alt && AltActive); };
        int Clip = -1;
        float ClipScale = 1.0f;
        if (L.ClipOverrideScaled)
        {
            // (0x4637cbd) the first attachment's override replaces the base clip, then x max(0, 1 + sum(scale
            // - 1)) (within 1e-6 of 1: 1), truncated
            int Base = T.contains("clip_size") ? T["clip_size"].get<int>() : -1;
            for (const Map* A : Atts)
                if (IntOr(*A, "clip_size_override", -1) >= 0)
                {
                    Base = (*A)["clip_size_override"].get<int>();
                    break;
                }
            for (const Map* A : Atts) ClipScale += (float)FloatOr(*A, "clip_size_scale", 1.0) - 1.0f;
            ClipScale = std::max(ClipScale, 0.0f);
            if (std::fabs(1.0f - ClipScale) <= 1e-6f) ClipScale = 1.0f;
            if (Base >= 0) Clip = (int)((float)Base * ClipScale);
        }
        else
        {
            for (const Map* A : Atts)
            {
                if (IntOr(*A, "clip_size_override", -1) >= 0) Clip = (*A)["clip_size_override"].get<int>();
                ClipScale += (float)FloatOr(*A, "clip_size_scale", 1.0) - 1.0f;
            }
            if (Clip < 0 && T.contains("clip_size")) Clip = (int)((float)T["clip_size"].get<int>() * ClipScale);
        }
        // max ammo (0x46690c0): the first attachment with an ammo block decides: by magazines (its reserve
        // magazines when > 0, else the weapon's) or its max ammo when >= 0; the weapon's otherwise. By
        // magazines: clip x magazines; else (int)(max x max(0, 1 + sum(scale - 1)))
        std::string MaxAmmo;
        if (L.AttMaxAmmoScale >= 0 && T.contains("max_ammo"))
        {
            bool ByClips = IntOr(T, "max_ammo_by_clips", 0) != 0;
            int Max = T["max_ammo"].get<int>(), Reserve = IntOr(T, "reserve_clips", 0);
            for (const Map* A : Atts)
                if (A->contains("ammo_override"))
                {
                    const Map& O = (*A)["ammo_override"];
                    if (O["by_clips"].get<int>() != 0)
                    {
                        ByClips = true;
                        if (O["reserve_clips"].get<int>() > 0) Reserve = O["reserve_clips"].get<int>();
                    }
                    else
                    {
                        ByClips = false;
                        if (O["max_ammo"].get<int>() >= 0) Max = O["max_ammo"].get<int>();
                    }
                    break;
                }
            if (ByClips) MaxAmmo = Clip >= 0 ? std::to_string(Clip * Reserve) : "";
            else
            {
                float S = 1.0f;
                for (const Map* A : Atts) S += (float)FloatOr(*A, "max_ammo_scale", 1.0) - 1.0f;
                S = std::max(S, 0.0f);
                if (std::fabs(1.0f - S) <= 1e-6f) S = 1.0f;
                MaxAmmo = std::to_string((int)((float)Max * S));
            }
        }
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
        std::vector<std::string> Out = { Int(Clip), Int(Time("reload")), Int(Time("reload_empty")), Int(Time("reload_add")), Int(Time("reload_empty_add")),
                                         Stf("sprint_out"), Stf("super_sprint_out"), AdsTime(0, "ads_time"), AdsTime(1, "ads_out_time"), Vel };
        if (L.AttMaxAmmoScale >= 0) Out.insert(Out.begin() + 1, MaxAmmo);
        // pellets (0x4676de0): the first attachment whose ammo block holds a count >= 0 replaces the tunables'
        if (L.Pellets.At >= 0 && L.AttAmmoPellets >= 0)
        {
            int Pellets = IntOr(T, "pellets", -1);
            // (0x7340fd2: one projectile unless the class (0x46542a0: the first attachment's override, else the
            // weapon's) is the spread class, 0x4680e40)
            bool Spread = L.SpreadClass < 0 || (W.contains("class") && W["class"].get<std::string>() == ClassNames[L.SpreadClass]);
            if (L.AttClass >= 0)
                for (const Map* A : Atts)
                    if (A->contains("class_override"))
                    {
                        Spread = (*A)["class_override"].get<int>() == L.SpreadClass;
                        break;
                    }
            for (const Map* A : Atts)
                if (IntOr(*A, "pellets_override", -1) >= 0)
                {
                    Pellets = (*A)["pellets_override"].get<int>();
                    break;
                }
            Out.insert(Out.begin() + (L.AttMaxAmmoScale >= 0 ? 2 : 1), Int(Pellets >= 0 && !Spread ? 1 : Pellets));
        }
        // the fire time (0x463a9b0, no player): the first attachment's override replaces the tunables',
        // then x max(0, 1 + sum(scale - 1)) (within 1e-6 of 1: 1), truncated
        if (L.AttFireTime >= 0)
        {
            std::string Ms, Rpm;
            if (T.contains("fire_time"))
            {
                int Base = T["fire_time"].get<int>();
                for (const Map* A : Atts)
                    if (A->contains("fire_time_override") && Overrides(A))
                    {
                        Base = (*A)["fire_time_override"].get<int>();
                        break;
                    }
                float S = 1.0f;
                for (const Map* A : Atts) S += (float)FloatOr(*A, "fire_time_scale", 1.0) - 1.0f;
                S = std::max(S, 0.0f);
                if (std::fabs(1.0f - S) <= 1e-6f) S = 1.0f;
                const int Fire = (int)(S * (float)Base);
                Ms = std::to_string(Fire);
                if (Fire > 0) Rpm = Num(60000.0 / Fire, 2);
            }
            Out.push_back(Ms);
            Out.push_back(Rpm);
        }
        // the burst delay: the first attachment's override replaces the tunables' (its own scale then
        // left out), the others' scales add their deviation; floor(t x scale + 0.5)
        if (L.BurstDelay.At >= 0)
        {
            std::string Cell;
            if (T.contains("burst_delay"))
            {
                float Base = (float)T["burst_delay"].get<double>(), S = 1.0f;
                bool Overridden = false;
                for (const Map* A : Atts)
                {
                    if (!Overridden && A->contains("burst_delay_override") && Overrides(A))
                    {
                        Base = (float)(*A)["burst_delay_override"].get<int>();
                        Overridden = true;
                        continue;
                    }
                    S += (float)FloatOr(*A, "burst_delay_scale", 1.0) - 1.0f;
                }
                Cell = std::to_string((int)std::floor(Snap(std::max(S, 0.0f)) * Base + 0.5f));
            }
            Out.push_back(Cell);
        }
        // the explosion: the first attachment with an explosion block gives the radius, the damages
        // and the three-ring flag
        if (L.ExplosionDamage.At >= 0)
        {
            const Map* Block = nullptr;
            for (const Map* A : Atts)
                if (A->contains("explosion_radius_override"))
                {
                    Block = A;
                    break;
                }
            const auto Value = [&](const char* Own, const char* Override) -> const Map* {
                if (Block != nullptr) return Block->contains(Override) ? &(*Block)[Override] : nullptr;
                return T.contains(Own) ? &T[Own] : nullptr;
            };
            const bool Alt = Block != nullptr ? IntOr(*Block, "explosion_alt_override", 0) != 0 : IntOr(T, "explosion_alt", 0) != 0;
            int Radius = 0;
            if (Block != nullptr)
            {
                const auto& R = (*Block)["explosion_radius_override"];
                Radius = Alt && R.size() > 1 ? R[1].get<int>() : R[0].get<int>();
            }
            else
                Radius = Alt ? IntOr(T, "explosion_radius_alt", 0) : IntOr(T, "explosion_radius", 0);
            std::string Inner, Outer;
            const Map* Rings = Value("explosion_rings", "explosion_rings_override");
            const Map* Damage = Value("explosion_damage", "explosion_damage_override");
            if (Radius > 0 && Alt && Rings != nullptr) Inner = std::to_string((*Rings)[3].get<int>()), Outer = std::to_string((*Rings)[5].get<int>());
            else if (Radius > 0 && !Alt && Damage != nullptr) Inner = std::to_string((*Damage)[0].get<int>()), Outer = std::to_string((*Damage)[1].get<int>());
            Out.push_back(Inner);
            Out.push_back(Outer);
        }
        // the gravity: the first attachment's ballistic block replaces the tunables'
        if (L.BallisticBlock.At >= 0 && L.BallisticGravity >= 0 && L.GravityInches > 0.0f)
        {
            const Map* G = T.contains("gravity") ? &T["gravity"] : nullptr;
            for (const Map* A : Atts)
                if (A->contains("gravity_override"))
                {
                    G = &(*A)["gravity_override"];
                    break;
                }
            Out.push_back(G != nullptr ? Num(L.GravityInches * G->get<double>() * 0.0254, 2) : std::string());
        }
        // damage by range (0x46386d0, 0x464a150): the first attachment's override record replaces the
        // weapon's, then each attachment multiplies the 5 damages and 4 ranges by its scale table's
        // record, or (without one) every damage by its damage scale and every range by its range
        // scale (when not 0); a product within 1e-6 of 1 is 1; damages truncated
        if (L.AttDamageTable >= 0)
        {
            std::vector<std::string> Cells(9);
            if (T.contains("damage") && T.contains("ranges"))
            {
                int D[5];
                float R[4], Sd[5] = { 1, 1, 1, 1, 1 }, Sr[4] = { 1, 1, 1, 1 };
                for (int K = 0; K < 5; K++) D[K] = T["damage"][K].get<int>();
                for (int K = 0; K < 4; K++) R[K] = (float)T["ranges"][K].get<double>();
                for (const Map* A : Atts)
                    if (A->contains("damage_override"))
                    {
                        for (int K = 0; K < 5; K++) D[K] = (*A)["damage_override"][K].get<int>();
                        for (int K = 0; K < 4; K++) R[K] = (float)(*A)["ranges_override"][K].get<double>();
                        break;
                    }
                for (const Map* A : Atts)
                {
                    if (A->contains("damage_range_scales"))
                    {
                        const auto& S = (*A)["damage_range_scales"];
                        for (int K = 0; K < 5; K++) Sd[K] *= (float)S[K].get<double>();
                        for (int K = 0; K < 4; K++) Sr[K] *= (float)S[5 + K].get<double>();
                        continue;
                    }
                    const float Ds = (float)FloatOr(*A, "damage_scale", 1.0), Rs = (float)FloatOr(*A, "range_scale", 0.0);
                    for (int K = 0; K < 5; K++) Sd[K] *= Ds;
                    if (Rs != 0.0f)
                        for (int K = 0; K < 4; K++) Sr[K] *= Rs;
                }
                const auto Snap6 = [](float V) { return std::fabs(1.0f - V) <= 1e-6f ? 1.0f : V; };
                const auto Dmg = [&](int K) { return std::to_string((int)(Snap6(Sd[K]) * (float)D[K])); };
                const auto Metres = [&](int K) { return Num((double)(Snap6(Sr[K]) * R[K] / InchesPerMetre), 2); };
                Cells = { Dmg(4), Metres(0), Dmg(1), Metres(1), Dmg(2), Metres(2), Dmg(3), Metres(3), Dmg(0) };
            }
            Out.insert(Out.end(), Cells.begin(), Cells.end());
        }
        // the hit location multipliers: the first attachment's table replaces the weapon's (0x4642180)
        if (L.AttHit >= 0)
        {
            const Map* Hit = T.contains("hit") ? &T["hit"] : nullptr;
            for (const Map* A : Atts)
                if (A->contains("hit_override"))
                {
                    Hit = &(*A)["hit_override"];
                    break;
                }
            for (size_t H = 1; H < std::size(HitLocations); H++)
                Out.push_back(Hit != nullptr && (*Hit)[H].is_number() ? Num((*Hit)[H].get<double>(), 4) : std::string());
        }
        // penetration (0x4648ad0, 0x46489e0): the first attachment's level and depth scale replace the
        // weapon's; the attachments' additions summed, and a non-zero level with a non-zero addition
        // clamped to [1, max]
        if (L.DefPenetration >= 0)
        {
            int Level = IntOr(W, "penetration_level", -1), Add = 0;
            double Scale = FloatOr(W, "penetration_depth_scale", -1.0);
            for (const Map* A : Atts)
                if (A->contains("penetration_override"))
                {
                    Level = (*A)["penetration_override"][0].get<int>();
                    Scale = (*A)["penetration_override"][1].get<double>();
                    break;
                }
            for (const Map* A : Atts) Add += IntOr(*A, "penetration_add", 0);
            if (Level > 0 && Add != 0) Level = std::clamp(Level + Add, 1, L.PenetrationMax);
            Out.push_back(Level < 0 ? std::string() : std::to_string(Level));
            Out.push_back(Scale < 0.0 ? std::string() : Num(Scale, 4));
        }
        // the perks the attachments give
        if (L.AttPerks >= 0)
        {
            std::string Perks;
            for (const Map* A : Atts)
                if (A->contains("perks"))
                    for (const auto& P : (*A)["perks"]) Perks += (Perks.empty() ? "" : " ") + P.get<std::string>();
            Out.push_back(CsvField(Perks));
        }
        return Out;
    }

    // the hit location columns of the attachment tables (",hit_helmet,..."), when the layout gives the override
    std::string HitColumns(const IwWeaponStats::Layout& L)
    {
        std::string S;
        if (L.AttHit >= 0)
            for (size_t K = 1; K < std::size(HitLocations); K++) S += std::string(",hit_") + HitLocations[K];
        return S;
    }

    // compatibility (0x46143c0 (A, B, slot of B), true when A's lists name B): B's group in A's group
    // list, B's own id in A's id list, B's slot in A's slot list; each match as {via, A's field}
    std::vector<std::pair<std::string, std::string>> Excludes(const IwWeaponStats::Layout& L, const Map& A, const Map& B, int SlotB)
    {
        std::vector<std::pair<std::string, std::string>> R;
        const auto In = [&](const char* Key, const std::string& V) {
            if (!A.contains(Key) || V.empty() || V == "00000000") return false;
            for (const auto& E : A[Key])
                if (E.get<std::string>() == V) return true;
            return false;
        };
        if (B.contains("group") && In("excludes_groups", B["group"].get<std::string>()))
            R.push_back({ "group " + B.value("group_name", B["group"].get<std::string>()), std::format("0x{:x}", L.AttExGroups) });
        if (B.contains("self_id") && In("excludes_ids", B["self_id"].get<std::string>())) R.push_back({ "attachment", std::format("0x{:x}", L.AttExIds) });
        if (L.SlotId && SlotB >= 0 && SlotB < (int)L.SlotNames.size() && In("excludes_slots", std::format("{:08x}", L.SlotId(L.SlotNames[(size_t)SlotB]))))
            R.push_back({ "slot " + L.SlotNames[(size_t)SlotB], std::format("0x{:x}", L.AttExSlots) });
        return R;
    }

    // a weapon's attachments: slot, index in the slot's list, facts
    struct Present
    {
        int Slot = 0, Index = 0;
        std::string Id;
        const Map* A = nullptr;
    };

    // the game's drop (0x4614550): over the slots in order, an attachment whose lists name another one
    // still present is removed; returns the removed ones' positions in P
    std::vector<size_t> Dropped(const IwWeaponStats::Layout& L, const std::vector<Present>& P)
    {
        std::vector<bool> Gone(P.size(), false);
        std::vector<size_t> Order(P.size());
        for (size_t I = 0; I < P.size(); I++) Order[I] = I;
        std::stable_sort(Order.begin(), Order.end(), [&](size_t X, size_t Y) { return P[X].Slot < P[Y].Slot; });
        std::vector<size_t> Out;
        for (const size_t I : Order)
            for (const size_t J : Order)
            {
                if (Gone[I] || Gone[J] || P[I].Slot == P[J].Slot || P[I].Id == P[J].Id) continue;
                if (!Excludes(L, *P[I].A, *P[J].A, P[J].Slot).empty())
                {
                    Gone[I] = true;
                    Out.push_back(I);
                }
            }
        return Out;
    }

    // a Zombies weapon's Pack-a-Punch attachments (a weaponmetadataentry bundle, read by the scripts): the
    // slot 0 attachment of each level, and the variants used while the weapon holds an attachment
    struct PapVariant
    {
        std::string Holding, Pap, Pap4;
    };
    struct PapInfo
    {
        std::string Base, Pap, Pap4;
        std::vector<PapVariant> Variants;
    };

    // the weaponmetadataentry bundles the bundles kind wrote beside this kind's folder (the zones' folders
    // under <out>/<game>/bundles), by weapon; keys by their FNV-1a 64 hash (some have no known name)
    // what the custom settings files take from the bundles: the custom settings bundles asked for (by id:
    // the bundle and its zone folder), the weaponmetadataentry bundles' Pack-a-Punch scales by weapon, the
    // weapon lists' scales and the entries they hold
    struct PapList
    {
        std::string Name;
        Map Scales = Map::array();      // levels 1-4 (null: the key absent)
        std::set<std::string> Entries;  // entry bundle ids
    };
    struct BundleSet
    {
        std::map<std::string, std::pair<Map, std::string>> Settings;
        std::map<std::string, std::pair<std::string, Map>> Entries;   // weapon -> {entry id, its own scales}
        std::vector<PapList> Lists;
    };

    std::map<std::string, PapInfo> PapTable(const IwWeaponStats::Layout& L, const std::string& Dir, const std::set<std::string>& Wanted, BundleSet& Set)
    {
        std::map<std::string, PapInfo> Out;
        if (L.PapBundleType.empty() && Wanted.empty()) return Out;
        std::error_code Ec;
        const fs::path Root = fs::path(Dir).parent_path() / "bundles";
        if (!fs::is_directory(Root, Ec)) return Out;
        // (a field absent or not a string: "")
        const auto Text = [](const Map& O, const char* K) { return O.is_object() && O.contains(K) && O[K].is_string() ? O[K].get<std::string>() : std::string(); };
        const auto Str = [&](const Map& Fields, uint64_t Key) -> std::string {
            const std::string K = Hex(Key);
            for (const auto& F : Fields)
                if (Text(F, "key_hash") == K) return Text(F, "value");
            return "";
        };
        for (const auto& E : fs::recursive_directory_iterator(Root, Ec))
        {
            if (E.path().extension() != ".json") continue;
            std::ifstream In(E.path(), std::ios::binary);
            Map B = Map::parse(In, nullptr, false);
            if (B.is_discarded() || !B["fields"].is_array()) continue;
            const std::string Type = Text(B, "type_name");
            if (const std::string Id = Text(B, "id"); Wanted.count(Id))
            {
                Set.Settings[Id] = { B, E.path().parent_path().filename().string() };
                continue;
            }
            const Map& F = B["fields"];
            // a scale: the float (or int) of a key, null when absent
            const auto Scale = [&](const Map& Fields, uint64_t Key) -> Map {
                const std::string K = Hex(Key);
                for (const auto& X : Fields)
                    if (Text(X, "key_hash") == K && X["value"].is_number()) return X["value"];
                return nullptr;
            };
            if (!L.PapListType.empty() && Type == L.PapListType)
            {
                PapList P;
                P.Name = Text(B, "name");
                for (int K = 0; K < 4; K++) P.Scales.push_back(Scale(F, L.PapListKeys[2 + K]));
                for (const auto& X : F)
                    if (Text(X, "key_hash") == Hex(L.PapListKeys[0]) && X["value"].is_array())
                        for (const auto& Item : X["value"])
                            if (Item.is_array())
                                for (const auto& Y : Item)
                                    if (Text(Y, "key_hash") == Hex(L.PapListKeys[1]) && !Text(Y, "value").empty()) P.Entries.insert(Text(Y, "value"));
                Set.Lists.push_back(std::move(P));
                continue;
            }
            if (Type != L.PapBundleType || L.PapBundleType.empty()) continue;
            const std::string Weapon = Str(F, L.PapKeys[0]);
            if (Weapon.empty()) continue;
            {
                Map Own = Map::array();
                bool Any = false;
                for (int K = 0; K < 4; K++)
                {
                    Own.push_back(L.PapEntryScales[K] != 0 ? Scale(F, L.PapEntryScales[K]) : Map(nullptr));
                    Any = Any || !Own.back().is_null();
                }
                Set.Entries[Weapon] = { Text(B, "id"), Any ? Own : Map(nullptr) };
            }
            PapInfo P{ Str(F, L.PapKeys[1]), Str(F, L.PapKeys[2]), Str(F, L.PapKeys[3]), {} };
            for (const auto& X : F)
                if (Text(X, "key_hash") == Hex(L.PapKeys[4]) && X["value"].is_array())
                    for (const auto& Item : X["value"])
                        if (Item.is_array()) P.Variants.push_back({ Str(Item, L.PapKeys[5]), Str(Item, L.PapKeys[6]), Str(Item, L.PapKeys[7]) });
            if (!P.Pap.empty()) Out[Weapon] = std::move(P);
        }
        return Out;
    }

    // the rows of a CSV text (quoted fields as CsvField writes them)
    std::vector<std::vector<std::string>> CsvRows(const std::string& Text)
    {
        std::vector<std::vector<std::string>> Rows(1);
        std::string Cell;
        bool Quoted = false;
        for (size_t I = 0; I < Text.size(); I++)
        {
            const char C = Text[I];
            if (Quoted)
            {
                if (C == '"' && I + 1 < Text.size() && Text[I + 1] == '"') Cell += '"', I++;
                else if (C == '"') Quoted = false;
                else Cell += C;
            }
            else if (C == '"') Quoted = true;
            else if (C == ',') Rows.back().push_back(std::move(Cell)), Cell.clear();
            else if (C == '\n') Rows.back().push_back(std::move(Cell)), Cell.clear(), Rows.emplace_back();
            else if (C != '\r') Cell += C;
        }
        if (!Cell.empty() || !Rows.back().empty()) Rows.back().push_back(std::move(Cell));
        if (Rows.back().empty()) Rows.pop_back();
        return Rows;
    }

    // a bundle's fields as the custom settings file writes them: the key's name ("name", null when only its
    // hash is known), its 64-bit key hash, its script hash, the type and the value; a struct's fields under
    // "fields", an array's elements under "elements" (a struct element: its fields)
    Json SettingsFields(const Map& Fields)
    {
        Json Out = Json::array();
        for (const auto& F : Fields)
        {
            if (!F.is_object()) continue;
            Json O;
            const std::string Key = F.value("key", std::string()), KeyHash = F.value("key_hash", std::string());
            O["name"] = Key != KeyHash ? Json(Key) : Json(nullptr);
            O["key_hash"] = KeyHash;
            O["script_hash"] = F.value("script_hash", std::string());
            const std::string Type = F.value("type", std::string());
            O["type"] = Type;
            const Map& V = F.contains("value") ? F["value"] : Map(nullptr);
            if ((Type == "struct" || Type == "keyed_struct") && V.is_array()) O["fields"] = SettingsFields(V);
            else if (Type == "array" && V.is_array())
            {
                if (F.contains("element_type")) O["element_type"] = F["element_type"];
                Json Elements = Json::array();
                for (const auto& E : V) Elements.push_back(E.is_array() ? SettingsFields(E) : Json::parse(E.dump()));
                O["elements"] = std::move(Elements);
            }
            else O["value"] = Json::parse(V.dump());
            if (F.contains("value_hash")) O["value_hash"] = F["value_hash"];
            Out.push_back(std::move(O));
        }
        return Out;
    }

    // every string value of the fields (nested ones too) with the path of its key: "name" or the key hash
    void SettingsStrings(const Json& Fields, const std::string& Path, std::vector<std::pair<std::string, std::string>>& Out)
    {
        for (const auto& F : Fields)
        {
            const std::string Here = Path + (Path.empty() ? "" : ".") + (F["name"].is_string() ? F["name"].get<std::string>() : F["key_hash"].get<std::string>());
            if (F.contains("fields")) SettingsStrings(F["fields"], Here, Out);
            if (F.contains("elements"))
                for (size_t K = 0; K < F["elements"].size(); K++)
                    if (F["elements"][K].is_array()) SettingsStrings(F["elements"][K], Here + std::format("[{}]", K), Out);
            if (F.contains("value") && F["value"].is_string()) Out.emplace_back(Here, F["value"].get<std::string>());
        }
    }

    // weapon_custom_settings/<weapon>.json for every weapon whose definition names a custom settings bundle;
    // the weapons its settings name with their damages, and its Pack-a-Punch levels where they differ
    size_t CustomSettingsFiles(const IwWeaponStats::Layout& L, const Map& Weapons, const Map& Strings, const std::string& Dir, const BundleSet& Set,
                               const std::map<std::string, PapInfo>& Paps, const std::map<std::string, std::map<std::string, std::string>>& Rows,
                               const std::string& AttCsv)
    {
        // weapon_attachments.csv by {weapon, attachment}
        std::vector<std::string> AttHead;
        std::map<std::pair<std::string, std::string>, std::vector<std::string>> AttRows;
        {
            auto All = CsvRows(AttCsv);
            if (!All.empty())
            {
                AttHead = All[0];
                const auto Col = [&](const char* N) { return (size_t)(std::find(AttHead.begin(), AttHead.end(), N) - AttHead.begin()); };
                const size_t WCol = Col("weapon"), ACol = Col("attachment");
                for (size_t I = 1; I < All.size(); I++)
                    if (WCol < All[I].size() && ACol < All[I].size()) AttRows.try_emplace({ All[I][WCol], All[I][ACol] }, All[I]);
            }
        }
        // the columns a level compares (the stats, not the row's description)
        static const std::set<std::string> NotStats = { "weapon", "slot", "index", "attachment", "display_name", "slot_name", "alt_mode", "excludes", "brings", "perks" };
        // the stats a linked weapon brings (its first tunables set's row)
        static const char* const Linked[] = { "class", "fire_time_ms", "clip_size", "max_ammo", "pellets", "muzzle_velocity_mps", "explosion_radius_m",
                                              "explosion_inner_damage", "explosion_outer_damage", "explosion_inner_radius_m", "explosion_mid_radius_m",
                                              "explosion_mid_damage", "damage_max", "range_max_m", "damage_mid1", "range_mid1_m", "damage_mid2", "range_mid2_m",
                                              "damage_mid3", "range_mid3_m", "damage_min" };
        const fs::path Out = fs::path(Dir) / "weapon_custom_settings";
        std::error_code Ec;
        fs::remove_all(Out, Ec);
        size_t Written = 0;
        std::set<std::string> Seen;
        for (const auto& W : Weapons)
        {
            if (!W.contains("custom_settings")) continue;
            const std::string Name = W["name"].get<std::string>(), Id = W["custom_settings"].get<std::string>();
            const auto It = Set.Settings.find(Id);
            if (It == Set.Settings.end() || !Seen.insert(Name).second) continue;
            const Map& B = It->second.first;
            Json J;
            J["weapon"] = Name;
            J["weapon_id"] = W["id"];
            if (W.contains("display") && Strings.contains(W["display"].get<std::string>())) J["display_name"] = Strings[W["display"].get<std::string>()];
            Json S;
            S["bundle_id"] = Id;
            S["bundle_type"] = B.contains("type") ? Json::parse(B["type"].dump()) : Json(nullptr);
            S["zone"] = It->second.second;
            S["fields"] = SettingsFields(B["fields"]);
            // (counts: the fields, nested ones included, and those named)
            size_t All = 0, Named = 0;
            const std::function<void(const Json&)> Count = [&](const Json& Fs) {
                for (const auto& F : Fs)
                {
                    All++, Named += F["name"].is_string() ? 1 : 0;
                    if (F.contains("fields")) Count(F["fields"]);
                    if (F.contains("elements"))
                        for (const auto& E : F["elements"])
                            if (E.is_array()) Count(E);
                }
            };
            Count(S["fields"]);
            S["field_count"] = All;
            S["named_fields"] = Named;
            // the weapons the settings name (a sub-weapon: a grenade, a fireball...) with their damages
            std::vector<std::pair<std::string, std::string>> Values;
            SettingsStrings(S["fields"], "", Values);
            Json Links = Json::array();
            for (const auto& [Path, Value] : Values)
            {
                Json Lk;
                Lk["field"] = Path;
                Lk["name"] = Value;
                if (const auto R = Rows.find(Value); R != Rows.end())
                {
                    Lk["kind"] = "weapon";
                    Json Stats;
                    for (const char* C : Linked)
                        if (const auto V = R->second.find(C); V != R->second.end() && !V->second.empty()) Stats[C] = V->second;
                    Lk["stats"] = std::move(Stats);
                }
                else if (AttRows.count({ Name, Value })) Lk["kind"] = "attachment";
                else continue;
                Links.push_back(std::move(Lk));
            }
            S["linked"] = std::move(Links);
            J["custom_settings"] = std::move(S);
            // Pack-a-Punch levels 0-4: the slot 0 attachment of each level (base; pap for 1-3; pap4, else pap,
            // for 4), the stats of weapon_attachments.csv that differ from level 0's, the damage scale
            if (const auto P = Paps.find(Name); P != Paps.end())
            {
                Json Levels = Json::array();
                const auto Row0 = AttRows.find({ Name, P->second.Base.empty() ? std::string("(none)") : P->second.Base });
                const auto Entry = Set.Entries.find(Name);
                for (int Lv = 0; Lv <= 4; Lv++)
                {
                    const std::string Att = Lv == 0 ? P->second.Base : Lv < 4 || P->second.Pap4.empty() ? P->second.Pap : P->second.Pap4;
                    Json E;
                    E["level"] = Lv;
                    E["attachment"] = Att;
                    if (Lv > 0 && Entry != Set.Entries.end())
                    {
                        if (!Entry->second.second.is_null() && !Entry->second.second[Lv - 1].is_null())
                            E["damage_scale"] = Json{ { "value", Json::parse(Entry->second.second[Lv - 1].dump()) },
                                                      { "source", "weaponmetadataentry " + Entry->second.first + " key " + Hex(L.PapEntryScales[Lv - 1]) } };
                        else
                        {
                            // the list's scale (the game mode's list: each list holding the weapon's entry)
                            Json ByList = Json::array();
                            for (const auto& Ls : Set.Lists)
                                if (Ls.Entries.count(Entry->second.first) && !Ls.Scales[Lv - 1].is_null())
                                    ByList.push_back(Json{ { "list", Ls.Name }, { "value", Json::parse(Ls.Scales[Lv - 1].dump()) },
                                                           { "source", "weaponmetadata " + Ls.Name + " key " + Hex(L.PapListKeys[2 + Lv - 1]) } });
                            if (!ByList.empty()) E["damage_scale_by_list"] = std::move(ByList);
                        }
                    }
                    const auto Rw = AttRows.find({ Name, Att });
                    if (Lv > 0 && Rw != AttRows.end() && Row0 != AttRows.end())
                    {
                        Json Diff;
                        for (size_t C = 0; C < AttHead.size() && C < Rw->second.size() && C < Row0->second.size(); C++)
                            if (!NotStats.count(AttHead[C]) && Rw->second[C] != Row0->second[C])
                                Diff[AttHead[C]] = Json{ { "level0", Row0->second[C] }, { "value", Rw->second[C] } };
                        E["stats_changed"] = std::move(Diff);
                        E["source"] = "weapon_attachments.csv, weapon " + Name + " with " + Att;
                    }
                    Levels.push_back(std::move(E));
                }
                J["pack_a_punch"] = std::move(Levels);
            }
            const std::string Text = J.dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
            fs::create_directories(Out, Ec);
            if (ExportWriters::WriteBytes(Out / (ExportNaming::SafeFileName(Name) + ".json"), (const uint8_t*)Text.data(), Text.size())) Written++;
        }
        return Written;
    }

    size_t AttachmentTables(const IwWeaponStats::Layout& L, const Map& Weapons, const Map& Tunables, const Map& Pkgs, const Map& Attachments, const Map& Names,
                            const Map& Strings, const std::string& Dir, std::string& AttCsv, std::string& LoadCsv, std::string& RulesCsv,
                            const std::map<std::string, PapInfo>& Paps)
    {
        const bool Rules = L.AttGroup >= 0 && !L.SlotNames.empty();
        // (the columns LoadoutStats appends for the fields the layout gives, in its order)
        const std::string Stats = std::string("clip_size,") + (L.AttMaxAmmoScale >= 0 ? "max_ammo," : "") +
                                  (L.Pellets.At >= 0 && L.AttAmmoPellets >= 0 ? "pellets," : "") +
                                  "reload_ms,reload_empty_ms,reload_add_ms,reload_empty_add_ms,sprint_to_fire_ms,"
                                  "super_sprint_to_fire_ms,ads_time_ms,ads_out_time_ms,muzzle_velocity_mps" +
                                  (L.AttFireTime >= 0 ? ",fire_time_ms,rpm" : "") + (L.BurstDelay.At >= 0 ? ",burst_delay_ms" : "") +
                                  (L.ExplosionDamage.At >= 0 ? ",explosion_inner_damage,explosion_outer_damage" : "") +
                                  (L.BallisticBlock.At >= 0 && L.BallisticGravity >= 0 && L.GravityInches > 0.0f ? ",gravity_mps2" : "") +
                                  (L.AttDamageTable >= 0 ? ",damage_max,range_max_m,damage_mid1,range_mid1_m,damage_mid2,range_mid2_m,"
                                                           "damage_mid3,range_mid3_m,damage_min"
                                                         : "") +
                                  HitColumns(L) + (L.DefPenetration >= 0 ? ",penetration_level,penetration_depth_scale" : "") +
                                  (L.AttPerks >= 0 ? ",perks" : "");
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
        const std::string Extra = Rules ? ",slot_name,alt_mode,excludes,brings" : "";
        AttCsv = std::string("weapon,slot,index,attachment,display_name,") + Stats + Extra + "\n";
        if (Rules)
            RulesCsv = "weapon,slot,slot_name,attachment,display_name,rule,other_slot,other_slot_name,other_attachment,other_display_name,"
                       "other_is_default,via,source\n";
        const auto SlotName = [&](int S) { return S >= 0 && S < (int)L.SlotNames.size() ? L.SlotNames[(size_t)S] : std::string(); };
        const auto AttName = [&](const std::string& Id) {
            return Attachments.contains(Id) ? Attachments[Id]["name"].get<std::string>() : "attachment_" + Id;
        };
        size_t Rows = 0;
        for (const auto& [Name, Wp] : ByName)
        {
            std::string Unknown;
            const auto Base = LoadoutStats(L, Wp, Pkgs, Attachments, {}, Unknown);
            AttCsv += CsvField(Name) + ",,,(none),";
            for (const auto& V : Base) AttCsv += "," + V;
            if (Rules) AttCsv += ",,,,";
            AttCsv += "\n";
            const auto& Slots = (*Wp.W)["slots"];
            std::vector<Present> All;
            for (size_t S = 0; S < Slots.size(); S++)
                for (size_t K = 0; K < Slots[S].size(); K++)
                    if (const std::string Id = Slots[S][K].get<std::string>(); Attachments.contains(Id)) All.push_back({ (int)S, (int)K, Id, &Attachments[Id] });
            // the weapon's default part of each slot
            std::map<int, std::string> Defaults;
            if (Wp.W->contains("defaults"))
                for (const auto& D : (*Wp.W)["defaults"]) Defaults[D["slot"].get<int>()] = D["attachment"].get<std::string>();
            // the slot an attachment id holds on this weapon (-1 none)
            const auto SlotOf = [&](const std::string& Id) {
                for (const auto& X : All)
                    if (X.Id == Id) return X.Slot;
                return -1;
            };
            for (const auto& X : All)
            {
                const std::string Asset = (*X.A)["name"].get<std::string>();
                const bool Alt = IsAltMode(L, X.Slot, *X.A);
                std::string U;
                const auto St = LoadoutStats(L, Wp, Pkgs, Attachments, { { X.Slot, X.Index } }, U, Alt);
                AttCsv += CsvField(Name) + "," + std::to_string(X.Slot) + "," + std::to_string(X.Index) + "," + CsvField(Asset) + "," + CsvField(Display(Asset));
                for (const auto& V : St) AttCsv += "," + V;
                if (Rules)
                {
                    // the other attachments of the weapon either one's lists name, the ones it brings
                    std::string Ex, Br;
                    for (const auto& Y : All)
                        if (Y.Slot != X.Slot && (!Excludes(L, *X.A, *Y.A, Y.Slot).empty() || !Excludes(L, *Y.A, *X.A, X.Slot).empty()))
                            Ex += (Ex.empty() ? "" : " ") + (*Y.A)["name"].get<std::string>();
                    if (X.A->contains("brings"))
                        for (const auto& B : (*X.A)["brings"]) Br += (Br.empty() ? "" : " ") + AttName(B.get<std::string>());
                    const std::string Mode = !Alt ? "" : IntOr(*X.A, "alt_active", 0) != 0 ? "active" : "never_active";
                    AttCsv += "," + CsvField(SlotName(X.Slot)) + "," + Mode + "," + CsvField(Ex) + "," + CsvField(Br);
                    // the rules: what its lists exclude (one row per attachment named, one per slot), what it brings
                    const std::string Head = CsvField(Name) + "," + std::to_string(X.Slot) + "," + CsvField(SlotName(X.Slot)) + "," + CsvField(Asset) + "," +
                                             CsvField(Display(Asset)) + ",";
                    std::set<int> SlotsBlocked;
                    for (const auto& Y : All)
                    {
                        if (Y.Slot == X.Slot) continue;
                        for (const auto& [Via, Source] : Excludes(L, *X.A, *Y.A, Y.Slot))
                        {
                            if (Via.rfind("slot ", 0) == 0)
                            {
                                if (SlotsBlocked.insert(Y.Slot).second)
                                    RulesCsv += Head + "blocks_slot," + std::to_string(Y.Slot) + "," + CsvField(SlotName(Y.Slot)) + ",,,," + CsvField(Via) + "," + Source + "\n";
                                continue;
                            }
                            const std::string Other = (*Y.A)["name"].get<std::string>();
                            const auto D = Defaults.find(Y.Slot);
                            RulesCsv += Head + "excludes," + std::to_string(Y.Slot) + "," + CsvField(SlotName(Y.Slot)) + "," + CsvField(Other) + "," +
                                        CsvField(Display(Other)) + "," + (D != Defaults.end() && D->second == Y.Id ? "1" : "") + "," + CsvField(Via) + "," + Source + "\n";
                        }
                    }
                    if (X.A->contains("brings"))
                        for (const auto& B : (*X.A)["brings"])
                        {
                            const std::string Id = B.get<std::string>(), Other = AttName(Id);
                            const int S = SlotOf(Id);
                            const auto D = Defaults.find(S);
                            RulesCsv += Head + "brings," + (S >= 0 ? std::to_string(S) : "") + "," + CsvField(SlotName(S)) + "," + CsvField(Other) + "," +
                                        CsvField(Display(Other)) + "," + (D != Defaults.end() && D->second == Id ? "1" : "") + ",installed," +
                                        std::format("0x{:x}", L.AttBrings) + "\n";
                        }
                }
                AttCsv += "\n";
                Rows++;
            }
        }
        // loadouts.txt: "<weapon>: <attachment>, <attachment>"
        std::ifstream In(fs::path(Dir) / "loadouts.txt");
        if (In)
        {
            LoadCsv = std::string("weapon,attachments,not_found,") + (Rules ? "brought,dropped,alt_mode,pap_variant," : "") + Stats + "\n";
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
                std::string Brought, Gone, AltName, PapVariantName;
                if (Rules)
                {
                    // the attachments the picks bring take their slots (0x4612510, 0x4614550), then the game
                    // drops each attachment whose lists name another one present (0x46143c0)
                    std::vector<Present> P;
                    for (const auto& [S, K] : Picks)
                    {
                        const std::string Id = Slots[(size_t)S][(size_t)K].get<std::string>();
                        P.push_back({ S, K, Id, &Attachments[Id] });
                    }
                    std::vector<Present> Add;
                    for (const auto& X : P)
                        if (X.A->contains("brings"))
                            for (const auto& B : (*X.A)["brings"])
                                for (size_t S = 0; S < Slots.size(); S++)
                                    for (size_t K = 0; K < Slots[S].size(); K++)
                                        if (Slots[S][K].get<std::string>() == B.get<std::string>() && Attachments.contains(B.get<std::string>()))
                                            Add.push_back({ (int)S, (int)K, B.get<std::string>(), &Attachments[B.get<std::string>()] });
                    for (const auto& X : Add)
                    {
                        std::erase_if(P, [&](const Present& Y) { return Y.Slot == X.Slot; });
                        P.push_back(X);
                        Brought += (Brought.empty() ? "" : " ") + (*X.A)["name"].get<std::string>();
                    }
                    std::vector<bool> Keep(P.size(), true);
                    for (const size_t I : Dropped(L, P))
                    {
                        Keep[I] = false;
                        Gone += (Gone.empty() ? "" : " ") + (*P[I].A)["name"].get<std::string>();
                    }
                    // Pack-a-Punch: while the weapon holds a variant's attachment, the scripts' bundle names
                    // another slot 0 attachment for the level
                    if (const auto Pi = Paps.find(Name); Pi != Paps.end())
                        for (size_t I = 0; I < P.size(); I++)
                        {
                            if (!Keep[I]) continue;
                            const std::string Own = (*P[I].A)["name"].get<std::string>();
                            const bool Level4 = Own == Pi->second.Pap4;
                            if (Own != Pi->second.Pap && !Level4) continue;
                            for (const auto& V : Pi->second.Variants)
                            {
                                bool Holds = false;
                                for (size_t J = 0; J < P.size(); J++)
                                    if (Keep[J] && (*P[J].A)["name"].get<std::string>() == V.Holding) Holds = true;
                                const std::string Want = Level4 ? V.Pap4 : V.Pap;
                                if (!Holds || Want.empty()) continue;
                                for (size_t K = 0; K < Slots[(size_t)P[I].Slot].size(); K++)
                                    if (const std::string Id = Slots[(size_t)P[I].Slot][K].get<std::string>();
                                        Attachments.contains(Id) && Attachments[Id]["name"].get<std::string>() == Want)
                                    {
                                        P[I] = { P[I].Slot, (int)K, Id, &Attachments[Id] };
                                        PapVariantName = Want;
                                    }
                                break;
                            }
                        }
                    Picks.clear();
                    for (size_t I = 0; I < P.size(); I++)
                        if (Keep[I])
                        {
                            Picks.push_back({ P[I].Slot, P[I].Index });
                            if (IsAltMode(L, P[I].Slot, *P[I].A)) AltName = (*P[I].A)["name"].get<std::string>();
                        }
                }
                std::string U;
                const auto St = LoadoutStats(L, It->second, Pkgs, Attachments, Picks, U);
                LoadCsv += CsvField(Name) + "," + CsvField(Trim(Rest)) + "," + CsvField(Missing);
                if (Rules) LoadCsv += "," + CsvField(Brought) + "," + CsvField(Gone) + "," + CsvField(AltName.empty() ? "" : AltName + " (inactive)") + "," + CsvField(PapVariantName);
                for (const auto& V : St) LoadCsv += "," + V;
                LoadCsv += "\n";
            }
        }
        return Rows;
    }

    // weapon_anims.csv: the anim table of each weapon's base animation package, one row per anim id
    size_t AnimTable(const Map& Weapons, const Map& Pkgs, std::string& Csv)
    {
        std::map<std::string, std::string> ByName;   // weapon -> its package
        std::set<std::string> Seen;
        for (const auto& W : Weapons)
            if (W.contains("animpkg") && Seen.insert(W["id"].get<std::string>()).second) ByName.emplace(W["name"].get<std::string>(), W["animpkg"].get<std::string>());
        Csv = "weapon,animpkg,anim_id,xanim\n";
        size_t Rows = 0;
        for (const auto& [Name, Pkg] : ByName)
        {
            if (!Pkgs.contains(Pkg) || !Pkgs[Pkg].contains("anims")) continue;
            for (const auto& A : Pkgs[Pkg]["anims"])
            {
                Csv += CsvField(Name) + "," + Pkg + "," + std::format("0x{:x}", A[0].get<int>()) + "," + CsvField(A[1].get<std::string>()) + "\n";
                Rows++;
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
        F32("burst_delay", L.BurstDelay);
        F32("move_speed_scale", L.MoveSpeedScale);
        F32("ads_move_speed_scale", L.AdsMoveSpeedScale);
        I32("reserve_clips", L.ReserveClips);
        I32("pellets", L.Pellets);
        if (const int64_t P = Base(L.ExplosionDamage); P >= 0) T["explosion_damage"] = Json::array({ At<int32_t>(Img, P), At<int32_t>(Img, P + 4) });
        if (const int64_t P = Base(L.ExplosionRings); P >= 0)
        {
            Json Rings = Json::array();
            for (int K = 0; K < 6; K++) Rings.push_back(At<int32_t>(Img, P + 4 * K));
            T["explosion_rings"] = std::move(Rings);
        }
        if (const int64_t P = Base(L.BallisticBlock); P >= 0)
        {
            if (L.BallisticFlag >= 0) T["ballistic"] = At<uint8_t>(Img, P + L.BallisticFlag);
            if (L.BallisticGravity >= 0) T["gravity"] = (double)At<float>(Img, P + L.BallisticGravity);
        }
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
        if (Type == "weapon" && (L.WeaponDef >= 0 || L.DefIsWeapon))
        {
            const uint64_t Id = AssetId(P);
            const int64_t Def = L.DefIsWeapon ? P : Target(P + L.WeaponDef);
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
            // the bullet penetration: level (definition u8) and depth scale (weapon f32)
            if (L.DefPenetration >= 0) W["penetration_level"] = At<uint8_t>(Img, Def + L.DefPenetration);
            if (L.WeaponPenetrationScale >= 0) W["penetration_depth_scale"] = (double)At<float>(Img, P + L.WeaponPenetrationScale);
            // the script bundle the scripts read as weapon.weaponcustomsettings (its id; the bundles kind writes it)
            if (L.DefCustomSettings >= 0)
                if (const uint64_t B = RefId(Def + L.DefCustomSettings, 0)) W["custom_settings"] = Hex(B);
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
            else if (L.DefaultSlots >= 0)
            {
                Json Defaults = Json::array();
                for (int64_t S = 0; S < L.DefaultSlotCount; S++)
                {
                    const int64_t A = Target(P + L.DefaultSlots + S * 0x10 + 8);
                    const uint32_t N2 = std::min<uint32_t>(At<uint32_t>(Img, P + L.DefaultSlots + S * 0x10), 128);
                    for (uint32_t K = 0; A >= 0 && K < N2; K++)
                        if (const uint64_t Id = RefId(A + 8 * (int64_t)K, 0)) Defaults.push_back(Json{ { "slot", S }, { "attachment", Hex(Id) } });
                }
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
                // (an array of AttModelCount xmodel pointers: its first)
                const int64_t Array = L.AttModelCount >= 0 && At<uint32_t>(Img, P + L.AttModelCount) != 0 ? Target(P + Off) : -1;
                if (L.AttModelCount >= 0 && Array < 0) return;
                if (const uint64_t H = Array >= 0 ? RefId(Array, 0) : At<uint64_t>(Img, P + Off) & IdBits; H != 0)
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
                    if (L.AttExplosionDamage >= 0)
                        A["explosion_damage_override"] = Json::array({ At<int32_t>(Img, B + L.AttExplosionDamage), At<int32_t>(Img, B + L.AttExplosionDamage + 4) });
                    if (L.AttExplosionRings >= 0)
                    {
                        Json Rings = Json::array();
                        for (int K = 0; K < 6; K++) Rings.push_back(At<int32_t>(Img, B + L.AttExplosionRings + 4 * K));
                        A["explosion_rings_override"] = std::move(Rings);
                    }
                    if (L.AttExplosionFlag >= 0) A["explosion_alt_override"] = At<uint8_t>(Img, B + L.AttExplosionFlag);
                }
            // the burst delay: an attachment's i32 replaces the tunables' (not an alternate mode's), the
            // modifier block's f32 +4 scales it
            if (L.AttBurstDelay >= 0)
                if (const int64_t B = Target(P + L.AttBurstDelay); B >= 0 && !(L.AttAltMode >= 0 && L.AltSlot < 0 && At<uint8_t>(Img, P + L.AttAltMode) == 1))
                    A["burst_delay_override"] = At<int32_t>(Img, B);
            if (L.AttBurstDelayScale >= 0 && L.AttModifiers >= 0)
            {
                const int64_t T = Target(P + L.AttModifiers);
                const int64_t B = T >= 0 ? Target(T + L.AttBurstDelayScale) : -1;
                A["burst_delay_scale"] = B >= 0 ? (double)At<float>(Img, B + 4) : 1.0;
            }
            // the fire time: an attachment's i32 (0: none) replaces the tunables' (not an alternate
            // mode's), the modifier block's f32 +4 scales it
            if (L.AttFireTime >= 0)
                if (const int32_t F = At<int32_t>(Img, P + L.AttFireTime); F != 0 && !(L.AttAltMode >= 0 && L.AltSlot < 0 && At<uint8_t>(Img, P + L.AttAltMode) == 1))
                    A["fire_time_override"] = F;
            if (L.AttFireTimeScale >= 0 && L.AttModifiers >= 0)
            {
                const int64_t T = Target(P + L.AttModifiers);
                const int64_t B = T >= 0 ? Target(T + L.AttFireTimeScale) : -1;
                A["fire_time_scale"] = B >= 0 ? (double)At<float>(Img, B + 4) : 1.0;
            }
            // the ballistic block that replaces the tunables' (its gravity factor and flag)
            if (L.AttBallistic >= 0)
                if (const int64_t B = Target(P + L.AttBallistic); B >= 0)
                {
                    if (L.BallisticFlag >= 0) A["ballistic_override"] = At<uint8_t>(Img, B + L.BallisticFlag);
                    if (L.BallisticGravity >= 0) A["gravity_override"] = (double)At<float>(Img, B + L.BallisticGravity);
                }
            // damage by range: the override table's first record (5 damages, 4 ranges in inches), the
            // scale table's first record (5 damage scales, 4 range scales), else the uniform damage
            // scale (modifier block +4) and range scale (0: none)
            if (L.AttDamageTable >= 0)
                if (const int64_t B = Target(P + L.AttDamageTable); B >= 0)
                {
                    Json D = Json::array(), Rg = Json::array();
                    for (int K = 0; K < 5; K++) D.push_back(At<int32_t>(Img, B + 4 * K));
                    for (int K = 0; K < 4; K++) Rg.push_back((double)At<float>(Img, B + 0x14 + 4 * K));
                    A["damage_override"] = std::move(D);
                    A["ranges_override"] = std::move(Rg);
                }
            if (L.AttDamageScales >= 0)
                if (const int64_t B = Target(P + L.AttDamageScales); B >= 0)
                {
                    Json S = Json::array();
                    for (int K = 0; K < 9; K++) S.push_back((double)At<float>(Img, B + 4 * K));
                    A["damage_range_scales"] = std::move(S);
                }
            if (L.AttDamageScale >= 0 && L.AttModifiers >= 0)
            {
                const int64_t T = Target(P + L.AttModifiers);
                const int64_t B = T >= 0 ? Target(T + L.AttDamageScale) : -1;
                A["damage_scale"] = B >= 0 ? (double)At<float>(Img, B + 4) : 1.0;
            }
            if (L.AttRangeScale >= 0) A["range_scale"] = (double)At<float>(Img, P + L.AttRangeScale);
            // the hit location multipliers that replace the weapon's
            if (L.AttHit >= 0)
                if (const int64_t B = Target(P + L.AttHit); B >= 0)
                {
                    Json Hit = Json::array();
                    for (int K = 0; K < 22; K++) Hit.push_back((double)At<float>(Img, B + 4 * K));
                    A["hit_override"] = std::move(Hit);
                }
            // penetration: the level and depth scale that replace the weapon's, the level addition
            if (L.AttPenetration >= 0)
                if (const int64_t B = Target(P + L.AttPenetration); B >= 0)
                    A["penetration_override"] = Json::array({ At<uint8_t>(Img, B), (double)At<float>(Img, B + 4) });
            if (L.AttPenetrationAdd >= 0) A["penetration_add"] = At<int32_t>(Img, P + L.AttPenetrationAdd);
            // the perks it gives, by name (#<hash> when no zone string names it)
            if (L.AttPerks >= 0 && L.AttPerkCount >= 0)
                if (const int64_t B = Target(P + L.AttPerks); B >= 0)
                {
                    Json Perks = Json::array();
                    for (uint32_t K = 0; K < At<uint8_t>(Img, P + L.AttPerkCount); K++)
                    {
                        const uint32_t H = At<uint32_t>(Img, B + 4 * (int64_t)K);
                        const std::string S = V.ScriptString ? V.ScriptString(H) : std::string();
                        Perks.push_back(S.empty() ? std::format("#{:08x}", H) : S);
                    }
                    A["perks"] = std::move(Perks);
                }
            // compatibility: its group and own id, the groups, slots and own ids it excludes (u32s), the
            // attachments it brings (u64 ids)
            const auto Hex8 = [](uint32_t V) { return std::format("{:08x}", V); };
            const auto U32List = [&](int64_t Base, uint32_t Count) {
                Json List = Json::array();
                for (uint32_t K = 0; Base >= 0 && K < Count; K++) List.push_back(Hex8(At<uint32_t>(Img, Base + 4 * (int64_t)K)));
                return List;
            };
            if (L.AttGroup >= 0 && L.AttSelfId >= 0)
            {
                const uint32_t Group = At<uint32_t>(Img, P + L.AttGroup);
                if (Group != 0)
                {
                    A["group"] = Hex8(Group);
                    if (const std::string S = V.ScriptString ? V.ScriptString(Group) : std::string(); !S.empty()) A["group_name"] = S;
                }
                A["self_id"] = Hex8(At<uint32_t>(Img, P + L.AttSelfId));
                if (L.AttExGroups >= 0 && L.AttExGroupCount >= 0)
                    if (const uint8_t Nx = At<uint8_t>(Img, P + L.AttExGroupCount); Nx != 0) A["excludes_groups"] = U32List(Target(P + L.AttExGroups), Nx);
                if (L.AttExSlots >= 0 && L.AttExSlotCount >= 0)
                    if (const uint8_t Nx = At<uint8_t>(Img, P + L.AttExSlotCount); Nx != 0) A["excludes_slots"] = U32List(Target(P + L.AttExSlots), Nx);
                if (L.AttExIds >= 0 && L.AttExIdCount >= 0)
                    if (const uint8_t Nx = At<uint8_t>(Img, P + L.AttExIdCount); Nx != 0) A["excludes_ids"] = U32List(P + L.AttExIds, Nx);
                if (L.AttBrings >= 0 && L.AttBringCount >= 0)
                    if (const uint8_t Nx = At<uint8_t>(Img, P + L.AttBringCount); Nx != 0)
                    {
                        Json List = Json::array();
                        for (uint32_t K = 0; K < Nx; K++) List.push_back(Hex(At<uint64_t>(Img, P + L.AttBrings + 0x10 * (int64_t)K) & IdBits));
                        A["brings"] = std::move(List);
                    }
            }
            // the alternate mode's flags; an attachment whose u8 AttAltMode is 1 overrides the fire time
            // and the burst delay only as the active alternate mode (0x4637124, 0x463a9b0)
            if (L.AltSlot >= 0 && L.AttAltActive >= 0 && L.AttAltWithOthers >= 0 && L.AttAltNormal >= 0)
            {
                if (L.AttAltMode >= 0 && At<uint8_t>(Img, P + L.AttAltMode) == 1) A["alt_only_overrides"] = 1;
                A["alt_active"] = At<uint8_t>(Img, P + L.AttAltActive);
                uint8_t Keep = At<uint8_t>(Img, P + L.AttAltWithOthers);
                // (0x4653c37: alone only when the block is set and its byte is 0)
                if (L.AttAltBlock >= 0 && L.AttAltBlockKeep >= 0)
                    if (const int64_t B = Target(P + L.AttAltBlock); B < 0 || At<uint8_t>(Img, B + L.AttAltBlockKeep) != 0) Keep = 1;
                A["alt_with_others"] = Keep;
                A["alt_normal"] = At<uint8_t>(Img, P + L.AttAltNormal);
            }
            // max ammo: the scale, and the clip override block's max ammo, reserve magazines and flag
            if (L.AttMaxAmmoScale >= 0) A["max_ammo_scale"] = (double)At<float>(Img, P + L.AttMaxAmmoScale);
            if (L.AttClipOverride >= 0 && L.AttAmmoMax >= 0 && L.AttAmmoReserve >= 0 && L.AttAmmoByClips >= 0)
                if (const int64_t Ov = Target(P + L.AttClipOverride); Ov >= 0)
                    A["ammo_override"] = Json{ { "max_ammo", At<int32_t>(Img, Ov + L.AttAmmoMax) },
                                               { "reserve_clips", At<int32_t>(Img, Ov + L.AttAmmoReserve) },
                                               { "by_clips", At<uint8_t>(Img, Ov + L.AttAmmoByClips) } };
            if (L.AttClipOverride >= 0 && L.AttAmmoPellets >= 0)
                if (const int64_t Ov = Target(P + L.AttClipOverride); Ov >= 0) A["pellets_override"] = At<int32_t>(Img, Ov + L.AttAmmoPellets);
            if (L.AttClass >= 0)
                if (const uint8_t Cl = At<uint8_t>(Img, P + L.AttClass); Cl != 0) A["class_override"] = Cl;
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
            // the anim table: {id, xanim} of each sorted id (the xanim's name, its hash when unnamed)
            if (const int64_t A = L.PkgAnims >= 0 ? Target(P + L.PkgAnims) : -1; A >= 0 && L.PkgAnimCount >= 0 && A + L.PkgAnimCount + 8 <= (int64_t)Img.size())
            {
                const int64_t Ids = Target(A + L.PkgAnimIds), Xanims = Target(A + L.PkgAnimXanims);
                const int64_t Count = std::min<int64_t>(At<int64_t>(Img, A + L.PkgAnimCount), 0x10000);
                Json Anims = Json::array();
                for (int64_t J = 0; Ids >= 0 && Xanims >= 0 && J < Count; J++)
                {
                    const uint64_t X = RefId(Xanims + 8 * J, 0);
                    Anims.push_back(Json::array({ At<uint16_t>(Img, Ids + 2 * J),
                                                  X == 0 ? std::string() : L.AnimName ? L.AnimName(X) : Hex(X) }));
                }
                K["anims"] = std::move(Anims);
            }
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
    // the columns of the fields only some games give, each after its neighbour
    const std::vector<std::string> BaseHead = Head;
    const auto After = [&Head](const char* Neighbour, bool Has, std::initializer_list<const char*> Columns) {
        if (!Has) return;
        auto It = std::find(Head.begin(), Head.end(), Neighbour) + 1;
        for (const char* C : Columns) It = Head.insert(It, C) + 1;
    };
    After("burst_count", L.BurstDelay.At >= 0, { "burst_delay_ms" });
    After("muzzle_velocity_mps", L.BallisticBlock.At >= 0 && L.BallisticFlag >= 0, { "ballistic" });
    After(L.BallisticFlag >= 0 ? "ballistic" : "muzzle_velocity_mps", L.BallisticBlock.At >= 0 && L.BallisticGravity >= 0 && L.GravityInches > 0.0f,
          { "gravity_mps2" });
    After("explosion_radius_m", L.ExplosionDamage.At >= 0, { "explosion_inner_damage", "explosion_outer_damage" });
    After(L.ExplosionDamage.At >= 0 ? "explosion_outer_damage" : "explosion_radius_m", L.ExplosionRings.At >= 0,
          { "explosion_inner_radius_m", "explosion_mid_radius_m", "explosion_mid_damage" });
    After("prone_speed_scale", L.MoveSpeedScale.At >= 0, { "move_speed_scale" });
    After(L.MoveSpeedScale.At >= 0 ? "move_speed_scale" : "prone_speed_scale", L.AdsMoveSpeedScale.At >= 0, { "ads_move_speed_scale" });
    After("hit_soft", L.DefPenetration >= 0, { "penetration_level", "penetration_depth_scale" });

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
        std::map<std::string, std::string> Cells;
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
            // by magazines: the clip size times the magazine count
            else if (IntOr(T, "max_ammo_by_clips", 0) != 0 && T.contains("reserve_clips") && T.contains("clip_size"))
                MaxAmmo = std::to_string(T["clip_size"].get<int>() * T["reserve_clips"].get<int>());
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
            std::map<std::string, std::string> Cells;
            for (size_t I = 0; I < BaseHead.size(); I++) Cells[BaseHead[I]] = C[I];
            // the burst delay in whole ms: floor(t + 0.5)
            if (T.contains("burst_delay")) Cells["burst_delay_ms"] = std::to_string((int)std::floor((float)T["burst_delay"].get<double>() + 0.5f));
            if (T.contains("ballistic")) Cells["ballistic"] = std::to_string(T["ballistic"].get<int>());
            if (T.contains("gravity") && L.GravityInches > 0.0f) Cells["gravity_mps2"] = Num(L.GravityInches * T["gravity"].get<double>() * 0.0254, 2);
            // the explosion's damages, with its radius: the three rings' when the flag picks them
            if (Radius > 0)
            {
                const bool Rings = T.value("explosion_alt", 0) != 0 && T.contains("explosion_rings");
                if (Rings)
                {
                    const auto& R = T["explosion_rings"];
                    Cells["explosion_inner_damage"] = std::to_string(R[3].get<int>());
                    Cells["explosion_outer_damage"] = std::to_string(R[5].get<int>());
                    Cells["explosion_inner_radius_m"] = Num((double)((float)R[0].get<int>() / InchesPerMetre), 2);
                    Cells["explosion_mid_radius_m"] = Num((double)((float)R[1].get<int>() / InchesPerMetre), 2);
                    Cells["explosion_mid_damage"] = std::to_string(R[4].get<int>());
                }
                else if (T.contains("explosion_damage"))
                {
                    Cells["explosion_inner_damage"] = std::to_string(T["explosion_damage"][0].get<int>());
                    Cells["explosion_outer_damage"] = std::to_string(T["explosion_damage"][1].get<int>());
                }
            }
            Cells["move_speed_scale"] = FloatCell("move_speed_scale", 4);
            Cells["ads_move_speed_scale"] = FloatCell("ads_move_speed_scale", 4);
            if (W.contains("penetration_level")) Cells["penetration_level"] = std::to_string(W["penetration_level"].get<int>());
            if (W.contains("penetration_depth_scale")) Cells["penetration_depth_scale"] = Num(W["penetration_depth_scale"].get<double>(), 4);
            Table.push_back({ Name, K, std::move(Cells) });
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
    std::string AttCsv, LoadCsv, RulesCsv, PapCsv;
    std::set<std::string> WantedSettings;
    for (const auto& W : Weapons)
        if (W.contains("custom_settings")) WantedSettings.insert(W["custom_settings"].get<std::string>());
    BundleSet Bundles;
    const auto Paps = PapTable(L, Dir, WantedSettings, Bundles);
    if (!Paps.empty())
    {
        PapCsv = "weapon,base_attachment,pap_attachment,pap4_attachment,variant_when_holding,variant_pap,variant_pap4\n";
        for (const auto& [W, P] : Paps)
        {
            const std::string Head = CsvField(W) + "," + CsvField(P.Base) + "," + CsvField(P.Pap) + "," + CsvField(P.Pap4) + ",";
            if (P.Variants.empty()) PapCsv += Head + ",,\n";
            for (const auto& V : P.Variants) PapCsv += Head + CsvField(V.Holding) + "," + CsvField(V.Pap) + "," + CsvField(V.Pap4) + "\n";
        }
    }
    const size_t AttRows = AttachmentTables(L, Weapons, Tunables, Pkgs, Attachments, AttachmentNames, Strings, Dir, AttCsv, LoadCsv, RulesCsv, Paps);
    Readme += L.AttachmentNotes;
    if (!RulesCsv.empty()) Readme += L.RulesNotes;
    std::string PartsCsv;
    const size_t PartRows = L.DefaultAttachments >= 0 || L.DefaultSlots >= 0 ? DefaultPartsTable(L, Weapons, Attachments, AttachmentNames, Strings, PartsCsv) : 0;
    if (PartRows != 0) Readme += L.DefaultPartsNotes;
    std::string AnimCsv;
    const size_t AnimRows = L.PkgAnims >= 0 ? AnimTable(Weapons, Pkgs, AnimCsv) : 0;
    if (AnimRows != 0) Readme += L.AnimNotes;
    if (L.DefCustomSettings >= 0)
    {
        // each weapon's first tunables set's cells, for the weapons a custom settings bundle names
        std::map<std::string, std::map<std::string, std::string>> FirstRows;
        for (const auto& R : Table) FirstRows.try_emplace(R.Weapon, R.Cells);
        if (CustomSettingsFiles(L, Weapons, Strings, Dir, Bundles, Paps, FirstRows, AttCsv) != 0) Readme += L.CustomSettingsNotes;
    }
    std::error_code E2;
    fs::create_directories(Dir, E2);
    if (AttRows != 0)
    {
        ExportWriters::WriteBytes(fs::path(Dir) / "weapon_attachments.csv", (const uint8_t*)AttCsv.data(), AttCsv.size());
        if (!LoadCsv.empty()) ExportWriters::WriteBytes(fs::path(Dir) / "weapon_loadouts.csv", (const uint8_t*)LoadCsv.data(), LoadCsv.size());
        if (!PapCsv.empty()) ExportWriters::WriteBytes(fs::path(Dir) / "weapon_zm_pap.csv", (const uint8_t*)PapCsv.data(), PapCsv.size());
        if (!RulesCsv.empty()) ExportWriters::WriteBytes(fs::path(Dir) / "weapon_attachment_rules.csv", (const uint8_t*)RulesCsv.data(), RulesCsv.size());
    }
    if (PartRows != 0) ExportWriters::WriteBytes(fs::path(Dir) / "weapon_default_parts.csv", (const uint8_t*)PartsCsv.data(), PartsCsv.size());
    if (AnimRows != 0) ExportWriters::WriteBytes(fs::path(Dir) / "weapon_anims.csv", (const uint8_t*)AnimCsv.data(), AnimCsv.size());
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
            if (F.Base == Field::In::Def && L.DefIsWeapon) return "";
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
                                    { &L.SuperSprintSpeedScale, "super_sprint_speed_scale" }, { &L.ProneSpeedScale, "prone_speed_scale" },
                                    { &L.BurstDelay, "burst_delay" }, { &L.MoveSpeedScale, "move_speed_scale" },
                                    { &L.AdsMoveSpeedScale, "ads_move_speed_scale" } })
            Name(*F, 0, N, "f32");
        Name(L.ReserveClips, 0, "reserve_clips", "i32");
        Name(L.ExplosionDamage, 0, "explosion.centre_damage", "i32");
        Name(L.ExplosionDamage, 4, "explosion.edge_damage", "i32");
        static const char* Rings[] = { "inner_radius", "mid_radius", "outer_radius", "inner_damage", "mid_damage", "outer_damage" };
        // (the outer radius is explosion_radius_alt, already named)
        for (int K = 0; K < 6; K++)
            if (K != 2) Name(L.ExplosionRings, K * 4, std::string("explosion_rings.") + Rings[K], "i32");
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
        if (L.DefaultSlots >= 0) Put("", L.DefaultSlots, "default_attachment_slots", "");
        if (L.WeaponDef >= 0 || L.DefIsWeapon)
        {
            const std::string D = L.DefIsWeapon ? std::string() : Key(L.WeaponDef) + "/";
            Put(D, L.DefTunables, "tunables", "");
            Put(D, L.DefSets, "tunables_sets", "");
            Put(D, L.DefSetCount, "tunables_set_count", "i32");
            Put(D, L.DefSetTags, "tunables_set_tags", "");
            Put(D, L.DefAnimPkg, "animpkg", "");
            Put(D, L.DefOverrides, "animpkg_overrides", "");
            Put(D, L.DefOverrideCount, "animpkg_override_count", "i32");
            Put(D, L.DefCustomSettings, "custom_settings", "");
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
        Put("", L.AttDamageTable, "damage_table_override", "");
        Put("", L.AttDamageScales, "damage_range_scales", "");
        Put("", L.AttRangeScale, "range_scale", "f32");
        Put("", L.AttHit, "hit_multipliers_override", "");
        Put("", L.AttPenetration, "penetration_override", "");
        Put("", L.AttPenetrationAdd, "penetration_level_add", "i32");
        Put("", L.AttPerks, "perks", "");
        Put("", L.AttGroup, "group", "");
        Put("", L.AttSelfId, "self_id", "");
        Put("", L.AttExGroups, "excludes_groups", "");
        Put("", L.AttExSlots, "excludes_slots", "");
        Put("", L.AttMaxAmmoScale, "max_ammo_scale", "f32");
        Put("", L.AttWorldModel, "world_model", L.AttModelCount >= 0 ? "" : "hash");
        Put("", L.AttViewModel, "view_model", L.AttModelCount >= 0 ? "" : "hash");
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
