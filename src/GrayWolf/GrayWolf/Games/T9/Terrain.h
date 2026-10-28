#pragma once

#include "ByteView.h"

// Black Ops Cold War's terrain (terraingfx) as world geometry and collision triangles. A surface
// is a heightmap grid drawn where its visibility bitmap says, coloured by its colour map; its
// meshes (the levels of nodes, the finest streamed) are the same grid, so the grid is built from
// the heightmap. Layout in Terrain.cpp, measured on the zones' bytes.

#include "MapScene.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ColdWar
{
    struct TerrainSource
    {
        const ByteView* Image = nullptr;   // the zone
        std::function<int64_t(uint64_t)> Off;          // a pointer field's zone position, -1 when none
        // an image's largest mip, decoded: its raw pixels (no file header), size and DXGI format
        std::function<bool(int64_t Image, std::vector<uint8_t>& Pixels, uint32_t& Width, uint32_t& Height)> Pixels;
        std::function<std::string(int64_t Image)> ImageName;
    };

    // The grid's resolution: every heightmap sample unless a surface's quads exceed these (then
    // every 2nd, 4th... sample: a 1024 grid every 2nd, a 2048 grid every 4th, ~520 000 triangles;
    // a full 1024 grid's 2 million made zm_silver's Radiant export take 18 minutes)
    struct TerrainOptions
    {
        uint32_t MaxDrawnQuads = 1u << 18;
        uint32_t MaxCollisionQuads = 1u << 18;
    };

    // Adds each surface of the terraingfx asset at Terrain to Scene.Objects ("terrain_<n>", its
    // material "terrain_<n>" drawing the colour map) and its triangles to Scene.Collision;
    // Counters says what was read (terrain_*). False when there is no surface.
    bool ReadTerrain(Maps::MapScene& Scene, const TerrainSource& Src, int64_t Terrain, const TerrainOptions& Options,
                     std::map<std::string, size_t>& Counters);
}
