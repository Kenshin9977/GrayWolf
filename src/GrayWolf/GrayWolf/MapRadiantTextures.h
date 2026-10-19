#pragma once

// The drawn world's materials on the Radiant export's brushes (MapRadiantWriter.h): a brush side
// lying on drawn triangles (the world's for a world brush, its brush model's for a brush
// entity's) takes the material covering most of its face, its texture projection fitted to their
// UVs; the triangles its face covers whole are not written again. Projection of "<material> W H Sx Sy R 0": Quake 3's base axes for the plane,
// turned R degrees, u = (p . s - Sx) / W, v = (p . t - Sy) / H (measured on World at War's
// mp_castle, mod tools source against the compiled zone).

#include "MapScene.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Maps
{
    struct RadiantTextureStats
    {
        size_t Sides = 0;            // sides of solid brushes looked at
        size_t SidesTextured = 0;    // given a drawn material
        size_t SidesSplit = 0;       // of those, a second material over 5% of the drawn area on the face
        size_t SidesNoFit = 0;       // drawn triangles on the face, no projection fitted (degenerate UVs)
        size_t SidesMixedMaps = 0;   // the material's triangles on the face mapped two ways (over 5% of its area not the fitted way)
        size_t SidesUvMissed = 0;    // fitted, but missing the drawn UVs by more than MaxUvError: left a tool
        size_t SidesUndrawn = 0;     // over 5% of the face neither drawn the fitted way nor inside another brush: left a tool
        size_t BrushesTextured = 0;  // brushes with at least one textured side
        // a fitted side's largest UV error at its drawn vertices, as written (one repeat = 1)
        size_t UvExact = 0, UvBelow1e3 = 0, UvBelow1e2 = 0, UvBelow1e1 = 0, UvAbove = 0;
        double UvErrorSum = 0;
        size_t TrianglesDrawn = 0, TrianglesRemoved = 0;
        size_t TrianglesColoured = 0;   // drawn with vertex colours (a blend, a tint) no brush side carries: left out of the matching
        // the matched triangles' farthest vertex from the side's plane: 0, below 1e-3, 1e-2, the tolerance (what the tolerance rests on)
        size_t Distance[4] = { 0, 0, 0, 0 };
        // the share of a textured side's face its drawn triangles cover: below 10%, 50%, 90%, the rest
        size_t Coverage[4] = { 0, 0, 0, 0 };
    };

    // a tool material's name (lower case): caulk, clip, trigger, sky, ...
    bool RadiantTool(const std::string& Lower);
    // Quake 3's texture axes (s, t) of a plane of normal N, as Radiant projects onto it
    void RadiantBaseAxes(const std::array<double, 3>& N, std::array<double, 3>& S, std::array<double, 3>& T);

    // A side's material and texture projection
    struct SideTexture
    {
        std::string Material;   // the scene material's name
        double Width = 64, Height = 64, ShiftX = 0, ShiftY = 0, Rotation = 0;
        double UvError = 0;     // the largest UV error at the drawn vertices
    };

    class DrawnWorld
    {
    public:
        using V3 = std::array<double, 3>;

        // Scene.Objects' drawn triangles, tool materials and vertex-coloured triangles left out
        // (counted in Stats)
        DrawnWorld(const MapScene& Scene, RadiantTextureStats& Stats);

        // whose a mesh is: 0 the world's, N brush model N's ("brushmodel_<N>")
        static uint32_t Owner(const std::string& MeshName);

        // Face: a side's polygon (counter-clockwise seen from outside) on N . p = D (N unit, out
        // of the brush); Written: the normal Radiant reads back (it picks the base axes). True
        // when drawn triangles cover enough of the face: Out the material covering most of it,
        // its projection fitted to their UVs; the face's whole triangles of it marked removed.
        // Only the triangles of Owner's meshes are looked at. Hidden (when given) tells a point
        // inside another brush: the part of a face it hides needs no drawn triangle.
        using HiddenTest = std::function<bool(const V3&)>;
        bool Texture(const std::vector<V3>& Face, const V3& N, double D, const V3& Written, uint32_t Owner, const HiddenTest* Hidden,
                     SideTexture& Out, RadiantTextureStats& Stats);

        // whether triangle K (indices 3K..3K+2) of Scene.Objects[Object].Surfaces[Surf] is drawn by a brush
        bool Removed(size_t Object, size_t Surf, size_t K) const;
        size_t Count() const { return Tris.size(); }

    private:
        struct Tri
        {
            V3 P[3];
            double Uv[3][2];
            V3 N;   // unit, facing as the vertex normals do
            double Area;
            uint32_t Material, Object, Surf, K, Owner;
        };
        std::vector<Tri> Tris;
        std::vector<std::string> Names;
        std::vector<std::vector<std::vector<char>>> Gone;   // per object, surface, triangle
        // a grid of cells, each holding the triangles facing one way (the axis their normal is
        // closest to) whose bounds reach it: (cell, triangle) sorted, a cell's range by its key
        std::vector<std::pair<uint64_t, uint32_t>> Grid;
        std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> Cells;
        std::vector<uint32_t> Stamp;
        uint32_t Query = 0;
    };
}
