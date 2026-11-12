#pragma once

// Where a Radiant .map ends (MapRadiantWriter.cpp's outer sky box): the box of what a level holds
// that players can see, runaway geometry cut off. Every element of the level (a drawn or
// collision triangle, a placement, an entity) at its distance beyond where players start (the
// farthest of its points), sorted: kept out to the first gap where the next one is more than
// GapFactor times farther than the last (and than Floor). A level's scenery runs on without such
// a gap: vista terrain, mountains, the sky's dome reach 69 000 units beyond the starts on BO1's
// mp_nuked, 49 000 on BO2's mp_raid, 56 000 on Ghosts' mp_prisonbreak, none of them a step past
// 1.44 times the last. Runaway geometry stands alone far beyond: BO1 mp_firingrange's water, 12
// triangles out to 2.36 million units, the nearest of their corners 46 times farther than
// anything else (21 000). What lies outside the box is cut: a triangle across it clipped to it,
// one outside left out, a placement or an entity standing outside left out.

#include "MapLive.h"
#include "MapScene.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Maps
{
    struct SkyBound
    {
        // nothing within the box's reach left out: the scene as it is
        bool Cuts = false;
        double Lo[3] = { 0, 0, 0 }, Hi[3] = { 0, 0, 0 };   // the box, ClipMargin past what is kept when it cuts (empty: Lo > Hi)
        double Reach = 0;      // how far beyond the reference the elements kept reach
        double Next = 0;       // the nearest element past Reach (0: none)
        bool FromStarts = false;   // the reference: the players' starts (else the geometry's median point)
        bool Holds(const float* P) const;
        bool Holds(const double* P) const;
    };

    // never cut within GapFactor x Floor (65536 units) of the reference; past it, at a gap of
    // GapFactor (the next element GapFactor times farther than the last)
    constexpr double SkyBoundFloor = 16384.0, SkyBoundGapFactor = 4.0;
    // how far past what is kept the box goes (what it cuts is clipped there)
    constexpr double SkyBoundClipMargin = 512.0;

    // The bound of Scene's world (its meshes but the brush models', its collision triangles, its
    // placements) and of Ents' origins (worldspawn's aside), from the players' starts' box (Starts
    // false: none, the geometry's median point instead); the box within Limit of the origin on
    // every axis (the target's world, RadiantTarget.h WorldLimit)
    SkyBound UsefulBound(const MapScene& Scene, const std::vector<Entity>& Ents, bool Starts, const double (&StartLo)[3],
                         const double (&StartHi)[3], double Limit);

    // Scene's world cut to Bound: what CutToBound fills, and what it cut
    struct BoundedWorld
    {
        std::vector<Mesh> Objects;
        std::vector<float> CollisionVertices;
        std::vector<uint32_t> CollisionTriangles;
        std::vector<Instance> StaticInstances, DynamicInstances, SplinedInstances;
        size_t TrianglesCut = 0, TrianglesClipped = 0;     // drawn
        size_t CollisionCut = 0, CollisionClipped = 0;     // collision
        size_t PlacementsCut = 0;
    };
    void CutToBound(const MapScene& Scene, const SkyBound& Bound, BoundedWorld& Out);
}
