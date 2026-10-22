#pragma once

// What the Treyarch map readers (BO3 T7, BO4 T8) share and Infinity Ward's do not: the world's
// vertices in two streams, the 0x60 GfxSurface with its material at 0x48, dynamic entities that
// open with a quaternion, an origin, the model and the destroyed model, and primary lights typed
// 1 sun, 2 spot, 4 omni with their origin at 72. The rest of each record differs per game.

#include "DiskGame.h"
#include "MapLive.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Treyarch
{
    constexpr int64_t SurfaceSize = 0x60, SurfaceMaterial = 0x48;

    // A world vertex: its position (12 bytes) in one stream, its colour, uv and 10:10:10 normal
    // (20 bytes) in the other.
    Maps::WorldBuilder::Fetch WorldVertices(const std::vector<uint8_t>& Image, int64_t Positions, int64_t Data, uint32_t Count);

    // Dynamic entities (Stride bytes each) as placements; ModelAt names a model's address.
    void DynamicEntities(const std::vector<uint8_t>& Image, int64_t List, uint32_t Count, int64_t Stride, const std::string& Prefix,
                         const std::function<std::string(uint64_t)>& ModelAt, Maps::MapScene& Scene,
                         std::map<std::string, uint64_t>& Models);

    // Where a game keeps the rest of a primary light (its origin at 72 in BO4, 104 in Cold War)
    struct LightFields
    {
        int64_t Size = 0, Color = 0, Dir = 0, CosHalfFov = 0, Radius = 0, Origin = 72;
    };
    void Lights(const std::vector<uint8_t>& Image, int64_t List, uint32_t Count, const LightFields& F, Maps::MapScene& Scene);

    // The entities (BO4's entity_list, Cold War's entitylist) as the map_ents text: { "key"
    // "value" ... } per entity. Count entities of 0x30 bytes from Array (the key-value count at 0,
    // the pairs at 8); a pair (0x20): the key string at 0, the value at 8, its type at 0x18 (2
    // string, 3 three floats, 4 a 60-bit name hash, 5 float, 6 u32). C2M BlackOps4.cs. Off and
    // String follow a pointer; HashName names a hash ("" unknown: written #<hex>). Model, when
    // set, gives an entity without a "model" key one from its number and its model index (i32
    // at 0x10): "*N" for a brush model, "?N" for a trigger volume, "" none.
    struct EntityCounts
    {
        size_t Keys = 0, KeysNamed = 0, Hashes = 0, HashesNamed = 0;
    };
    std::string EntityText(const std::vector<uint8_t>& Image, int64_t Array, uint64_t Count, const std::function<int64_t(uint64_t)>& Off,
                           const std::function<std::string(uint64_t)>& String, const std::function<std::string(uint64_t)>& HashName,
                           EntityCounts& N, const std::function<std::string(uint64_t Entity, int32_t ModelIndex)>& Model = {});
    // The mapents kind for a zone's entity text (EntityText's): its entities' pairs, a key's order
    // within its entity alphabetical (IwctxReaders::ExportMapEnts writes the TSV)
    void ExportMapEnts(const DiskGame::Context& C, DiskGame::Totals& T, const std::string& Zone, const std::string& Text);
}
