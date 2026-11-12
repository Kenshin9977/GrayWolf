#pragma once

// What a Radiant .map is written for (MapRadiantWriter.h). The format is Black Ops III Radiant's
// whatever the source game; a target says what of the source goes in and by what names.
// PlainTarget keeps the map as the game has it (a .map beside the export); Bo3ModToolsTarget makes
// it a level the mod tools build in their tree: names of the map's own, the source's
// single-player logic left out.

#include "MapLive.h"
#include "MapRadiantWriter.h"
#include "MapScene.h"

#include <memory>
#include <string>

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
        // the brush sides the collision may take (the clip map's brushes' with the prisms'): past
        // it, the collision polygons are patches instead (infinite: always prisms)
        virtual double CollisionPlaneBudget() const = 0;
        // the brush sides (the world's and the brush models') a map may hold: past it, the brush
        // models of the most sides keep their drawn triangles only
        virtual double BrushSideBudget() const = 0;
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
        // the collision triangles a map may keep whole; past it, only those in the play area (the
        // players' starts grown by PlayMargin, across and up)
        virtual double CollisionTriangleBudget() const = 0;
        struct Margin
        {
            double Across, Up;
        };
        virtual Margin PlayMargin() const = 0;
        // what the target's tools refuse outright, checked again on what was written (the rules
        // above keep within them): the most surfaces a model holds, vertices a model's surfaces of
        // one material hold together, characters an asset name has, brush sides a map holds (the
        // largest seen to build). Zero: no such limit.
        struct Limits
        {
            size_t ModelSurfaces = 0, MaterialVertices = 0, NameLength = 0, BrushSides = 0;
        };
        virtual Limits HardLimits() const = 0;
        // the keys a model of the world's (a tile of it, its origin the world's) takes besides its
        // model and placement, as (key, value) pairs
        virtual std::vector<std::pair<std::string, std::string>> WorldModelKeys() const { return {}; }
        // a material of the map's by its GDT entry's name (MaterialGdt::PrefixedName)
        std::string MaterialName(const std::string& Material) const;
    };

    // Game: the source game's id (the export's folder); Stem: the map's file stem
    std::unique_ptr<IRadiantTarget> PlainTarget(const std::string& Stem);
    std::unique_ptr<IRadiantTarget> Bo3ModToolsTarget(const std::string& Game, const std::string& Stem);

    // A source game's tool material as Black Ops III's mod tools name it (the tools Radiant's
    // configs/navmesh.json and the shipped maps use): kept when the mod tools have it, else its
    // counterpart by the rules below; "" when none, the brush's content flags then choosing. A
    // game's own tool names go in that table.
    std::string Bo3ToolName(const std::string& Material);
}
