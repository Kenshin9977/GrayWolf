#pragma once

// The planes Black Ops III's cod2map makes of a .map's brushes (its FindFloatPlane, AddBrushBevels and
// edge bevels, read from cod2map64.exe: graywolf-tools/docs/bo3-modtools-limits.md), counted for its
// MAX_MAP_PLANES_COMPILE (0xa0000 slots, two per plane) and MAX_MAP_PLANES (0x20000). Patches,
// terrain and models make none; the BSP's block splits (a few hundred) are not counted. Measured
// against cod2map itself (BO2's zm_transit, rotated boxes, prisms): 0.1 to 1.6% high.

#include "MapScene.h"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace Maps
{
    struct CompilerPlane
    {
        float N[3] = { 0, 0, 1 };
        float D = 0;
    };

    // a brush's planes as cod2map makes them: its sides', then the bevels it adds
    struct CompilerBrush
    {
        std::vector<CompilerPlane> Sides, Bevels;
        size_t BoxBevels = 0, EdgeBevels = 0;
    };
    // from each side's three points as the .map gives them
    CompilerBrush CompilerPlanes(const std::vector<std::array<std::array<float, 3>, 3>>& Points);
    // from a brush's planes (its sides' normals and distances): about what its written points give
    CompilerBrush CompilerPlanes(const Brush& B);

    // cod2map's table of planes: Find gives a plane's slot (even: as made, odd: its flip), a new one
    // taking two; equal within 1e-7 on each normal component and 0.001 on the distance
    class PlaneTable
    {
    public:
        size_t Find(const CompilerPlane& P);
        size_t Unique() const { return Planes.size() / 2; }
        size_t Slots() const { return Planes.size(); }

    private:
        std::vector<CompilerPlane> Planes;
        std::unordered_map<int64_t, std::vector<size_t>> Buckets;   // floor(|d| * 8) -> slots
    };

    struct PlaneCount
    {
        size_t Brushes = 0, Sides = 0, BoxBevels = 0, EdgeBevels = 0;
        size_t Slots = 0;            // MAX_MAP_PLANES_COMPILE's count (0xa0000)
        size_t SidePlanes = 0;       // the slots the sides use: MAX_MAP_PLANES's least (0x20000)
        size_t CollisionSides = 0;   // sides and bevels the brushes keep (MAX_MAP_BRUSHSIDES, 0xa0000)
    };
    // The planes cod2map makes of a .map's text (its brushes; patches skipped)
    PlaneCount CountPlanes(const std::string& MapText);
    // The planes of brushes (CompilerPlanes) made together, then Again (planes found once more: an
    // entity's brushes moved to its origin, cod2map's 0x1400a45c0)
    PlaneCount CountPlanes(const std::vector<const CompilerBrush*>& Brushes, const std::vector<CompilerPlane>& Again = {});
}
