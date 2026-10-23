#include "stdafx.h"

// Call of Duty Online's weapon layout and attachment files (CodolWeapons.h).

#include "Games/CODOL/CodolWeapons.h"

#include "ExportNaming.h"

#include <cmath>
#include <format>
#include <set>

namespace
{
    using ExportNaming::At;
    using WeaponFiles::Clean;
}

namespace Codol
{
const char* const AttachmentTable = "mp/coweaponattachmenttable.csv";

IwWeaponDefs::Layout WeaponLayout()
{
    // VAs of the decrypted codoMP_client_shipRetail exe (image base 0x400000); offsets in the 0x101c
    // weapon asset (the WeaponDef itself)
    IwWeaponDefs::Layout L;
    L.Name = 0;
    L.Display = 4;
    L.Def = -1;
    L.HitsInline = true;
    L.OffsetsField = "codol.offsets";
    L.Fields = {
        { "weaponType", 0x588 },          { "weaponClass", 0x58c },       { "fireType", 0x598 },
        { "fireTime", 0xe70 },            { "shotCount", 0x7fc },         { "clipSize", 0xe68 },
        { "reloadTime", 0x85c },          { "reloadEmptyTime", 0x868 },   { "reloadAddTime", 0x870 },
        { "reloadEmptyAddTime", 0x878 },  { "reloadStartTime", 0x880 },   { "reloadStartAddTime", 0x884 },
        { "reloadEndTime", 0x888 },       { "reloadTimeAkimbo", 0x860 },  { "reloadEmptyTimeAkimbo", 0x86c },
        { "reloadAddTimeAkimbo", 0x874 }, { "reloadEmptyAddTimeAkimbo", 0x87c },
        { "dropTime", 0x88c },            { "raiseTime", 0x890 },         { "quickDropTime", 0x898 },
        { "quickRaiseTime", 0x89c },      { "emptyDropTime", 0x8a8 },     { "firstRaiseTime", 0xeb4 },
        { "adsTransInTime", 0xc28 },      { "adsTransOutTime", 0xc2c },
        { "damage", 0xd10 },              { "damage2", 0xd14 },           { "damage3", 0xd18 },
        { "damage4", 0xd1c },             { "damage5", 0xd20 },           { "minDamage", 0xd24 },
        { "maxDamageRange", 0xd28 },      { "damageRange2", 0xd2c },      { "damageRange3", 0xd30 },
        { "damageRange4", 0xd34 },        { "damageRange5", 0xd38 },      { "minDamageRange", 0xd3c },
        { "meleeDamage", 0x818 },
        { "explosionRadius", 0xa1c },     { "explosionRadiusMin", 0xa20 }, { "explosionInnerDamage", 0xa24 },
        { "explosionOuterDamage", 0xa28 }, { "projectileSpeed", 0xa38 },
        { "moveSpeedScale", 0x908 },      { "adsMoveSpeedScale", 0x90c },
        { "locationDamageMultipliers", 0xd4c },
        { "burstCount", 0xfc0 },
    };
    // the exe's name tables: weapon types (0xe5f8f0), classes (0xe5f908, read at 0x432130), fire types
    // (0xe5ebb4), hit locations (0xe4fb30, 20 read at 0x7b81b0)
    L.Types = { "bullet", "grenade", "projectile", "riotshield", "bomb", "melee" };
    // (13: past the exe's table, which stops at melee; the class of the seven bows, bow_mp... sparrow_mp,
    // named so here)
    L.Classes = { "rifle", "sniper", "mg", "smg", "spread", "pistol", "grenade", "rocketlauncher", "turret", "throwingknife", "non-player", "item",
                  "melee", "bow" };
    L.FireTypes = { "Full Auto", "Single Shot", "Burst", "Auto-Burst", "Charge Shot", "Charge Shot Burst", "Charge Shot Auto Burst",
                    "Option Shot", "Double Barrel" };
    L.HitLocations = { "none",           "helmet",          "head",           "neck",           "torso_upper",
                       "torso_lower",    "right_arm_upper", "left_arm_upper", "right_arm_lower", "left_arm_lower",
                       "right_hand",     "left_hand",       "right_leg_upper", "left_leg_upper", "right_leg_lower",
                       "left_leg_lower", "right_foot",      "left_foot",      "gun",            "shield" };
    auto& N = L.Notes;
    N.Title = "Call of Duty Online";
    N.Names = "The display names are the localized strings of zone localized_code_post_gfx_mp (its SF_WEAPON_<NAME>_MP_NAME\n"
              "keys, the Chinese release's text): export it into the same --out. A weapon a patch zone holds is read\n"
              "from it; zones lists every zone holding the weapon.\n";
    N.Source =
        "Call of Duty Online keeps no weapon field table in its exe (its weapon files are not shipped). Each weapon\n"
        "is the zone's weapon asset, one 0x101c WeaponDef (MW2's WeaponDef and WeaponCompleteDef in one struct, moved\n"
        "and extended), read field by field and written back as the weapon file the fields make, by MW2's field\n"
        "names (OpenAssetTools' IW4 weapon_fields). Times are milliseconds in the asset (written as the weapon file's\n"
        "seconds and read back), distances inches. Each field is read where the decrypted codoMP_client_shipRetail\n"
        "exe reads it (VAs, image base 0x400000):\n"
        "- weaponType 0x588 (the stats UI 0x7da5a0: 3 a riot shield, 5 a melee weapon), weaponClass 0x58c (named\n"
        "  by the exe's table 0xe5f908, read at 0x432130), fireType 0x598 (0x4322e0: 1 single shot, 2 a burst of\n"
        "  0xfc0 rounds); named by the exe's own tables (bullet...; rifle...; Full Auto, Single Shot, Burst...).\n"
        "  weaponClass 13 is past the class table (which stops at melee, 12): the seven bows' (bow_mp...\n"
        "  sparrow_mp), written bow, a name of this table's, not the exe's.\n"
        "- burstCount 0xfc0: the rounds of a burst (the stats UI 0x4322e0 reads it for fire type 2, Burst).\n"
        "- clipSize 0xe68 and fireTime 0xe70: the rounds a second of the stats UI (0x4322e0: clipSize x fireTime /\n"
        "  1000 to empty a clip); clipSize is the max ammo's magazine too (0x432e90, 0x42bf80).\n"
        "- shotCount 0x7fc: the pellets the bullet code fires (0x5923c0, 0x595df0), out to the last range 0xd3c.\n"
        "- reloadTime 0x85c, reloadEmptyTime 0x868: the reload state (0x4238c0) takes the empty one when the clip\n"
        "  is empty (0x4233b0), reloadTimeAkimbo 0x860 / reloadEmptyTimeAkimbo 0x86c when dual wielded;\n"
        "  reloadAddTime 0x870, reloadEmptyAddTime 0x878 (0x423660: the delay before the ammo is added, when\n"
        "  shorter than the reload; akimbo 0x874, 0x87c); reloadStartTime 0x880 and its add 0x884 (the same\n"
        "  function's start states), reloadEndTime 0x888 (0x424cd0: the reload end state).\n"
        "- dropTime 0x88c (quickDropTime 0x898, emptyDropTime 0x8a8: 0x423cf0), raiseTime 0x890, quickRaiseTime\n"
        "  0x89c, firstRaiseTime 0xeb4 (the raise states, 0x424400).\n"
        "- adsTransInTime 0xc28, adsTransOutTime 0xc2c: the ADS rates 1 / time (0x42d8d0; 300 and 500 ms when 0).\n"
        "- the damage table: six i32 damages at 0xd10 and six f32 ranges at 0xd28, where MW2 keeps damage, minDamage\n"
        "  and their two ranges; the stats UI's damage is 0xd10 (0x7da5a0), its range 0xd28-0xd30 (0x432510).\n"
        "- meleeDamage 0x818 (the stats UI's damage of a melee weapon, 0x7da5a0).\n"
        "- explosionRadius 0xa1c, explosionInnerDamage 0xa24, explosionOuterDamage 0xa28 (the radius damage call,\n"
        "  0x536460); explosionRadiusMin 0xa20 and projectileSpeed 0xa38 by MW2's order of those fields.\n"
        "- moveSpeedScale 0x908, adsMoveSpeedScale 0x90c (the move speed 0x415080 is multiplied by them).\n"
        "- locationDamageMultipliers: 20 f32 inline at 0xd4c, one per hit location of the exe's table (0xe4fb30, the\n"
        "  20 names read at 0x7b81b0: none, helmet, head... gun, shield), between MW2's damage fields and fireRumble.\n"
        "The fields keep MW2's order, in blocks moved together: those blocks hold MW2's values for most of the 48\n"
        "weapons both games name alike (the rebalanced damages, fire and reload times apart).\n";
    N.ExtraNotes =
        "\nCall of Duty Online\n"
        "- damage_<n> at range_<n>_m: six stops, as Black Ops II's (damage, damage2-5, minDamage; maxDamageRange,\n"
        "  damageRange2-5, minDamageRange). A stop whose range is 0 is left out (its damage is 0 too, for every\n"
        "  weapon of these zones). Ranges a unit apart (AK-47: 45 up to 1600, 41 from 1601, 33 at 2250 and 3000)\n"
        "  look like steps; the damage at a range is computed by the server, not this client exe: not read.\n"
        "- start_ammo, max_ammo: empty. The max ammo (0x42bf80) is clipSize x a count of the game mode's\n"
        "  mp/gametypeWeaponCustomTable.csv (0x432130) plus 0x7f8; the start ammo is not read in the exe.\n"
        "- sprint_out_ms: empty (sprintOutTime is read nowhere in the exe; sprintInTime 0x8ac is).\n"
        "- Not applied: the game mode's scales (mp/gametypeWeaponCustomTable.csv and its _damageRange table), the\n"
        "  alternate damage table of the altfire weapons (0xfe4, 0xffc: read when the shot's flag 2 is set,\n"
        "  0x42cc60), the marketplace's buffs.\n\n";
    return L;
}

std::vector<WeaponFiles::Weapon> AttachmentFiles(const IwWeaponStats::ZoneView& V)
{
    // weaponattach (0xc): its name at 0, the u32 count at 4, the 12-byte overrides at 8 {u8 type, u8
    // operation, u16, u16 WeaponDef offset at 4, value at 8}; the number fields' (types 4 i32, 7 f32, 8 a
    // speed, 9 a time; their value an f32)
    const auto& Img = *V.Image;
    const auto& R = *V.Walk;
    const auto Fits = [&](int64_t P, int64_t N) { return P >= 0 && P + N <= (int64_t)Img.size(); };
    std::vector<WeaponFiles::Weapon> Out;
    std::set<std::string> Done;
    for (const auto& I : R.Instances)
    {
        if (I.Owner >= R.OwnerNames.size() || R.OwnerNames[I.Owner] != "weaponattach" || !Fits(I.Pos, 0xc)) continue;
        const std::string Name = V.Text(I.Pos);
        if (Name.empty() || Name[0] == ',' || !Done.insert(Name).second) continue;
        const uint32_t Count = At<uint32_t>(Img, I.Pos + 4);
        const uint32_t Ptr = At<uint32_t>(Img, I.Pos + 8);
        const int64_t E = Ptr != 0 ? V.Off(Ptr) : -1;
        std::string Lines;
        for (uint32_t K = 0; E >= 0 && K < Count && K < 4096 && Fits(E + 12 * (int64_t)K, 12); K++)
        {
            const int64_t P = E + 12 * (int64_t)K;
            const int Type = At<uint8_t>(Img, P), Op = At<uint8_t>(Img, P + 1);
            if (Type != 4 && Type != 7 && Type != 8 && Type != 9) continue;
            const float X = At<float>(Img, P + 8);
            if (!std::isfinite(X)) continue;
            Lines += std::format("{} {} 0x{:x} {};", Type, Op, At<uint16_t>(Img, P + 4), X);
        }
        Out.push_back({ Name, "ATTACHMENTFILE\\codol.format\\1\\codol.overrides\\" + Clean(Lines), {} });
    }
    return Out;
}
}
