#pragma once

// A map's pieces as the live readers give them, in MapScene terms: a material (XMaterial_t) and a
// model (the live model reader's mesh). What every game read through its live readers shares.

#include "MapScene.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct XMaterial_t;
namespace ExportWriters { struct LiveModel; }

namespace Maps
{
    // Adds M once (by name) and returns the name surfaces use for it.
    std::string AddMaterial(MapScene& Scene, const XMaterial_t& M);
    // The model's loaded LOD as Scene.Models[Name], its materials added; false without a triangle.
    // Drawn (when set) leaves out the surfaces of a material the game does not draw as geometry
    // (a shadow caster), counted in Hidden.
    bool AddModel(MapScene& Scene, const std::string& Name, const ExportWriters::LiveModel& M,
                  const std::function<bool(const XMaterial_t&)>& Drawn = nullptr, size_t* Hidden = nullptr);
    // The terrain alone (map_content terrain): the world meshes named "terrain*" and their materials
    // and images; models, instances, lights, entities and collision dropped
    void KeepTerrain(MapScene& Scene, std::map<std::string, uint64_t>& Models);
    // GH_MAPDEBUG=<file>: where a map reader writes what it read (its fields, raw), else nullptr
    FILE* DebugFile();
    // A placement's rotation from a quaternion (x, y, z, w) into Instance::Axis.
    void AxisFromQuat(const float Q[4], float Axis[9]);
    // A static model's placement as MW (2019) to Black Ops 7 pack it (24 bytes at Pos): its origin
    // in 1/4096 units (3 i32), its rotation a quaternion of u16 mapped to [-1, 1] (at 12), its scale
    // at 20 (a float, or a half when HalfScale). Name and Model are the caller's.
    Instance PackedPlacement(const std::vector<uint8_t>& Image, int64_t Pos, bool HalfScale);
    // A name kept inline at Pos (a char array, NUL-terminated within Max): empty when it is not
    // printable text.
    std::string InlineName(const std::vector<uint8_t>& Image, int64_t Pos, size_t Max);

    // The entity string ({ "key" "value" ... } blocks, Quake 3 to IW5) as key/value maps, in
    // order; false when it does not read as one. A key may be a bare token (MW3 numbers them).
    using Entity = std::map<std::string, std::string>;
    bool ParseEntities(const std::string& Text, std::vector<Entity>& Out);
    // Count floats of an entity's value ("origin" "1 2 3"); false when missing or short.
    bool EntityFloats(const Entity& E, const char* Key, float* Out, int Count);
    // Quake 3's AnglesToAxis ("angles": pitch, yaw, roll in degrees) as Instance::Axis rows.
    void AnglesToAxis(const float Angles[3], float Axis[9]);
    // AddWorld's brush models 1..n ("brushmodel_<n>") are kept in their own space (CoD4, World at
    // War: centred on their entity's origin). PlaceBrushModel moves model N by a placement (false:
    // no such mesh); PlaceBrushModels moves each to the entity whose "model" is "*<n>" (its origin
    // and angles) and returns the ones with a mesh and no entity (left where they are).
    bool PlaceBrushModel(MapScene& Scene, uint32_t N, const float Origin[3], const float Axis[9]);
    std::vector<uint32_t> PlaceBrushModels(MapScene& Scene, const std::vector<Entity>& Ents);

    // World geometry out of a game's shared vertex and index buffers: triangles grouped by
    // material, each buffer vertex copied once per group (Fetch reads it; false: outside).
    class WorldBuilder
    {
    public:
        using Fetch = std::function<bool(uint32_t Index, Vertex& Out)>;
        // a vertex's UV sets after its Uv: up to Max pairs written to Out, how many returned
        using FetchUvs = std::function<uint32_t(uint32_t Index, float* Out, uint32_t Max)>;
        static constexpr uint32_t MaxMoreUvs = 7;
        WorldBuilder(std::string Name, Fetch Read, FetchUvs MoreUvs = {})
            : Out{ std::move(Name), {} }, Read(std::move(Read)), MoreUvs(std::move(MoreUvs)) {}
        // false when a vertex could not be read (the triangle is left out)
        bool Triangle(const std::string& Material, uint32_t A, uint32_t B, uint32_t C);
        size_t Triangles() const { return Count; }
        Mesh Take() { return std::move(Out); }

    private:
        Mesh Out;
        Fetch Read;
        FetchUvs MoreUvs;
        std::unordered_map<std::string, size_t> Group;                  // material -> surface
        std::vector<std::unordered_map<uint32_t, uint32_t>> Remap;      // per surface
        size_t Count = 0;
    };

    // A world's brush models as meshes: "world" (model 0, the level) and "brushmodel_<n>" (its
    // movers), each from the surfaces Range gives it ([first, end), clipped to SurfaceCount), Surface
    // adding one surface's triangles to the model's builder; a model without a triangle left out
    void BuildWorld(MapScene& Scene, uint32_t BrushCount, uint32_t SurfaceCount,
                    const std::function<std::pair<uint32_t, uint32_t>(uint32_t Brush)>& Range, const WorldBuilder::Fetch& Read,
                    const std::function<void(uint32_t Surface, WorldBuilder& Mesh)>& Surface, const WorldBuilder::FetchUvs& MoreUvs = {});

    // A GfxWorld's surfaces as every game lays them out (C2M's readers, BO4 and Ghosts): brush
    // models own ranges of surfaces (model 0 is the level), a surface a range of triangles in the
    // world's one index buffer, its indices counted from its first vertex. Field places per game.
    struct WorldLayout
    {
        const std::vector<uint8_t>* Image = nullptr;   // the served zone
        int64_t Surfaces = -1, Brushes = -1, Indices = -1;   // where the arrays are (-1: not in the zone)
        uint32_t SurfaceCount = 0, BrushCount = 0, IndexCount = 0;
        int64_t SurfaceSize = 0, FirstVertex = 0, TriCount = 0, BaseIndex = 0, Material = 0;   // u32, u16, u32, pointer
        uint32_t FirstVertexUnit = 1;   // what the first vertex counts: vertices (1), or bytes of a stream
        // non-zero: the first vertex is a byte offset into a block of vertices this many bytes each,
        // not always a whole vertex (Black Ops II); a vertex is then read by its byte offset
        uint32_t VertexBytes = 0;
        int PointerSize = 8;            // the material pointer's width (4 on a 32-bit game)
        int64_t BrushSize = 0, FirstSurface = 0, SurfaceCountAt = 0;                           // u32, i32
        int BrushFieldBytes = 4;   // the two brush fields' width: 4, or 2 (IW4/IW5: u16, 0xffff none)
        // surfaces left out (the sky's, which the import script draws from its texture instead)
        const std::unordered_set<uint32_t>* Skip = nullptr;
        // added to a surface's first vertex, by the surface's position (a level whose vertices are in
        // several transient zones: where that surface's zone starts in the vertex space Read reads)
        std::function<uint32_t(int64_t Surface)> VertexBase;
        // a surface's vertex's UV sets after its Uv (by the surface's position and the vertex's
        // number in it: up to Max pairs written to Out, how many returned); unset: one set
        std::function<uint32_t(int64_t Surface, uint32_t Local, float* Out, uint32_t Max)> MoreUvs;
    };
    // Adds one world mesh per brush model with triangles; the triangles left out (a vertex not read).
    size_t AddWorld(MapScene& Scene, const WorldLayout& L, const WorldBuilder::Fetch& Read,
                    const std::function<std::string(uint64_t Material)>& MaterialOf);
}
