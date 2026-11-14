#include "stdafx.h"

#include "WeaponStats.h"

#include "Games/BO7/GameBlackOps7.h"

// Every offset below is proven by the exe (RVAs of C:\tmp\mw4\mw4_flat.exe, cod.exe PE timestamp
// 0x6add5b92), found from the GSC builtins of its builtin table (file 0xcdc5350.., names by their
// script hash) and disassembled. A weapon's definition is the weapon asset itself (0x1760): every
// getter takes the weapon of a weapon index from the table at 0xe23cc10 and reads its tunables
// through 0xd68. Fields Black Ops 7 names and this game's code was not read for are left out (-1).
namespace MW4
{
const IwWeaponStats::Layout& WeaponStatsLayout()
{
    static const IwWeaponStats::Layout L = [] {
        using F = IwWeaponStats::Field;
        IwWeaponStats::Layout L;
        L.WeaponName = 8, L.DefIsWeapon = true;
        // the tunables sets, Black Ops 7's {tunables **, u32 count, tags {u32 count, u64 *} per set}
        // and the set in use: tunables 0xd68 (every getter below reads it), the sets 0xd50 (count
        // 0xd58, their tags 0xd60: rex_ar_mike4_mp holds #1 to #4, #1 tagged "default", the one 0xd68
        // names in the zone)
        L.DefSets = 0xd50, L.DefSetCount = 0xd58, L.DefSetTags = 0xd60, L.DefTunables = 0xd68;
        // the tags' names: their FNV-1a 64 (63 bits) is the tag; two more tags of the guns
        // (7e06fd237021ee60, 22fed8e110ca51d5) are not named yet
        L.TagNames = { "default", "rex_wz" };
        // weaponfiretime (0x6bdbaf0 -> 0x31768b0): i32 0x4df0 (0x3176d51; 0x4e0c in the alternate
        // mode, 0x3176cd3), returned * 0.001
        L.FireTime = F(0x4df0);
        // weaponburstcount (0x6bdc790 -> 0x3155fa0): i32 0x4e38 (0x3155fe5)
        L.BurstCount = F(0x4e38);
        // weaponclipsize (0x6bdbe40 -> 0x327fd90 -> 0x314e040): i32 0x4e64 (0x314e09c), then scaled by
        // the attachments (not applied here)
        L.ClipSize = F(0x4e64);
        // weaponstartammo (0x6bddac0 -> 0x315eec0): i32 0x4e54 when not 0 (0x315ef9a), else 0x4e50
        L.StartAmmo = F(0x4e50), L.StartAmmoOverride = F(0x4e54);
        // weaponmaxammo (0x6bddc10 -> 0x315e710): i32 0x4e60 (0x315e7ff; 0x4e58 when not 0, 0x315e7f1);
        // with u8 0x4f3f set the reserve counts magazines, i32 0x4e7c of them (0x315e912, 0x315e919)
        L.MaxAmmo = F(0x4e60), L.MaxAmmoByClips = F(0x4f3f), L.ReserveClips = F(0x4e7c);
        L.NameHash = [](const std::string& Name) { return GameBlackOps7::HashIWAsset(Name); };
        L.Title = "Modern Warfare 4 weapon stats";
        L.Columns =
            "\nThe guns are in global_stream_mp (and global_stream_cfwd_mp); export those zones into the same --out\n"
            "and the table is rebuilt from every facts file. Only the fields Modern Warfare 4's exe was read for\n"
            "are filled; the others (damage, ranges, reload and ADS times, speeds, fire type) stay empty.\n\n"
            "Columns and units (RVAs of the decrypted cod.exe, PE timestamp 0x6add5b92)\n"
            "- set: the tunables set (weapon 0xd50[set-1], tags 0xd60). The weapon asset is its own definition;\n"
            "  every getter reads the tunables through weapon 0xd68.\n"
            "- fire_time_ms: tunables i32 0x4df0 (weaponfiretime 0x6bdbaf0 -> 0x31768b0; 0x4e0c in an alternate\n"
            "  mode). rpm = 60000 / fire_time_ms.\n"
            "- burst_count: i32 0x4e38 (weaponburstcount 0x6bdc790 -> 0x3155fa0).\n"
            "- clip_size: i32 0x4e64 (weaponclipsize 0x6bdbe40 -> 0x314e040), before attachments.\n"
            "- start_ammo: i32 0x4e54 when not 0, else 0x4e50 (weaponstartammo 0x6bddac0 -> 0x315eec0).\n"
            "- max_ammo: i32 0x4e60 (weaponmaxammo 0x6bddc10 -> 0x315e710; i32 0x4e58 overrides it when not 0);\n"
            "  with u8 0x4f3f set the reserve counts magazines: clip_size x i32 0x4e7c.\n";
        return L;
    }();
    return L;
}
}
