#pragma once

// The one map writer: a MapScene as a .glb for Blender (GltfSceneWriter). World meshes, one glTF
// mesh per surface; each model converted once and placed by a node per instance (instancing);
// lights (KHR_lights_punctual); images embedded (PNG bytes) or referred to by a path relative to
// the .glb. Beside it: <map>.materials.json when the images are files (the game's material
// adapter, MaterialDescription.h), <map>.map_ents when the scene has an entity string, and
// <map>_import.py, which imports the .glb in Blender and sets the sky texture when there is one
// (written as <map>_sky.png when the scene holds its bytes).

#include "MapScene.h"

#include <string>
#include <vector>

namespace Materials { class IAdapter; }

namespace Maps
{
    struct WriteStats
    {
        size_t WorldMeshes = 0, WorldTriangles = 0, WorldSurfacesTextured = 0;
        size_t Models = 0, ModelMeshes = 0;           // models placed (a glTF mesh each), their surfaces
        size_t Instances = 0, InstanceNodes = 0;      // placements, their nodes (one per placement)
        size_t InstancesWithoutModel = 0;             // placements of a model the scene lacks
        size_t SkyMeshes = 0, Lights = 0;
        size_t Materials = 0, Images = 0;             // glTF materials and images
        size_t MissingTextures = 0;                   // colour maps without pixels
        size_t AlphaMasked = 0, AlphaBlended = 0;     // glTF materials cut out, blended
        size_t LayerWeightSurfaces = 0;               // surfaces whose vertex colours are layer weights: _LAYER_WEIGHTS, COLOR_0 white
        // Auto alpha whose colour map is not written yet (a dependency): written opaque, for
        // ResolveAlpha once the run has written it
        struct LaterAlpha
        {
            std::string Material, ColorMap;
            float Cutoff = 0.5f;
        };
        std::vector<LaterAlpha> AlphaLater;
    };

    // Adapter: the game's material adapter for the sidecar and the alpha modes (nullptr: the
    // default one). A material's alpha mode goes in the .glb as its description says (cutout:
    // MASK at its cutoff; Auto: MASK when its colour map's alpha masks something).
    bool WriteGlb(const MapScene& Scene, const std::string& GlbPath, const Materials::IAdapter* Adapter,
                  WriteStats& Stats, std::string& Error);

    // Judges Later's colour maps and rewrites the .glb's JSON for those that mask: the number cut
    // out (0 when none, the file untouched).
    size_t ResolveAlpha(const std::string& GlbPath, const std::vector<WriteStats::LaterAlpha>& Later);

    // The counts as report lines ("world_meshes=12\n...").
    std::string Describe(const WriteStats& S);
}
