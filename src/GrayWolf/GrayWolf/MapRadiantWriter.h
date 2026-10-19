#pragma once

// A MapScene as a Radiant source ("iwmap 4": Black Ops III's mod tools, World at War's, Black
// Ops'), to port a level: <map>.map with worldspawn and the clip map's world brushes (a tool
// material from their content flags; a solid side on drawn world triangles their material and
// projection, MapRadiantTextures.h), every entity of the entity string (a brush entity with its
// brush model's brushes, textured the same way from its drawn triangles), the drawn triangles no
// brush side draws as patches (a 2x2 mesh each, nonColliding), a misc_model per placement (and
// per piece of world geometry a patch cannot carry); and <map>_radiant/ the models it names as
// XMODEL_EXPORT with a GDT of them.

#include "MapRadiantTextures.h"
#include "MapScene.h"

#include <array>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace Maps
{
    // A side of a brush that bounds something: its polygon (counter-clockwise seen from outside)
    struct BrushFace
    {
        size_t Side = 0;
        std::vector<std::array<double, 3>> Points;
    };
    // The faces of a convex brush (its sides clipped by one another); a side bounding nothing (a
    // bevel, a duplicate) has none; nor has a sliver, or a face lying on another side's plane, when
    // the brush stays within Within of its plane without it. Fewer than four faces: not a closed
    // volume.
    std::vector<BrushFace> BrushFaces(const Brush& B, double Within = 0.01);
    // A collision triangle as the brush this export writes for one of the world's: a prism Thick
    // units deep behind its face (P facing out along (P1 - P0) x (P2 - P0)); false for one without
    // area (a game's reader making a brush model's triangles into brushes uses it)
    bool TrianglePrism(const std::array<double, 3> P[3], double Thick, Brush& B);

    struct RadiantStats
    {
        size_t Brushes = 0, BrushesDropped = 0, BrushesUnsound = 0, Faces = 0, Entities = 0, BrushEntities = 0, BrushEntitiesEmpty = 0;
        size_t Placements = 0, WorldModels = 0, ModelFiles = 0, ModelsElsewhere = 0, Triangles = 0, TrianglesDropped = 0, TriggerBrushes = 0;
        size_t TrianglesDuplicate = 0;
        // the sides given the drawn world's materials: the clip map's world brushes', the collision
        // triangles' prisms', the brush entities'
        RadiantTextureStats Textures, PrismTextures, EntityTextures;
        // the drawn triangles no brush side draws: patches (of them vertex-coloured), left out
        // (no area), left to the world's misc_models (no material, a coordinate not finite)
        size_t Patches = 0, PatchesColoured = 0, TrianglesDegenerate = 0, TrianglesInModels = 0;
    };
    // With Gdt (the material_gdt setting), the model files name their materials by their GDT asset
    // names (MaterialGdt::AssetName) and the GDT is left to the caller: its model entries and the
    // scene materials the model files name are returned there.
    struct RadiantGdt
    {
        std::filesystem::path Root;            // <map>_radiant
        std::string Map;                       // the map's folder name below model_export
        std::vector<std::string> ModelEntries;  // the xmodel entries (LF)
        std::set<std::string> Materials;       // scene material names
    };
    bool WriteRadiant(const MapScene& Scene, const std::string& MapPath, RadiantStats& Stats, std::string& Error,
                      RadiantGdt* ForGdt = nullptr);
}
