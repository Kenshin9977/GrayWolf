#include "stdafx.h"

// Modern Warfare 4 on the disk template: Black Ops 7's engine (Games/BO7/BO7Family.h), its zones read
// by Black Ops 7's zone class with the mw4 profile and the offsets this game's loaders moved (the
// mw4 tables of Wolff's profiles/mw4 against bo7's, field by field). The state and what is left:
// graywolf-tools/docs/games/MW4.md.

#include "DiskGame.h"
#include "Games/BO7/BO7Family.h"
#include "Games/MW4/VfxLayout.h"
#include "Games/MW4/WeaponStats.h"
#include "Games/IW/IwctxReaders.h"

#include <format>

namespace
{
    // The multiplayer's Spec (cod26, profile mw4); the campaign's starts from it
    BO7Family::Spec Multiplayer()
    {
        BO7Family::Spec S;
        S.Id = "mw4", S.Title = "Call of Duty: Modern Warfare 4", S.Engine = "MW4", S.LiveGame = "MW4";
        S.Profile = "mw4", S.Folder = "cod26";
        // the package index's names: HashIWAsset is Black Ops 7's FNV-1a 64 (the xmodels' hash at 0
        // is it of the name at 8, 130 of 130 on ingame_mp), so the Black Ops 7 and 6 packages name
        // the assets this game shares with them (its sounds most: ingame_mp 538 of 6 919)
        S.NamePackages = { "bo7_names.wni", "bo6_names.wni" };
        // (the index lists Black Ops 7's parts: xmodelsurfs, xmodel, material, image; parts1 had
        // the xmodelsurfs alone)
        S.IndexSuffix = "+parts2";
        // soundbank (0x48, 0x1d24830): aliases 0x20, u32 count 0x38 (Black Ops 7: 0x34); an alias
        // (0x48, 0x1c5b5b0) as Black Ops 7's
        S.SoundLinks = { 0x20, 0x38, 0x48, 0x18, 0x10, 0x34, 0x30, 0x20 };
        // particlesystem (0x80): Black Ops 7's, two modules larger (Games/MW4/VfxLayout.cpp)
        S.VfxLayout = &MW4::VfxLayout;
        // a script bundle value (its loader 0x20a0360, a hand table) of type 10 points to {an
        // animtree's string record}: animtree keys (Black Ops 7 reads nothing for 10); the other
        // codes read as Black Ops 7's (6/7 a table, 8 an array, 9/18 a record, 5 a string)
        S.BundleTypes = { { 10, BundleValue::Anim } };
        // scriptable (0xe8, 0x1fd6e10) and scriptablevariant (0x58): dumped by offset
        S.Scriptables = { { "scriptable" }, { "scriptablevariant" } };
        // the weapon assets as struct dumps, a weapon's name at 8 (0x1760), an attachment's at 0x210
        // (0x6f0), the fields the weapon stats prove named (Games/MW4/WeaponStats.cpp); the weapon
        // stats table from those fields
        {
            const auto& L = MW4::WeaponStatsLayout();
            S.Weapons = { { "weapon", 8, IwWeaponStats::DumpFields(L, "weapon") }, { "attachment", 0x210 },
                          { "weapontunables", -1, IwWeaponStats::DumpFields(L, "weapontunables") }, { "weaponaccuracy" }, { "weapontrigger" },
                          { "weaponarchetype" }, { "weaponblueprint" }, { "attachmentblueprint" }, { "grappleweapontunables" },
                          { "deployableweaponconfig" }, { "animpkg" }, { "suit" } };
            S.Stats = [] { return &MW4::WeaponStatsLayout(); };
        }
        // gfx_map_trzone (0x190): its draw data inline at 0x20 (Black Ops 7: 0x18)
        S.TzIndex = 0x18, S.TzDraw = 0x20;
        // map_ents (0x978): no spawn point list (map_spawn_points holds them); the scriptable
        // instances' struct at 0x308 (Black Ops 7: 0x318): count u32 0x308, 0x60 records at 0x378
        S.SpawnCount = -1, S.ScriptableCount = 0x308, S.ScriptableRecords = 0x378;
        // map_spawn_points (0x18, 0x2070b70): the list map_ents had, inline at 8 (u16 count, 0x48
        // records at 0x10, the record's table 0x20c7500 as Black Ops 7's)
        S.SpawnPoints = 8;
        // scriptable (0xe8): its models u32 0xc4, xmodel pointers 0xc8 (Black Ops 7: 0xb4, 0xb8)
        S.ScriptableSize = 0xe8, S.ScriptableModelCount = 0xc4, S.ScriptableModels = 0xc8;
        S.Terrain = &IwTerrain::ModernWarfare4;
        return S;
    }

    const BO7Family::Spec& ModernWarfare4()
    {
        static const BO7Family::Spec S = Multiplayer();
        return S;
    }

    // The campaign (sp26, its own sp26-cod.exe, profile mw4sp): a later build of the same engine,
    // its tables generated from its own exe. The mw4sp tables against mw4's, field by field
    // (graywolf-tools docs/games/MW4.md, "Campaign"): xmodel, xmodelsurfs, material, image, xanim,
    // sndasset, particlesystem's modules, mapinfo, aitype, map_spawn_points and gfx_map's surfaces
    // and static models read the same; what moved:
    const BO7Family::Spec& ModernWarfare4Campaign()
    {
        static const BO7Family::Spec S = [] {
            BO7Family::Spec S = Multiplayer();
            // its live readers "MW4SP" (Games/MW4/LiveGame.cpp): the anim header is Black Ops 7's
            S.Id = "mw4sp", S.Title = "Call of Duty: Modern Warfare 4 (campaign)", S.Engine = "MW4SP", S.LiveGame = "MW4SP";
            S.Profile = "mw4sp", S.Folder = "sp26";
            // a level loads after code_pre_gfx, code_post_gfx, global, global_sp, ingame and ingame_sp
            // (sp26 has no _mp zone); the campaign's anim sets and characters are in global_sp and ingame_sp
            S.LevelZones = { "code_pre_gfx", "code_post_gfx", "global", "global_sp", "ingame", "ingame_sp" };
            S.AnimZones = { "global_sp", "ingame_sp" };
            // soundbank (0x40, 0x1e65e50): aliases 0x20, u32 count 0x34, as Black Ops 7's
            S.SoundLinks = { 0x20, 0x34, 0x48, 0x18, 0x10, 0x34, 0x30, 0x20 };
            // weapon (0x1710, 0x2226a30): its name at 8; attachment (0x9f8, 0x2224110) at 0x318; the
            // weapon stats' fields were proven on the multiplayer's exe only (weapontunables 0x4fa0
            // there, 0x4be8 here): no stats, the structs dumped by offset
            S.Weapons = { { "weapon", 8 }, { "attachment", 0x318 }, { "weapontunables" }, { "weaponaccuracy" }, { "weapontrigger" },
                          { "weaponarchetype" }, { "weaponblueprint" }, { "attachmentblueprint" }, { "grappleweapontunables" },
                          { "deployableweaponconfig" }, { "animpkg" }, { "suit" } };
            S.Stats = nullptr;
            // gfx_map_trzone (0x188, 0x21891d0): its index i32 0x14, its draw data inline at 0x18, as
            // Black Ops 7's (sp_rex_defection_def_ne_rock_formation_05_..._0_tr: index 0x33)
            S.TzIndex = 0x14, S.TzDraw = 0x18;
            // map_ents (0x948): the scriptable instances' struct still at 0x308 (count u32 0x308, 0x60
            // records at 0x378, the multiplayer's); scriptable (0xd8, 0x21d4f40): its models u32 0xb4, pointers 0xb8, as Black Ops 7's
            S.ScriptableSize = 0xd8, S.ScriptableModelCount = 0xb4, S.ScriptableModels = 0xb8;
            S.Terrain = &IwTerrain::ModernWarfare4Campaign;
            return S;
        }();
        return S;
    }

    class ModernWarfare4Game : public BO7Family::Game
    {
    public:
        explicit ModernWarfare4Game(const BO7Family::Spec& S = ModernWarfare4()) : Game(S) {}
        // its maps by the presence strings (code_post_gfx's map table names mp_rex_nautical
        // PRESENCE/MP_REX_NAUTICAL, "Nautical"), in English (eng_code_post_gfx), a key as the zones
        // name it hashed (localize_<FNV-1a 64 of the key>, Black Ops 7's asset hash); a level the strings
        // do not name (or whose text the rebuilt cipher got wrong) by file name: mp_rex_nautical with
        // its _entities, _subset_ai, ww_, techsets_ and language zones, mp_rew_escape with its
        // transient _tr ones
        const LevelNaming* LevelNames() const override
        {
            static const LevelNaming Naming = [] {
                LevelNaming N;
                N.From = { "eng_code_post_gfx" };
                N.Keys = { { "mp_*", { "PRESENCE/{NAME}" } } };
                N.Hashed = [](const std::string& Key) { return std::format("localize_{:x}", IwctxReaders::AssetHash(Key)); };
                N.ByFileName = { "mp_*", "sp_*", "cp_*", "wz_*" };
                return N;
            }();
            return &Naming;
        }
    };

    const ModernWarfare4Game Game, Campaign(ModernWarfare4Campaign());
    const DiskGame::Registration Registered(Game), RegisteredCampaign(Campaign);
}
