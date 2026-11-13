#include "stdafx.h"

#include "Games/IW4/OriginalWeapons.h"

namespace Iw64
{
    namespace
    {
        // OAT's names (ObjCommon/Game/IW4, IW5 WeaponFields.h / EnumStrings.h): MW2's weapType has no
        // "none", MW3's starts with it; the hit locations are hitLocation_t's first 20 (none .. shield),
        // the 80 bytes the tables read for locationDamageMultipliers
        const std::vector<std::string> Classes = { "rifle", "sniper", "mg", "smg", "spread", "pistol", "grenade", "rocketlauncher",
                                                   "turret", "throwingknife", "non-player", "item" };
        const std::vector<std::string> FireTypes = { "Full Auto", "Single Shot", "2-Round Burst", "3-Round Burst", "4-Round Burst",
                                                     "Double Barrel" };

        // the anim fields by weapAnimFiles_t (OAT's WeaponFields.h names; the root, 0, has none):
        // MW2's 37, MW3's 42 (the blasts before adsUp, altAdjust last)
        std::vector<std::string> AnimNames(bool Mw3)
        {
            std::vector<std::string> N = { "", "idleAnim", "emptyIdleAnim", "fireAnim", "holdFireAnim", "lastShotAnim", "rechamberAnim",
                                           "meleeAnim", "meleeChargeAnim", "reloadAnim", "reloadEmptyAnim", "reloadStartAnim",
                                           "reloadEndAnim", "raiseAnim", "firstRaiseAnim", "breachRaiseAnim", "dropAnim", "altRaiseAnim",
                                           "altDropAnim", "quickRaiseAnim", "quickDropAnim", "emptyRaiseAnim", "emptyDropAnim",
                                           "sprintInAnim", "sprintLoopAnim", "sprintOutAnim", "stunnedAnimStart", "stunnedAnimLoop",
                                           "stunnedAnimEnd", "detonateAnim", "nightVisionWearAnim", "nightVisionRemoveAnim",
                                           "adsFireAnim", "adsLastShotAnim", "adsRechamberAnim" };
            if (Mw3) N.insert(N.end(), { "blastFrontAnim", "blastRightAnim", "blastBackAnim", "blastLeftAnim" });
            N.insert(N.end(), { "adsUpAnim", "adsDownAnim" });
            if (Mw3) N.push_back("altAdjustAnim");
            return N;
        }

        // Complete: the WeaponCompleteDef's fields (adsTransIn, adsTransOut, clipSize, fireTime,
        // firstRaiseTime); Def: the WeaponDef's (types, ammo, damage, then the state timers from
        // Timers: reload, reloadEmpty, reloadAdd, reloadStart, reloadEnd, drop, raise, quickRaise,
        // sprintOut at +0, +8, +0xc, +0x10, +0x18, +0x1c, +0x20, +0x2c, +0x44), explosion,
        // projectile speed, the damage falloff, the hit multipliers, the move speed scales (move, ads)
        struct Offsets
        {
            int64_t AdsIn, AdsOut, Clip, Fire, FirstRaise;
            int64_t Type, Class, FireType, StartAmmo, MaxAmmo, Shots, Damage, Melee, Timers, Explosion, Projectile, MinDamage,
                MaxRange, MinRange, Hits, MoveSpeed;
        };

        // XAnims: the WeaponCompleteDef's szXAnims; Overrides: its numAnimOverrides (the pointer after
        // it), -1 none (MW2)
        IwWeaponDefs::Layout Make(const char* Title, const char* Where, const char* Checked, const std::vector<std::string>& Types,
                                  const Offsets& O, int64_t XAnims, int64_t Overrides)
        {
            IwWeaponDefs::Layout L;
            L.Name = 0, L.Def = 4, L.Display = 8, L.PointerSize = 4;
            // the models and anims (the anim owners'): the WeaponDef's gunXModel, handXModel,
            // szXAnimsRightHanded, szXAnimsLeftHanded at 4, 8, 0xc, 0x10 in both; AnimOverrideEntry 0x18
            L.GunModels = 4, L.HandModel = 8;
            L.AnimNames = AnimNames(Overrides >= 0);
            L.AnimLists = { { XAnims, false, "" }, { 0xc, true, "R" }, { 0x10, true, "L" } };
            if (Overrides >= 0) L.AnimOverrideCount = Overrides, L.AnimOverrides = Overrides + 4, L.AnimOverrideSize = 0x18;
            L.Types = Types, L.Classes = Classes, L.FireTypes = FireTypes;
            L.HitLocations.resize(20);
            const auto C = [](const char* N, int64_t At) { return IwWeaponDefs::Field{ N, At, false }; };
            L.Fields = { { "weaponType", O.Type }, { "weaponClass", O.Class }, { "fireType", O.FireType }, { "startAmmo", O.StartAmmo },
                         { "maxAmmo", O.MaxAmmo }, C("clipSize", O.Clip), { "shotCount", O.Shots }, { "damage", O.Damage },
                         { "meleeDamage", O.Melee }, C("fireTime", O.Fire), { "reloadTime", O.Timers },
                         { "reloadEmptyTime", O.Timers + 0x8 }, { "reloadAddTime", O.Timers + 0xc }, { "reloadStartTime", O.Timers + 0x10 },
                         { "reloadEndTime", O.Timers + 0x18 }, { "dropTime", O.Timers + 0x1c }, { "raiseTime", O.Timers + 0x20 },
                         { "quickRaiseTime", O.Timers + 0x2c }, C("firstRaiseTime", O.FirstRaise), { "sprintOutTime", O.Timers + 0x44 },
                         C("adsTransInTime", O.AdsIn), C("adsTransOutTime", O.AdsOut), { "explosionRadius", O.Explosion },
                         { "explosionRadiusMin", O.Explosion + 4 }, { "explosionInnerDamage", O.Explosion + 8 },
                         { "explosionOuterDamage", O.Explosion + 0xc }, { "projectileSpeed", O.Projectile }, { "minDamage", O.MinDamage },
                         { "maxDamageRange", O.MaxRange }, { "minDamageRange", O.MinRange }, { "locationDamageMultipliers", O.Hits },
                         { "moveSpeedScale", O.MoveSpeed }, { "adsMoveSpeedScale", O.MoveSpeed + 4 } };
            L.Notes = IwWeaponDefs::GameNotes(Title, "the zone's own localize", Checked, Where,
                                              "the same weapons' fields in the 64-bit rebuild, read through OpenAssetTools");
            L.Notes.Source = std::string(Title) +
                             "'s weapons are read from its zones (OpenAssetTools' x64 build reads the rebuild's only):\n"
                             "each one from " + Where + ",\nwritten back as the weapon file those fields make (the names "
                             "OpenAssetTools' weapon files use). Times are milliseconds in the asset\n(written as seconds), "
                             "distances inches. Fields not read are left out (their columns stay empty).\n";
            return L;
        }
    }

    // OAT's IW4_Assets.h for x86: WeaponCompleteDef 0x74 (the iw4x86 weapon table fedf50), WeaponDef
    // 0x684 (feceb0, its locationDamageMultipliers 80 bytes at 0x5b4); szXAnims at 0x10
    const IwWeaponDefs::Layout& Mw2x86Weapons()
    {
        static const IwWeaponDefs::Layout L =
            Make("Modern Warfare 2 (32-bit)",
                 "its WeaponCompleteDef (0x74) and WeaponDef (0x684) at the offsets of OpenAssetTools' IW4_Assets.h built for x86",
                 "so_crossing_so_bridge and mp_rust", { "bullet", "grenade", "projectile", "riotshield" },
                 { 0x18, 0x1c, 0x20, 0x28, 0x54, 0x2c, 0x30, 0x3c, 0x208, 0x21c, 0x220, 0x230, 0x238, 0x26c, 0x3e8, 0x404, 0x598, 0x5a0,
                   0x5a4, 0x5b4, 0x2f4 },
                 0x10, -1);
        return L;
    }

    // IW5_Assets.h for x86: WeaponCompleteDef 0xb8 (iw5x86 fc7e60), WeaponDef 0x7a4 (fc6d40, its
    // multipliers at 0x6b8), the timers in its stateTimers (0x274); szXAnims at 0x1c, then
    // numAnimOverrides and animOverrides
    const IwWeaponDefs::Layout& Mw3x86Weapons()
    {
        static const IwWeaponDefs::Layout L =
            Make("Modern Warfare 3 (32-bit)",
                 "its WeaponCompleteDef (0xb8) and WeaponDef (0x7a4) at the offsets of OpenAssetTools' IW5_Assets.h built for x86",
                 "so_ied_berlin", { "none", "bullet", "grenade", "projectile", "riotshield" },
                 { 0x4c, 0x50, 0x54, 0x5c, 0x94, 0x2c, 0x30, 0x3c, 0x210, 0x224, 0x228, 0x238, 0x240, 0x274, 0x4e0, 0x4fc, 0x69c, 0x6a4,
                   0x6a8, 0x6b8, 0x3e4 },
                 0x1c, 0x20);
        return L;
    }
}
