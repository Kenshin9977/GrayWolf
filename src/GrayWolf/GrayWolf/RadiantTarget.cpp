#include "stdafx.h"

#include "RadiantTarget.h"

#include "ExportNaming.h"
#include "MaterialGdt.h"

#include <limits>
#include <set>

namespace Maps
{
namespace
{
    bool Starts(const std::string& S, const char* Prefix) { return S.rfind(Prefix, 0) == 0; }

    class Plain final : public IRadiantTarget
    {
    public:
        explicit Plain(std::string Stem) : Map(std::move(Stem)) {}
        const std::string& MapName() const override { return Map; }
        const std::string& NamePrefix() const override { return None; }
        bool Admit(Entity&, const MapScene&, RadiantStats&) const override { return true; }
        std::string ModelName(const std::string& Model) const override { return Model; }
        const char* ModelExtension() const override { return ".XMODEL_EXPORT"; }
        double WorldLimit() const override { return std::numeric_limits<double>::infinity(); }
        bool PlacesUnwrittenModels() const override { return true; }
        double ModelTriangleSpan() const override { return std::numeric_limits<double>::infinity(); }
        // every one, BO1's of a negative scale too
        float MinModelScale() const override { return -std::numeric_limits<float>::infinity(); }
        // as the clip map has them: coplanar (about 0.08 degrees)
        Merge CollisionMerge() const override { return { 0.01, 1.0 - 1e-6 }; }
        double CollisionPlaneBudget() const override { return std::numeric_limits<double>::infinity(); }
        double BrushSideBudget() const override { return std::numeric_limits<double>::infinity(); }
        size_t MaxModelSurfaces() const override { return std::numeric_limits<size_t>::max(); }
        size_t MaxMaterialVertices() const override { return std::numeric_limits<size_t>::max(); }
        double MinPatchTriangleArea() const override { return 0.0; }
        size_t MaxBrushSides() const override { return std::numeric_limits<size_t>::max(); }
        double CollisionTriangleBudget() const override { return std::numeric_limits<double>::infinity(); }
        Margin PlayMargin() const override { return { 0.0, 0.0 }; }
        Limits HardLimits() const override { return {}; }

    private:
        std::string Map, None;
    };

    // In a mod tools tree: the map named after its game too (CoD4's and MWR's mp_shipment never
    // share a .map, a usermap or an asset name there), every asset name the map's own, so two maps
    // or BO3's stock assets (lambert1) never define one twice; XMODEL_BIN, the linker refusing
    // XMODEL_EXPORT version 6.
    class Bo3ModTools final : public IRadiantTarget
    {
    public:
        Bo3ModTools(const std::string& Game, const std::string& Stem)
            : Map(Game.empty() ? Stem : Game + "_" + Stem), Prefix(MaterialGdt::AssetName(Map) + "_") {}
        const std::string& MapName() const override { return Map; }
        const std::string& NamePrefix() const override { return Prefix; }
        std::string ModelName(const std::string& Model) const override { return MaterialGdt::PrefixedName(Prefix, Model); }
        const char* ModelExtension() const override { return ".XMODEL_BIN"; }
        // the engine's world ends at 131072 units (2^17) on each axis: beyond, the linker asserts
        // on a static model's box (MWR's mp_shipment keeps scenery out there); a tile inside it,
        // the sky box 1088 units out still in
        double WorldLimit() const override { return 131072.0 - 2048.0; }
        bool PlacesUnwrittenModels() const override { return false; }
        // a model holding a triangle tens of thousands of units long (a vista's ground: MW2's
        // trainer, MWR's mp_shipment) gets a box the linker cannot sort ("boxside of 0 in a
        // static model"): a tile's size at most, the rest patches of the world
        double ModelTriangleSpan() const override { return 2048.0; }
        // a placement scaled to nothing (BO1 hides some so): the linker ends on "has scale of 0.0"
        float MinModelScale() const override { return 1e-3f; }
        // as the clip map has them (coplanar): merged half a unit and 2 degrees apart, then made
        // prisms 2 units deep, they stood the player up to 8 units above BO1's mp_firingrange's
        // obstacles (measured on the compiled port, against BO1's clip map)
        Merge CollisionMerge() const override { return { 0.01, 1.0 - 1e-6 }; }
        // every collision triangle a patch of its own, at its exact height, which takes no plane
        // (cod2map's MAX_MAP_PLANES, 131072, ended Ghosts' and AW's prisms): no prism at all
        double CollisionPlaneBudget() const override { return 0.0; }
        // cod2map alone on variants of IW's mp_flip (cod2map_probe): 153912 brush sides stop on
        // MAX_MAP_PLANES_COMPILE, about 86000 pass; Ghosts' mp_prisonbreak built with 72110
        double BrushSideBudget() const override { return 100000.0; }
        // the linker keeps a model's surface count in a byte: Ghosts' dense tiles had 256 and more
        // ("(SMallType) 0x0 == (BigType) 0x100")
        size_t MaxModelSurfaces() const override { return 200; }
        // the linker makes one draw surface of a model's surfaces of one material, of 16-bit
        // indices ("surf->numverts <= (64 * 1024 - 2)": Cold War's p9_wz_silo_panel_metal); some
        // room for the vertices it adds
        size_t MaxMaterialVertices() const override { return 60000; }
        // cod2map asserts on a collision patch whose triangle it drops ("tot_faces == triCount",
        // AW): slivers of under a square unit left out
        double MinPatchTriangleArea() const override { return 1.0; }
        // "Winding point limit (64) exceeded on brush face" (BO4's mp_frenetic: a cylinder's)
        size_t MaxBrushSides() const override { return 60; }
        // the maps built so far kept up to 95000 (Ghosts' mp_prisonbreak 89806); IW's mp_flip
        // (122988) stopped cod2map on MAX_MAP_PLANES_COMPILE
        double CollisionTriangleBudget() const override { return 95000.0; }
        Margin PlayMargin() const override { return { 4096.0, 2048.0 }; }
        // the linker's byte of surfaces (0x100 asserts) and 16-bit indices (64 * 1024 - 2),
        // cod2map's asset names (64), the brush sides of IW's mp_flip that stopped cod2map once
        // its largest brush models were left out (about 86000 built, 153912 did not)
        Limits HardLimits() const override { return { 255, 64 * 1024 - 2, 64, 153000 }; }
        // static, lit per vertex: a static model is otherwise lit from one point, a tile's the
        // world's origin, under BO1's mp_firingrange's ground (its floors black). Treyarch's own
        // long models (barbed wire, sandbag rows, boards) carry "vertexbake" "1.0" so (map_source/_prefabs)
        std::vector<std::pair<std::string, std::string>> WorldModelKeys() const override
        {
            return { { "static", "1" }, { "vertexbake", "1.0" } };
        }

        // Left out: the source's single-player logic (LeftOut), the misc_model entities its
        // placements stand for (CoD1 and CoD2 keep their placed models so; the copy would name
        // "xmodel/<name>", no asset), a script_model whose model no file of the export holds (no
        // such xmodel in BO3: the compiler refuses it, and one without a model too). A
        // script_origin as a script_struct: the point for scripts, which takes none of the
        // game's limited entities.
        bool Admit(Entity& E, const MapScene& Scene, RadiantStats& Stats) const override
        {
            const auto Class = E.find("classname");
            const std::string C = Class == E.end() ? std::string() : Class->second;
            // another game's breakable glass names a glass type BO3 does not have ("glass_dec_glass02":
            // the linker warns "is not a valid glass"), and the level then fails to load
            const bool OtherGlass = C == "glass" && Scene.Game != "bo3";
            // a multiplayer mode's own (script_gameobjectname: its bomb zones, HQs, the cover BO1's
            // mp_nuked adds across doorways), which that game's scripts delete in every other
            // mode: kept in a zombies level, its collision stands as invisible walls
            if (LeftOut(C) || OtherGlass || C == "misc_model" || E.count("weaponinfo") || E.count("script_gameobjectname"))
            {
                Stats.EntitiesLeftOut++;
                return false;
            }
            if (const auto M = E.find("model");
                M != E.end() && !M->second.empty() && M->second[0] != '*' && M->second[0] != '?' && !Scene.Models.count(M->second))
            {
                Stats.ModelsLeftOut++;
                if (C == "script_model")
                {
                    Stats.EntitiesLeftOut++;
                    return false;
                }
                E.erase(M);
            }
            if (C == "script_origin")
            {
                E["classname"] = "script_struct";
                Stats.OriginsAsStructs++;
            }
            return true;
        }

    private:
        std::string Map, Prefix;

        // What a level Black Ops III builds has no use for, of an older game's entities: its AI
        // spawners and vehicles (BO3 has no such aitype or vehicle; the linker refuses them), path
        // and cover nodes (BO3's AI walk the navmesh its compiler makes), weapons lying about and
        // turrets (the source game's weapons; any entity with a "weaponinfo", CoD2's misc_mg42 too),
        // the multiplayer helicopters' ceiling (a zombies level crashes spawning heli_height_lock:
        // strtol on a null key, BO1's mp_firingrange)
        static bool LeftOut(const std::string& Class)
        {
            for (const char* Start : { "actor_", "script_vehicle", "info_vehicle_node", "node_", "weapon_", "misc_turret", "misc_mg42",
                                       "heli_height_lock" })
                if (Starts(Class, Start)) return true;
            return false;
        }
    };
}

std::string IRadiantTarget::MaterialName(const std::string& Material) const
{
    return MaterialGdt::PrefixedName(NamePrefix(), Material);
}

std::unique_ptr<IRadiantTarget> PlainTarget(const std::string& Stem)
{
    return std::make_unique<Plain>(Stem);
}

std::unique_ptr<IRadiantTarget> Bo3ModToolsTarget(const std::string& Game, const std::string& Stem)
{
    return std::make_unique<Bo3ModTools>(Game, Stem);
}

std::string Bo3ToolName(const std::string& Material)
{
    static const std::set<std::string> Bo3 = {
        "caulk", "clip", "clip_full", "clip_player", "clip_ai", "clip_missile", "clip_weapon", "clip_vehicle", "clip_physics",
        "clip_nosight", "clip_player_vehicle", "clip_player_playervehicle", "clip_playervehicle_only", "clip_stairs", "clip_utility", "clip_carver",
        "clip_navmesh_carver", "clip_navvolume_carver", "clip_ai_wallrun", "clip_ai_wallrun_nomesh", "clip_weap_glass",
        "clip_weapon_noimpact", "clip_missile_no_player", "clip_out_of_bounds", "clip_water_shallow", "clip_slick", "sky", "hint", "portal",
        "portal_nodraw", "nodraw_notsolid", "mantle_on", "mantle_over", "nosight_noclip", "trigger" };
    // the older games' names: a name, or a name's start (Whole false), and BO3's. CoD4: sky_<map>
    // the sky, clip_weap clip_weapon, clip_nosight_<surface> clip_nosight, clip_<surface> clip
    // (the surface being only the impact sound).
    struct Rule
    {
        const char* Source;
        bool Whole;
        const char* Bo3;
    };
    static const Rule Rules[] = {
        { "sky", false, "sky" },
        { "clip_weap", true, "clip_weapon" },
        { "clip_nosight_", false, "clip_nosight" },
        { "clip_foliage", true, "clip" }, { "clip_snow", true, "clip" }, { "clip_water", true, "clip" },
        { "clip_metal", true, "clip" }, { "clip_wood", true, "clip" }, { "clip_rock", true, "clip" },
        { "clip_dirt", true, "clip" }, { "clip_grass", true, "clip" }, { "clip_glass", true, "clip" },
        { "clip_concrete", true, "clip" },
    };
    if (Bo3.count(Material)) return Material;
    for (const Rule& R : Rules)
        if (R.Whole ? Material == R.Source : Starts(Material, R.Source)) return R.Bo3;
    return std::string();
}
}
