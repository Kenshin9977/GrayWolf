#pragma once

// What a Radiant .map is written for (MapRadiantWriter.h). The format is Black Ops III Radiant's
// whatever the source game; a target says what of the source goes in and by what names.
// PlainTarget keeps the map as the game has it (a .map beside the export, for Black Ops III's
// Radiant or an older game's); Bo3ModToolsTarget makes it a level the mod tools build in their
// tree: names of the map's own, the source's single-player logic left out, fitted to the tools'
// limits (Fits).

#include "MapLive.h"
#include "MapRadiantWriter.h"
#include "MapScene.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace Maps
{
    class IRadiantTarget
    {
    public:
        virtual ~IRadiantTarget() = default;
        // the map's name: the .map's, its folder's below model_export, its usermap's
        virtual const std::string& MapName() const = 0;
        // before every asset name the map defines ("" none)
        virtual const std::string& NamePrefix() const = 0;
        // an entity of the source as the .map gets it, rewritten in place; false: left out
        virtual bool Admit(Entity& E, const MapScene& Scene, RadiantStats& Stats) const = 0;
        // a model of the map's (one its export writes) by its GDT entry's name
        virtual std::string ModelName(const std::string& Model) const = 0;
        // the models' files: ".XMODEL_EXPORT", or what the mod tools' linker takes
        virtual const char* ModelExtension() const = 0;
        // how far from the origin, on any axis, geometry is kept (drawn triangles, placements, what
        // the outer sky box takes in); infinite for none
        virtual double WorldLimit() const = 0;
        // whether a placement of a model the export writes no file of (one of another zone) is
        // kept: a .map as the game has it, yes; a level to build, no (the compiler refuses it)
        virtual bool PlacesUnwrittenModels() const = 0;
        // the most a drawn triangle may span on an axis and go into the world's models; a larger
        // one is a patch of the world instead (infinite: every one to the models)
        virtual double ModelTriangleSpan() const = 0;
        // the smallest scale a placement keeps (-infinity: every one)
        virtual float MinModelScale() const = 0;
        // how far from a plane (units) and how far off its facing (the least cosine) a collision
        // triangle may be and still join a polygon of it (a prism: fewer, larger ones, fewer planes)
        struct Merge
        {
            double Distance, MinDot;
        };
        virtual Merge CollisionMerge() const = 0;
        // the compiler's plane slots (MapRadiantPlanes.h) the collision may take (the clip map's
        // brushes with the prisms): past it, the collision polygons are patches instead (0: always
        // patches; infinite: always prisms)
        virtual double CollisionPlaneBudget() const = 0;
        // the most surfaces (one per material) a model of the world's drawn triangles takes
        virtual size_t MaxModelSurfaces() const = 0;
        // the most vertices a model's surfaces of one material may hold together (a larger model
        // is several, placed together)
        virtual size_t MaxMaterialVertices() const = 0;
        // the least area (square units) a collision triangle written as a patch (a terrain grid's,
        // or one of its own) keeps; thinner than a tenth of a unit it goes too (0: every one)
        virtual double MinPatchTriangleArea() const = 0;
        // the most sides a brush of the clip map's may have and be written
        virtual size_t MaxBrushSides() const = 0;
        // the play area: the players' starts grown by PlayMargin, across and up (what the writer
        // keeps first when a limit makes it leave something out)
        struct Margin
        {
            double Across, Up;
        };
        virtual Margin PlayMargin() const = 0;
        // what the target's tools refuse outright, checked again on what was written (the rules
        // above keep within them): the most surfaces a model holds, vertices a model's surfaces of
        // one material hold together, characters an asset name has; entities a map holds, static
        // models (its misc_models), materials it names, drawn triangles of its world (its
        // patches'); plane slots, brushes, brush sides with their bevels (MapRadiantPlanes.h), the
        // patches' collision triangles and vertices. Zero: no such limit. The table:
        // graywolf-tools/docs/bo3-modtools-limits.md.
        struct Limits
        {
            size_t ModelSurfaces = 0, MaterialVertices = 0, NameLength = 0;
            size_t Entities = 0, StaticModels = 0, Materials = 0, WorldTriangles = 0;
            size_t PlaneSlots = 0, Brushes = 0, BrushSides = 0, CollisionTriangles = 0, CollisionVertices = 0, Assets = 0;
            // exploders (Radiant: "This map will fail to link" past 256); brush models (cod2map's
            // 1024 are the non-dynamic ones', counted as if every one were)
            size_t Exploders = 0, BrushModels = 0;
        };
        virtual Limits HardLimits() const = 0;
        // whether the writer fits the map under HardLimits (MapRadiantWriter.cpp: what a player misses
        // least left out first); false: only told (warnings)
        virtual bool Fits() const = 0;
        // the keys a model of the world's (a tile of it, its origin the world's) takes besides its
        // model and placement, as (key, value) pairs
        virtual std::vector<std::pair<std::string, std::string>> WorldModelKeys() const { return {}; }
        // a material of the map's by its GDT entry's name (MaterialGdt::PrefixedName)
        std::string MaterialName(const std::string& Material) const;
        // Whether the target's tools define the source's asset Name as an entry of Gdf ("xmodel.gdf",
        // "material.gdf") themselves, the same asset: a map of the game the tools are for (Black Ops
        // III's own in its mod tools). Such an asset is named as it is and neither written nor
        // defined again: the tools' own is exact (its techniques, tints and gloss ranges, its LODs
        // and collision), where a copy is an approximation and, under the same name, a duplicate.
        virtual bool Stock(const std::string& Name, const char* Gdf) const { return false; }
    };

    // Game: the source game's id (the export's folder); Stem: the map's file stem; Stock: the assets
    // the tree's own GDTs define, by lower-case name, with their GDF (ModToolsTree::DefinedTypes),
    // empty for a map of another game; Tree: the mod tools tree (its effects and colour gradings,
    // which an entity of the map may name: "" none checked)
    std::unique_ptr<IRadiantTarget> PlainTarget(const std::string& Stem);
    std::unique_ptr<IRadiantTarget> Bo3ModToolsTarget(const std::string& Game, const std::string& Stem,
                                                      std::unordered_map<std::string, std::string> Stock = {},
                                                      const std::string& Tree = std::string());
    // The Stock a Black Ops III map gets in the mod tools tree Tree (its stock GDTs' assets); empty
    // for another game's map
    std::unordered_map<std::string, std::string> Bo3StockAssets(const MapScene& Scene, const std::string& Tree);

    // A source game's tool material as Black Ops III's mod tools name it (the tools Radiant's
    // configs/navmesh.json and the shipped maps use): kept when the mod tools have it, else its
    // counterpart by the rules below; "" when none, the brush's content flags then choosing. A
    // game's own tool names go in that table.
    std::string Bo3ToolName(const std::string& Material);
}
