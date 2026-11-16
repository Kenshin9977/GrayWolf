#pragma once

// A MapScene as a Radiant source ("iwmap 4": Black Ops III's mod tools, World at War's, Black
// Ops'), to port a level: <map>.map with worldspawn and the clip map's world brushes (a tool
// material from their content flags; a solid side on drawn world triangles their material and
// projection, MapRadiantTextures.h), every entity of the entity string (a brush entity with its
// brush model's brushes, textured the same way from its drawn triangles), the drawn triangles no
// brush side draws as misc_models of the world's pieces (or patches, a 2x2 mesh each,
// nonColliding), a misc_model per placement; and <map>_radiant/ the models it names as
// XMODEL_EXPORT with a GDT of them.

#include "MapRadiantLayers.h"
#include "MapRadiantTextures.h"
#include "MapScene.h"
#include "MapSceneWriter.h"

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
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
        // the collision triangles' terrain patches (grids of them) and brushes (those left, merged plane by plane)
        size_t TrianglesDuplicate = 0, TerrainPatches = 0, TriangleBrushes = 0;
        // the portal brushes left out (the source's visibility cells; Black Ops III's compiler
        // ignores them, warning) and the outer sky box's brushes (what seals the level for it)
        size_t PortalBrushes = 0, SkyboxBrushes = 0;
        // in a mod tools tree, the source's single-player logic left out (AI, vehicles, nodes,
        // weapons) with the misc_model entities its placements stand for, its script_origins
        // written as script_structs, the models entities or placements named that no file of the
        // export holds
        size_t EntitiesLeftOut = 0, OriginsAsStructs = 0, ModelsLeftOut = 0;
        // the drawn triangles and placements beyond the target's world (RadiantTarget.h), left out
        size_t BeyondWorld = 0;
        // drawn triangles another of the same sort key covers exactly (CoveredTriangles), left out
        size_t TrianglesCovered = 0;
        // the level cut to its sky box (MapRadiantBound.h): the drawn and collision triangles
        // outside it left out, those across it clipped, the placements and entities outside left
        // out; how far beyond the players' starts the box reaches, its largest side (units)
        size_t BeyondSkyBox = 0, ClippedToSkyBox = 0, BeyondSkyBoxPlacements = 0, BeyondSkyBoxEntities = 0, SkyBoxReach = 0, SkyBoxSize = 0;
        // the drawn triangles too large for the world's models (RadiantTarget.h), patches instead
        size_t OversizePatches = 0;
        // the placements scaled below the target's least (RadiantTarget.h), left out
        size_t ScaledToNothing = 0;
        // the collision triangles written as patches of one triangle, past the target's budget of
        // planes (RadiantTarget.h)
        size_t CollisionPatches = 0;
        // the clip map's brushes of more sides than the target takes, and the collision triangles
        // outside the play area a crop left out (RadiantTarget.h)
        size_t BrushesManySided = 0, CollisionOutsidePlay = 0;
        // the brush models' brushes left out past the target's budget of brush sides
        size_t BrushesOverBudget = 0;
        // the target's hard limits passed (RadiantTarget.h HardLimits), each a line of <map>.limits.txt
        size_t LimitsOver = 0;
        // with patches asked (the map_surfaces setting), the drawn triangles of a world too large for a
        // patch each (MostPatches): its models instead
        size_t PatchesRefused = 0;
        // the placements map_props left out: of foliage, of small props
        size_t FoliageLeftOut = 0, SmallLeftOut = 0;
        // what Black Ops III's cod2map makes of the brushes written (MapRadiantPlanes.h): plane slots,
        // those the sides use, the bevels it adds, the brush sides it keeps; the slots the prisms
        // would have taken (when weighed); the patches' collision triangles and vertices
        size_t PlaneSlots = 0, PlaneSidePlanes = 0, BoxBevels = 0, EdgeBevels = 0, CollisionBrushSides = 0, PrismPlaneSlots = 0;
        size_t PatchCollisionTriangles = 0, PatchCollisionVertices = 0;
        // what fitting the map to Black Ops III's tools did (graywolf-tools/docs/bo3-modtools-limits.md):
        // world brushes written as collision patches (planes, brushes), brushes made clip only (nodes)
        // and the sides left to split the BSP, collision triangles left out (farthest from play),
        // placements left out past the static models' room, exploders and brush entities let go
        size_t BrushesAsPatches = 0, BrushesFlat = 0, StructuralSides = 0, CollisionFitted = 0, PlacementsFitted = 0;
        size_t ExplodersDropped = 0, BrushEntitiesDropped = 0, PlaneSlotsPlanned = 0;
        // brush models whose drawn triangles were in their own space, written where their entity stands
        size_t BrushModelsPlaced = 0;
        // what the caller should tell (written, but past what the target's tools take): the limits
        // passed, a mode turned down
        std::vector<std::string> Warnings;
        // the sides given the drawn world's materials: the clip map's world brushes', the collision
        // triangles' prisms', the brush entities'
        RadiantTextureStats Textures, PrismTextures, EntityTextures;
        // the drawn triangles no brush side draws: patches (of them vertex-coloured), left out
        // (no area), left to the world's misc_models (no material, a coordinate not finite)
        size_t Patches = 0, PatchesColoured = 0, TrianglesDegenerate = 0, TrianglesInModels = 0;
        // the world's vertices whose colour holds layer weights (MapScene's VertexColors), not white:
        // written white (patches, models), the weights not carried over
        size_t VertexWeightsWhite = 0;
        // the terrain triangles drawn by their layer's own material (MapScene's TexelLayers), not the
        // terrain's baked colour map
        size_t TexelLayerTriangles = 0;
        // the later layers of the layered materials (MapRadiantLayers.h): those the materials have
        // (material, layer), carried as overlays, left out (no colour image; no triangle drawn, its
        // weight 0 at every corner; without the material GDT, which defines their materials); the
        // overlays' materials, the triangles drawn as overlay patches or in the world's models (by
        // layer, index K - 2), and the overlay triangles left out for a weight of 0 at their corners
        size_t Layers = 0, LayersCarried = 0, LayersNoColour = 0, LayersUndrawn = 0, LayersNoGdt = 0, OverlayMaterials = 0;
        size_t OverlayPatches[3] = { 0, 0, 0 }, OverlayInModels[3] = { 0, 0, 0 }, OverlayUnweighted = 0;
        // the target's tools' own assets the map names as they are (IRadiantTarget::Stock): placements
        // and entities of such a model (no file written), materials (no GDT entry)
        size_t StockModels = 0, StockMaterials = 0;
        // in a mod tools tree, what an entity names that the tree lacks: an effect (share/raw/fx/<fxdef>.efx:
        // the fx entity left out), a colour grading (share/raw/grades/<name>.lvi: the pbg_box's use of it off)
        size_t EffectsMissing = 0, GradesMissing = 0;
    };
    // With Gdt (the material_gdt setting), the model files name their materials by their GDT asset
    // names (MaterialGdt::AssetName) and the GDT is left to the caller: its model entries and the
    // scene materials the model files name are returned there.
    struct RadiantGdt
    {
        std::filesystem::path Root;            // <map>_radiant, or the mod tools tree
        std::string Map;                       // the map's folder below model_export ("<map>", in a tree "graywolf/<map>")
        std::filesystem::path GdtFile;         // where the GDT goes
        bool InTree = false;                   // written into a mod tools tree (ModToolsTree.h)
        std::string Prefix;                    // before every asset name: in a tree "<map>_" (maps and BO3's own share its GDT names), else ""
        std::vector<std::string> ModelEntries;  // the xmodel entries (LF)
        std::set<std::string> Materials;       // scene material names, and the overlays' (below)
        std::map<std::string, BlendLayer> Overlays;   // the blend layers' overlay materials the files name
        // the materials the target's tools define themselves (IRadiantTarget::Stock), named as they
        // are and left out of Materials: no entry of the GDT
        std::shared_ptr<const std::unordered_set<std::string>> Stock;
        std::set<std::string> StockNamed;     // those the files name
    };
    // With Tree (a Black Ops III Mod Tools root, the mod_tools setting), the files go into it rather
    // than beside MapPath: map_source/graywolf/<map>.map, model_export/graywolf/<map>/,
    // source_data/graywolf_<map>.gdt, the .map and GDT never overwritten (ModToolsTree::WriteKept).
    // Without Patches (the map_surfaces setting), the world's drawn triangles no brush side draws go
    // to its models (tiles of 2048 units, one surface per material) rather than a patch each, which
    // Radiant draws one by one; a brush entity's stay patches (they move with it).
    // Game: the source game's id, which a tree's map name takes (RadiantTarget.h). Progress: its
    // stages as it goes (MapSceneWriter.h); false from it stops the writing (Error "cancelled").
    // Props: the placements kept (the map_props setting): "all"; "no_foliage" (LeavesOut); "large"
    // (no foliage, no model under SmallProp units on every side, scaled)
    bool WriteRadiant(const MapScene& Scene, const std::string& MapPath, RadiantStats& Stats, std::string& Error,
                      RadiantGdt* ForGdt = nullptr, const std::string& Tree = std::string(), bool Patches = false,
                      const std::string& Game = std::string(), const WriteProgress& Progress = {}, const std::string& Props = "all",
                      const std::string& Fit = "fit");
    // Whether a model's name says foliage: one of its words (between non-letters) grass, foliage,
    // weed(s), bush(es), shrub, fern, ivy, vine(s), hedge, plant(s), flower(s), leaf, leaves, reed(s), moss
    bool FoliageName(const std::string& Model);
    constexpr double SmallProp = 24.0;
}
